#include "OcrEngine.h"
#include "PdfCoords.h"
#include <fpdf_edit.h>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QDebug>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

extern QMutex s_pdfiumMutex;

#ifdef FELIXPDF_ENABLE_OCR
#include <tesseract/baseapi.h>

namespace {

#ifdef Q_OS_WIN
// CHI DUNG TREN WINDOWS. Tesseract 4.x (Ubuntu 22.04) va 5.x (vcpkg/Windows)
// khai FileReader KHAC CHU KY:
//   4.1.1: bool(*)(const STRING&, GenericVector<char>*)
//   5.x  : bool(*)(const char*,   std::vector<char>*)
// Tren Linux khong can ham nay: fopen cua glibc von nhan duong dan UTF-8.
// Tesseract doc file traineddata qua con tro ham nay khi duoc cap cho Init.
// Mac dinh Tesseract mo file bang fopen voi chuoi hep the ANSI code page
// (Windows) nen truot khi duong dan chua ky tu ngoai ASCII — rat pho bien
// voi khach hang Viet (C:/Users/Nguyen Van A/...). QFile xu ly Unicode dung
// nen moi file Tesseract can (vie/eng.traineddata, config...) deu doc qua day.
bool ocrFileReader(const char* filename, std::vector<char>* data) {
    if (!filename || !data) return false;
    QFile f(QString::fromUtf8(filename));
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QByteArray bytes = f.readAll();
    if (bytes.isEmpty()) return false;
    data->assign(bytes.constData(), bytes.constData() + bytes.size());
    return true;
}
#endif  // Q_OS_WIN

// Hai bien moi truong duoi day CHI de DO LAI 3 cau hinh tren cung mot ban dung
// (bang so trong REPORT_OCR_QUALITY_R2_2026-08-16.md) va de chan doan tren may
// khach ma khong phai dung lai. Khong dat thi dung mac dinh da chot.
//   TORREADER_OCR_PSM=<0..13>  che do phan trang (3=PSM_AUTO, 11=PSM_SPARSE_TEXT)
//   TORREADER_OCR_UDPI=0       KHONG bao user_defined_dpi (nhu ban truoc khi va)
int ocrPsmSetting() {
    static const int v = []() -> int {
        const QByteArray e = qgetenv("TORREADER_OCR_PSM");
        bool ok = false;
        const int n = e.toInt(&ok);
        // Khoang hop le cua tesseract::PageSegMode: 0..13.
        if (ok && n >= 0 && n <= 13) return n;
        return OcrEngine::kDefaultPsm;
    }();
    return v;
}

bool ocrReportDpi() {
    static const bool v = (qgetenv("TORREADER_OCR_UDPI") != QByteArray("0"));
    return v;
}

} // namespace
#endif

QString OcrEngine::tessdataDir() {
    const QStringList dirs = {
        QString::fromLocal8Bit(qgetenv("TESSDATA_PREFIX")),
        QCoreApplication::applicationDirPath() + QLatin1String("/tessdata"),
        QCoreApplication::applicationDirPath() + QLatin1String("/../resources/tessdata"),
    };
    for (const QString& d : dirs) {
        if (d.isEmpty()) continue;
        // App mac dinh dung langs "vie+eng" nen can DU hai file ngon ngu.
        QFileInfo fe(d + QLatin1String("/eng.traineddata"));
        QFileInfo fv(d + QLatin1String("/vie.traineddata"));
        if (fe.isFile() && fv.isFile()) return QDir::cleanPath(d);
    }
    return QString();
}

bool OcrEngine::available(QString* whyNot) {
#ifdef FELIXPDF_ENABLE_OCR
    if (tessdataDir().isEmpty()) {
        if (whyNot) *whyNot = QStringLiteral(
            "Language data (eng.traineddata / vie.traineddata) not found. "
            "Set TESSDATA_PREFIX or place the files in tessdata/ next to the app.");
        return false;
    }
    return true;
#else
    if (whyNot) *whyNot = QStringLiteral(
        "OCR engine was not built in (Tesseract was not found at build time). "
        "Install Tesseract and rebuild to enable.");
    return false;
#endif
}

QVector<OcrWord> OcrEngine::recognizePage(FPDF_DOCUMENT doc, int pageIndex,
                                          const QString& langs, int dpi,
                                          std::function<bool()> isCancelled) {
    QVector<OcrWord> out;
#ifdef FELIXPDF_ENABLE_OCR
    if (!doc || dpi <= 0) return out;

    QElapsedTimer timer; timer.start();

    // ── Render trang thanh bitmap thang xam (giu PDFium mutex, nhanh) ──────
    double wPt = 0, hPt = 0;
    int rot = 0;
    QPointF box(0.0, 0.0);
    int wPx = 0, hPx = 0, stride = 0;
    int effDpi = dpi; // tinh that o trong khoi mutex duoi day; giu ngoai de con dung khi quy doi px->pt
    std::vector<unsigned char> buf;
    {
        QMutexLocker lock(&s_pdfiumMutex);
        FPDF_PAGE page = FPDF_LoadPage(doc, pageIndex);
        if (!page) return out;
        wPt = FPDF_GetPageWidth(page);
        hPt = FPDF_GetPageHeight(page);
        rot = FPDFPage_GetRotation(page);
        box = pdfBoxOrigin(page);

        // dpi theo KICH THUOC TRANG: trang cang lon thi dpi cang cao, sao cho
        // canh dai cua anh raster dat khoang 4000-6000 px (tran 600 de khong no
        // bo nho). dpi nguoi goi truyen vao chi la can nho tuyet doi (300).
        const double maxEdgePt = qMax(wPt, hPt);
        effDpi = (maxEdgePt > 0.0)
            ? qBound(300, static_cast<int>(std::lround(5000.0 * 72.0 / maxEdgePt)), 600)
            : dpi;

        wPx = qMax(1, static_cast<int>(std::lround(wPt * effDpi / 72.0)));
        hPx = qMax(1, static_cast<int>(std::lround(hPt * effDpi / 72.0)));
        stride = wPx;
        buf.assign(static_cast<size_t>(stride) * hPx, 255);
        FPDF_BITMAP bmp = FPDFBitmap_CreateEx(wPx, hPx, FPDFBitmap_Gray,
                                              buf.data(), stride);
        if (bmp) {
            FPDFBitmap_FillRect(bmp, 0, 0, wPx, hPx, 0xFFFFFFFF);
            FPDF_RenderPageBitmap(bmp, page, 0, 0, wPx, hPx, 0,
                                  FPDF_ANNOT | FPDF_RENDER_LIMITEDIMAGECACHE);
            FPDFBitmap_Destroy(bmp);
        }
        FPDF_ClosePage(page);
        // Mutex PDFium duoc nha ra ngay truoc khi chay Tesseract (co the la giay).
    }

    if (buf.empty()) return out;

    // ── Tesseract: chay NGOAI khoa PDFium ──────────────────────────────────
    // Init kieu "in-memory" (data_size != 0) chi nap duoc MOT traineddata —
    // thu nghiem thuc te voi "vie+eng": ngon ngu thu hai bi "Failed loading
    // language 'eng'" (Tesseract Clear() giua moi ngon ngu roi doc lai file).
    // Nen dung FileReader de Tesseract doc MOI file qua QFile (Unicode-safe),
    // dua duong dan UTF-8; Windows khong con mo file bang fopen ANSI nua.
    tesseract::TessBaseAPI api;
    const int initRc = api.Init(tessdataDir().toUtf8().constData(), 0,
                                langs.toUtf8().constData(),
                                tesseract::OEM_LSTM_ONLY,
                                nullptr, 0, nullptr, nullptr, false,
#ifdef Q_OS_WIN
                                &ocrFileReader);
#else
                                nullptr);
#endif
    if (initRc != 0) return out;
    // Che do phan trang: PSM_AUTO (3) gia dinh trang VAN BAN co bo cuc, con ban
    // ve ky thuat la chu RAI RAC khap trang => PSM_SPARSE_TEXT (11). Da DO ca
    // hai tren trang scan that (bang so trong REPORT_OCR_QUALITY_R2_2026-08-16).
    const int psm = ocrPsmSetting();
    api.SetPageSegMode(static_cast<tesseract::PageSegMode>(psm));
    // Bao dpi THUC SU cua anh vua render cho Tesseract. Khong bao thi Tesseract
    // tu doan do phan giai, doan sai thi bo phan doan dong loai bo chu nho.
    const bool udpi = ocrReportDpi();
    if (udpi)
        api.SetVariable("user_defined_dpi", QByteArray::number(effDpi).constData());
    api.SetVariable("preserve_interword_spaces", "1");
    api.SetImage(buf.data(), wPx, hPx, 1, stride);
    if (api.Recognize(nullptr) != 0) return out;

    tesseract::ResultIterator* it = api.GetIterator();
    if (!it) return out;

    // Quy doi pixel -> point theo dpi THUC SU da render (effDpi), khong phai
    // dpi nguoi goi truyen vao (co the bi tran len 300-600 theo kich thuoc trang).
    const double k = 72.0 / double(effDpi);
    QRectF prevLineBox;
    int lineIndex = 0;
    int droppedLow = 0, droppedEmpty = 0;
    float confMin = 1000.0f, confMax = -1000.0f;
    for (; !it->Empty(tesseract::RIL_WORD); it->Next(tesseract::RIL_WORD)) {
        if (isCancelled && isCancelled()) break;
        const float conf = it->Confidence(tesseract::RIL_WORD);
        if (conf < kMinConf) { ++droppedLow; continue; }

        int left = 0, top = 0, right = 0, bottom = 0;
        if (!it->BoundingBox(tesseract::RIL_WORD, &left, &top, &right, &bottom))
            continue;

        std::unique_ptr<char[]> txt(it->GetUTF8Text(tesseract::RIL_WORD));
        if (!txt) continue;
        QString text = QString::fromUtf8(txt.get()).trimmed();
        if (text.isEmpty()) { ++droppedEmpty; continue; }

        // Hop bao cua DONG (RIL_TEXTLINE) chua tu nay — dung chung cho moi tu.
        QRectF lineBox;
        int ll = 0, lt = 0, lr = 0, lb = 0;
        if (it->BoundingBox(tesseract::RIL_TEXTLINE, &ll, &lt, &lr, &lb)) {
            const QPointF lp1 = dispToPdf(ll * k, lt * k, wPt, hPt, rot, box.x(), box.y());
            const QPointF lp2 = dispToPdf(lr * k, lb * k, wPt, hPt, rot, box.x(), box.y());
            lineBox = QRectF(lp1, lp2).normalized();
        }

        // Quy doi pixel -> point (he DISPLAY, goc tren-trai) roi sang PDF
        // user space (goc duoi-trai, da tinh /Rotate + CropBox origin).
        // Tu kiem: rot=0, box=(0,0) => x = px*72/dpi, y = hPt - py*72/dpi.
        const double xd1 = left * k,  yd1 = top * k;
        const double xd2 = right * k, yd2 = bottom * k;
        const QPointF p1 = dispToPdf(xd1, yd1, wPt, hPt, rot, box.x(), box.y());
        const QPointF p2 = dispToPdf(xd2, yd2, wPt, hPt, rot, box.x(), box.y());

        // Tu cung mot hop dong (doc lien tiep) gop vao cung lineIndex.
        if (lineBox != prevLineBox) {
            if (!prevLineBox.isNull()) ++lineIndex;
            prevLineBox = lineBox;
        }

        OcrWord w;
        w.text      = text;
        w.boxPt     = QRectF(p1, p2).normalized();
        w.conf      = conf;
        w.lineIndex = lineIndex;
        w.lineBoxPt = lineBox;
        out.append(w);
        confMin = qMin(confMin, conf);
        confMax = qMax(confMax, conf);
    }
    delete it;

    // So lieu cho nguoi dung: in ra stdout mot dong (dung khuon chuan de loc),
    // rua bo nho dem truoc khi ket thuc ham.
    // psm/udpi in kem de doc log la biet chay cau hinh nao (khong phai doan).
    qInfo().noquote() << QString::asprintf("[ocr] page=%d pagePt=%.1fx%.1f effDpi=%d px=%dx%d psm=%d udpi=%d words=%d dropped=%d minConf=%.0f confMin=%.0f confMax=%.0f ms=%lld",
           pageIndex, wPt, hPt, effDpi, wPx, hPx, psm, udpi ? 1 : 0, int(out.size()),
           droppedLow + droppedEmpty, kMinConf,
           out.isEmpty() ? 0.0f : confMin, out.isEmpty() ? 0.0f : confMax,
           static_cast<long long>(timer.elapsed()));
#else
    Q_UNUSED(doc); Q_UNUSED(pageIndex); Q_UNUSED(langs); Q_UNUSED(dpi);
    Q_UNUSED(isCancelled);
#endif
    return out;
}
