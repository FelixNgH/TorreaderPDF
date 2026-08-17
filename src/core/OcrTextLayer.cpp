#include "OcrTextLayer.h"
#include <QByteArray>
#include <QFile>
#include <QHash>
#include <QMutex>
#include <QSet>
#include <QDebug>
#include <algorithm>
#include <cstdio>
#include <vector>
#include <fpdf_edit.h>
#include <fpdf_text.h>

extern QMutex s_pdfiumMutex;

// Luu trang nao da OCR theo doc (FPDF_DOCUMENT con hieu luc trong app).
// Bao ve boi s_pdfiumMutex cung luc doc thay doi tai lieu.
static QHash<FPDF_DOCUMENT, QSet<int>>* g_ocrDonePages = nullptr;
static QHash<FPDF_DOCUMENT, FPDF_FONT>* g_fontCache    = nullptr;

// He so hieu chinh co chu: chieu cao hop TU Tesseract (boxPt) ~ chieu cao chu
// thuc tren anh, nhung glyph cua font DejaVu khong kin het 1 em co chu. Do do
// co chu = boxPt * kOcrFontFactor moi ra hop bao PDFium cao dung bang boxPt.
// Do bang --ocr-layer-test voi tu gia (box 10pt): "Hello"=0.784, "Gach"=0.953
// => he so 1.15 dua ca hai ve 0.90 va 1.10 (muc tieu 0.9-1.15).
static constexpr float kOcrFontFactor = 1.15f;

// DO (5 tu dau moi luot): in ty le (chieu cao hop bao chu do PDFium tra ve) /
// (chieu cao hop tu do Tesseract tra ve). Muc tieu ty le nam trong 0.9-1.15.
static int g_ratioCount = 0;

// Che do baseline. Mac dinh: TRUNG VI cho ca dong (xem ghi chu o vong lap).
// TORREADER_OCR_BASELINE_MODE=word tra ve cach cu (moi tu mot baseline =
// boxPt.top() cua chinh no) — chi de DO truoc/sau tren cung mot ban dung.
static bool baselinePerWordMode() {
    static const bool v = (qgetenv("TORREADER_OCR_BASELINE_MODE") == QByteArray("word"));
    return v;
}

// DO baseline: bat bang TORREADER_OCR_BASELINE_PROBE=1. Sau khi chen xong,
// doc lai trang bang FPDFText_* va in bang nghiem thu:
//   dong | tu | boxPt.top() goc | baseline dung | GetRect y-duoi | y-tren
// Kem do lech y-duoi trong CUNG mot dong (tieu chi: <= 0.5 pt).
static bool baselineProbeOn() {
    static const bool v = !qgetenv("TORREADER_OCR_BASELINE_PROBE").isEmpty()
                          && qgetenv("TORREADER_OCR_BASELINE_PROBE") != QByteArray("0");
    return v;
}

// Mot hang cua bang DO baseline.
struct OcrBaselineRow {
    int     line     = -1;
    QString text;
    double  origTop  = 0.0;   // boxPt.top() goc cua rieng tu nay
    double  baseline = 0.0;   // baseline THUC SU dung khi chen
    double  left     = 0.0;   // boxPt.left() — dung de tim lai ky tu dau
    double  fontSize = 0.0;
};

static void ensureRegistries() {
    if (!g_ocrDonePages) g_ocrDonePages = new QHash<FPDF_DOCUMENT, QSet<int>>;
    if (!g_fontCache)    g_fontCache    = new QHash<FPDF_DOCUMENT, FPDF_FONT>;
}

bool OcrTextLayer::pageDone(FPDF_DOCUMENT doc, int pageIndex) {
    QMutexLocker lock(&s_pdfiumMutex);
    ensureRegistries();
    const auto it = g_ocrDonePages->constFind(doc);
    return it != g_ocrDonePages->cend() && it->contains(pageIndex);
}

void OcrTextLayer::forgetDocument(FPDF_DOCUMENT doc) {
    // Goi tu PdfDocument::close() — mutex PDFium DA duoc giu, khong lock lai.
    ensureRegistries();
    g_ocrDonePages->remove(doc);
    auto fit = g_fontCache->find(doc);
    if (fit != g_fontCache->end()) {
        FPDFFont_Close(fit.value());
        g_fontCache->erase(fit);
    }
}

static FPDF_FONT fontFor(FPDF_DOCUMENT doc) {
    // Goi tu insertPage — mutex PDFium DA duoc giu.
    ensureRegistries();
    const auto it = g_fontCache->constFind(doc);
    if (it != g_fontCache->cend()) return it.value();

    QFile f(QStringLiteral(":/fonts/DejaVuSans.ttf"));
    if (!f.open(QIODevice::ReadOnly)) {
        qWarning() << "[ocrlayer] cannot open :/fonts/DejaVuSans.ttf";
        return nullptr;
    }
    const QByteArray data = f.readAll();
    // cid=1: Type0/Identity-H + ToUnicode tu sinh. cid=0 lam huong ky tu
    // Viet (da do: Gạch ra Gÿch) — nguoc lai muc tieu "phu du tieng Viet" cua spec.
    FPDF_FONT font = FPDFText_LoadFont(
        doc, reinterpret_cast<const uint8_t*>(data.constData()),
        static_cast<uint32_t>(data.size()), FPDF_FONT_TRUETYPE, /*cid=*/1);
    if (font) g_fontCache->insert(doc, font);
    else qWarning() << "[ocrlayer] FPDFText_LoadFont failed";
    return font;
}

int OcrTextLayer::insertPage(FPDF_DOCUMENT doc, int pageIndex,
                             const QVector<OcrWord>& words) {
    if (!doc || words.isEmpty()) return 0;

    QMutexLocker lock(&s_pdfiumMutex);
    ensureRegistries();
    QSet<int>& done = (*g_ocrDonePages)[doc];
    if (done.contains(pageIndex)) return 0;   // trang da OCR — khong lam hai lan

    FPDF_FONT font = fontFor(doc);
    if (!font) return 0;

    FPDF_PAGE page = FPDF_LoadPage(doc, pageIndex);
    if (!page) return 0;

    int inserted = 0;
    int i = 0;
    const int n = words.size();
    const bool perWord  = baselinePerWordMode();
    const bool probeOn  = baselineProbeOn();
    QVector<OcrBaselineRow> probeRows;
    while (i < n) {
        // Co chu CHUNG cho ca dong. Chieu cao DONG (lineBoxPt) ma Tesseract tra
        // ve bao gom ca phan tren/duoi cua dong, lon hon chieu cao chu thuc te —
        // lay nguyen thi o highlight to hon chu nhieu. Suy co chu tu chieu cao
        // TRUNG BINH cua cac hop TU trong dong (sieu gan chieu cao chu thuc),
        // nhan he so hieu chinh font de hop bao PDFium khop chu tren anh scan.
        const int line = words[i].lineIndex;
        double lineWordH = words[i].boxPt.isValid() ? words[i].boxPt.height() : 0.0;
        {
            int cnt = 0; double sumH = 0;
            for (int j = i; j < n && words[j].lineIndex == line; ++j) {
                if (words[j].boxPt.isValid() && words[j].boxPt.height() > 0) {
                    sumH += words[j].boxPt.height();
                    ++cnt;
                }
            }
            if (cnt > 0) lineWordH = sumH / cnt;
        }
        const float fontSize = static_cast<float>(qMax(0.5, lineWordH * kOcrFontFactor));

        // Baseline CHUNG cho ca dong. Hop TU cua Tesseract bao KHIT chu nen day
        // hop (boxPt.top() trong he PDF y-up = day chu) KHONG phai duong co so:
        //   TRICH — chu hoa, dau sac o TREN  => day hop = dung baseline
        //   DOAN  — dau nang o DUOI chu A    => day hop THAP HON baseline
        // Neu moi tu tu dat baseline rieng thi hai tu cung hang nam o hai cao do
        // khac nhau => FPDFText_GetRect tra ve hai o highlight cao thap. Sua:
        // lay TRUNG VI cua boxPt.top() cua MOI tu trong dong roi dung chung.
        // Trung vi (khong phai trung binh) vi vai tu co dau duoi keo lech trung
        // binh, con trung vi mien nhiem khi DA SO tu trong dong khong co dau duoi.
        // KHONG dung lineBoxPt.top(): hop DONG cua Tesseract bao ca phan tren/duoi
        // (ghi chu ngay ben tren) nen o highlight se to hon chu.
        float lineBaseline = static_cast<float>(words[i].boxPt.top());
        {
            std::vector<double> tops;
            tops.reserve(static_cast<size_t>(n - i));
            for (int j = i; j < n && words[j].lineIndex == line; ++j)
                if (words[j].boxPt.isValid()) tops.push_back(words[j].boxPt.top());
            if (!tops.empty()) {
                std::sort(tops.begin(), tops.end());
                // Phan tu giua. Voi so tu CHAN lay phan tu TREN trong hai phan tu
                // giua (chi so size/2): dau duoi chi keo top() XUONG chu khong bao
                // gio day len, nen lech ve phia TREN moi la huong an toan.
                lineBaseline = static_cast<float>(tops[tops.size() / 2]);
            }
        }

        for (; i < n && words[i].lineIndex == line; ++i) {
            const OcrWord& w = words[i];
            FPDF_PAGEOBJECT obj = FPDFPageObj_CreateTextObj(doc, font, fontSize);
            if (!obj) continue;

            // Text UTF-16LE cho FPDFText_SetText.
            const QString text = w.text;
            if (!FPDFText_SetText(obj,
                                  reinterpret_cast<FPDF_WIDESTRING>(text.utf16()))) {
                FPDFPageObj_Destroy(obj);
                continue;
            }

            // Ve chu = VÔ HÌNH. KHONG dung "to mau trang" — chu trang van che noi
            // dung khi in / nen khong trang.
            if (!FPDFTextObj_SetTextRenderMode(obj, FPDF_TEXTRENDERMODE_INVISIBLE)) {
                qWarning() << "[ocrlayer] SetTextRenderMode INVISIBLE failed";
            }
            // Lop bao thu 2 (PDFium ban bblanchon tren Windows khong ton trong
            // SetTextRenderMode): mau voi ALPHA = 0 — trong suot hoan toan, van
            // tim duoc boi search/CountChars, in ra khong muc. To trang thi che — CAM.
            if (!FPDFPageObj_SetFillColor(obj, 0, 0, 0, 0))
                qWarning() << "[ocrlayer] SetFillColor alpha=0 failed";
            if (!FPDFPageObj_SetStrokeColor(obj, 0, 0, 0, 0))
                qWarning() << "[ocrlayer] SetStrokeColor alpha=0 failed";

            // Dat tai boxPt (PDF user space, goc duoi-trai). Diem goc cua text obj
            // nam o ben trai duong co so (baseline left) => e=left, f=baseline.
            // Baseline = TRUNG VI day hop tu cua CA DONG (lineBaseline) — CHUNG
            // cho moi tu trong dong nen tu co dau duoi (ạ ẹ ọ ụ ị) khong con tut
            // xuong so voi tu khong dau. Toa do NGANG van rieng tung tu (left).
            // Da DO (TORREADER_OCR_BASELINE_K 0.0/0.1/0.22/0.3): he so day
            // baseline xuong cang lon thi PDFium center cang lech khoi Tesseract
            // center (0.22 -> 12-30% lineH, 0.0 -> 0.9-9.5%) => khong day.
            const float baselineY = perWord ? static_cast<float>(w.boxPt.top())
                                            : lineBaseline;
            FS_MATRIX m{1.0f, 0.0f, 0.0f, 1.0f,
                        static_cast<float>(w.boxPt.left()),
                        baselineY};
            if (!FPDFPageObj_SetMatrix(obj, &m)) {
                FPDFPageObj_Destroy(obj);
                continue;
            }
            FPDFPage_InsertObject(page, obj);
            if (probeOn) {
                OcrBaselineRow r;
                r.line     = line;
                r.text     = w.text;
                r.origTop  = w.boxPt.top();
                r.baseline = double(baselineY);
                r.left     = w.boxPt.left();
                r.fontSize = double(fontSize);
                probeRows.append(r);
            }
            // DO: ty le chieu cao hop bao PDFium (GetBounds) / hop tu Tesseract
            // (boxPt) + do lech TAM DOC. In cho 5 tu dau de chinh kOcrFontFactor.
            if (g_ratioCount < 5 && w.boxPt.isValid()) {
                float bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;
                if (FPDFPageObj_GetBounds(obj, &bx0, &by0, &bx1, &by1)) {
                    const double pdfH = qAbs(double(by1) - by0);
                    const double tessH = w.boxPt.height();
                    // Tam doc: hop Tesseract (PDF coords, y-up) vs hop PDFium sau khi chen.
                    const double tessCenterY = (w.boxPt.top() + w.boxPt.bottom()) / 2.0;
                    const double pdfCenterY  = (double(by0) + double(by1)) / 2.0;
                    const double centerDev   = pdfCenterY - tessCenterY;
                    const double lineH       = w.lineBoxPt.isValid() ? w.lineBoxPt.height() : tessH;
                    const double devPct      = lineH > 0.0 ? 100.0 * centerDev / lineH : -1.0;
                    ++g_ratioCount;
                    // qInfo (khong phai fprintf(stdout)): app Windows la GUI
                    // khong co console nen stdout mat trang — qua log handler
                    // moi vao %TEMP%\torreader.log duoc.
                    qInfo().noquote()
                        << QString::asprintf(
                               "[ocrratio] \"%s\" font=%.2f tessH=%.2f pdfH=%.2f ratio=%.3f "
                               "tessCenterY=%.2f pdfCenterY=%.2f dev=%.3f (%.1f%% lineH, signed)",
                               w.text.toUtf8().constData(),
                               double(fontSize), tessH, pdfH,
                               tessH > 0.0 ? pdfH / tessH : -1.0,
                               tessCenterY, pdfCenterY, centerDev, devPct);
                }
            }
            ++inserted;
        }
    }

    if (inserted > 0)
        FPDFPage_GenerateContent(page);

    // ── DO baseline (TORREADER_OCR_BASELINE_PROBE=1) ────────────────────────
    // Doc lai trang qua FPDFText_* — chinh duong ma search/select di qua — roi
    // in bang: dong | tu | boxPt.top() goc | baseline dung | y-duoi | y-tren.
    // y-duoi/y-tren lay tu FPDFText_GetRect cho DUNG doan ky tu cua tu do (giong
    // TextSearch.cpp: CountRects(charIdx, charCount) roi GetRect(0)).
    if (probeOn && inserted > 0 && !probeRows.isEmpty()) {
        FPDF_TEXTPAGE tp = FPDFText_LoadPage(page);
        if (tp) {
            const int nChars = FPDFText_CountChars(tp);
            // Tim ky tu DAU cua tung tu bang GOC KY TU (FPDFText_GetCharOrigin)
            // chu KHONG do chu: ToUnicode co the tra ve ky tu khac, va mot tu
            // co the xuat hien nhieu lan trong trang. Goc ky tu dau = dung
            // (boxPt.left(), baseline) da dat vao FS_MATRIX nen doi chieu la ra.
            std::vector<double> ox(static_cast<size_t>(qMax(0, nChars)), 0.0);
            std::vector<double> oy(static_cast<size_t>(qMax(0, nChars)), 0.0);
            for (int c = 0; c < nChars; ++c)
                FPDFText_GetCharOrigin(tp, c, &ox[size_t(c)], &oy[size_t(c)]);
            qInfo().noquote() << QString::asprintf(
                "[ocrbase] page=%d mode=%s rows=%d chars=%d",
                pageIndex, perWord ? "word" : "median",
                int(probeRows.size()), nChars);
            qInfo().noquote() << QStringLiteral(
                "[ocrbase] line | word | origTop | baselineUsed | rectYbottom | rectYtop");
            int curLine = -1;
            double lineLo = 1e18, lineHi = -1e18;
            int lineWords = 0;
            auto flushLine = [&]() {
                if (curLine < 0 || lineWords <= 0) return;
                qInfo().noquote() << QString::asprintf(
                    "[ocrbase] line=%d words=%d yBottomSpread=%.3f pt -> %s (tieu chi <= 0.5)",
                    curLine, lineWords, lineHi - lineLo,
                    (lineHi - lineLo) <= 0.5 ? "DAT" : "KHONG DAT");
            };
            for (const OcrBaselineRow& r : probeRows) {
                if (r.line != curLine) {
                    flushLine();
                    curLine = r.line; lineLo = 1e18; lineHi = -1e18; lineWords = 0;
                }
                double rl = 0, rt = 0, rr = 0, rb = 0;
                bool got = false;
                int at = -1;
                double best = 1e18;
                for (int c = 0; c < nChars; ++c) {
                    const double d = qAbs(ox[size_t(c)] - r.left)
                                   + qAbs(oy[size_t(c)] - r.baseline);
                    if (d < best) { best = d; at = c; }
                }
                if (at >= 0 && best <= 0.05) {
                    // Giong TextSearch.cpp: dem rect cho DUNG doan ky tu cua tu
                    // nay roi lay rect dau — day la o highlight nguoi dung thay.
                    if (FPDFText_CountRects(tp, at, r.text.size()) > 0)
                        got = FPDFText_GetRect(tp, 0, &rl, &rt, &rr, &rb) != 0;
                }
                if (got) {
                    lineLo = qMin(lineLo, rb);
                    lineHi = qMax(lineHi, rb);
                    ++lineWords;
                }
                qInfo().noquote() << QString::asprintf(
                    "[ocrbase] %d | %s | %.3f | %.3f | %s | %s",
                    r.line, r.text.toUtf8().constData(), r.origTop, r.baseline,
                    got ? QString::asprintf("%.3f", rb).toUtf8().constData() : "n/a",
                    got ? QString::asprintf("%.3f", rt).toUtf8().constData() : "n/a");
            }
            flushLine();
            FPDFText_ClosePage(tp);
        } else {
            qWarning() << "[ocrbase] FPDFText_LoadPage failed";
        }
    }

    FPDF_ClosePage(page);

    if (inserted > 0) done.insert(pageIndex);
    return inserted;
}
