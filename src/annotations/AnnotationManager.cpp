#include "AnnotationManager.h"
#include "../core/PdfCoords.h"
#include "../core/PageCache.h"
#include "../core/PdfiumLock.h"
#include "../core/OwnAnnotHideGuard.h"
#include <fpdf_edit.h>
#include <fpdf_save.h>
// 📐 NẤC 1 (0921): PDFium khong ghi duoc /AP stream cho FreeText (bug #1381,
// FPDFAnnot_SetAP tra false — da do). Phai ghi lai /AP o tang FILE bang QPDF.
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFWriter.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/Buffer.hh>
#include <qpdf/QPDFObjectHelper.hh>
#include <QSet>
#include <QVariant>
#include <QHash>
#include <QRegularExpression>
#include <QMutex>
#include <QFile>
#include <QStringList>
#include <QFontMetricsF>
#include <QVector>
#include <vector>
#include <cstring>
#include <algorithm>
#include <cmath>
#include <QDateTime>
#include <QFileInfo>
#include <stdexcept>
#include <QElapsedTimer>
#include <QThread>
#include <QCoreApplication>
#include <QDebug>
#include <QUuid>
#include <QRawFont>
#include <QPainter>
#include <QFontDatabase>
// 🔴 0902: QPainter/QFontDatabase keo windows.h vao => macro min/max cua Windows
// nuot std::min/std::max ben duoi (error C2589 'illegal token on right side of ::').
// Go macro ngay sau include — KHONG XOA.
#ifdef _WIN32
#  undef min
#  undef max
#endif

// Unicode -> ma glyph Identity-H (4 hex hoa/glyph) bang QRawFont doc dung tep TTF
// ma unicodeFont() nap — chi so glyph khớp với phông nhúng trong PDF.
// Tra ve rong khi khong map duoc (rong/toan 0) — caller KHONG duoc ghi bừa.
static QString glyphHexIdentityH(const QByteArray& ttfData, const QString& text) {
    if (text.isEmpty()) return QString();
    QRawFont rf(ttfData, 16.0);
    if (!rf.isValid()) return QString();
    const QList<quint32> gids = rf.glyphIndexesForString(text);
    if (gids.isEmpty()) return QString();
    QString hex;
    bool any = false;
    for (quint32 g : gids) {
        if (g > 0xFFFF) g = 0;
        if (g) any = true;
        hex += QString("%1").arg(g, 4, 16, QChar('0')).toUpper();
    }
    if (!any) return QString();
    return hex;
}

QFont trDejaVuFontAtPixelSize(double px);   // dinh nghia o duoi (245)

// ── 📐 NẤC 1 (0921): vá /AP cho FreeText NGOÀI ────────────────────────────────
// PDFium khong ghi duoc /AP stream cho FreeText (FPDFAnnot_SetAP tra false).
// Cach lam: luc retext chi danh dau /TR_AP_REBUILD + sua /Contents; luc save mo
// lai tep bang QPDF, giu NGUYEN khung nen/vien/mui ten (prefix+suffix), chi thay
// khoi BT..ET bang chu DejaVu ngat dong lai. /AP goc duoc luu vao /TR_AP_ORIG.

// 🔴 0927 LƯỢT 3: khoá duy nhất cho nội dung /AP của FreeText CỦA TA.
// Dùng chung lúc ghi bộ nhớ (createInlineNote_locked) và lúc vá file
// (patchOwnNoteAp) ⇒ hai vế luôn khớp, không lệch vì cách viết chuỗi.
static QString ownApKey(int pageIndex, const QString& trid) {
    return QString::number(pageIndex) + QLatin1Char(':') + trid;
}

// Lay font DejaVu (Type0/Identity-H) PDFium da nhung vao /Resources cua trang.
static QPDFObjectHandle findDejaVuFont(QPDFObjectHandle page) {
    QPDFObjectHandle res = page.getKey("/Resources");
    if (!res.isDictionary()) return QPDFObjectHandle();
    QPDFObjectHandle fonts = res.getKey("/Font");
    if (!fonts.isDictionary()) return QPDFObjectHandle();
    for (auto const& kv : fonts.getDictAsMap()) {
        QPDFObjectHandle f = kv.second;
        if (!f.isDictionary() || !f.hasKey("/BaseFont")) continue;
        if (f.getKey("/BaseFont").getName().find("DejaVuSans") != std::string::npos)
            return f;
    }
    return QPDFObjectHandle();
}

// ── 📐 NẤC 1 (0921, TRỘN 2 FONT) ─────────────────────────────────────────────
// Trong tệp thật: /FXF1 = /ArialMT TrueType WinAnsiEncoding KHÔNG nhúng; /FXF2 là
// Type0 subset. Vậy ký tự WinAnsi biểu diễn được ghi thẳng bằng /FXF1 (giữ đúng
// tên + cách khai như /AP gốc, dáng chữ y hệt bản gốc), chỉ ký tự NGOÀI WinAnsi
// mới cần DejaVu nhúng (/FTR). Đo bề rộng: /Widths của chính font ArialMT (đúng
// như Acrobat dùng) cho đoạn WinAnsi, QFontMetricsF của DejaVu cho đoạn còn lại.

// Unicode -> byte CP1252 (WinAnsi). -1 nếu không biểu diễn được.
static int winAnsiByteFor(char16_t u) {
    if (u < 0x80 || (u >= 0xA0 && u <= 0xFF)) return u;
    switch (u) {
        case 0x20AC: return 0x80; case 0x201A: return 0x82; case 0x0192: return 0x83;
        case 0x201E: return 0x84; case 0x2026: return 0x85; case 0x2020: return 0x86;
        case 0x2021: return 0x87; case 0x02C6: return 0x88; case 0x2030: return 0x89;
        case 0x0160: return 0x8A; case 0x2039: return 0x8B; case 0x0152: return 0x8C;
        case 0x017D: return 0x8E; case 0x2018: return 0x91; case 0x2019: return 0x92;
        case 0x201C: return 0x93; case 0x201D: return 0x94; case 0x2022: return 0x95;
        case 0x2013: return 0x96; case 0x2014: return 0x97; case 0x02DC: return 0x98;
        case 0x2122: return 0x99; case 0x0161: return 0x9A; case 0x203A: return 0x9B;
        case 0x0153: return 0x9C; case 0x017E: return 0x9E; case 0x0178: return 0x9F;
        default: return -1;
    }
}

// Bảng /Widths (1/1000 em) của font WinAnsi gốc — đo y hệt Acrobat, không cần tệp font.
struct WinAnsiMetrics {
    bool valid = false;
    QHash<int,double> w;   // byte CP1252 -> bề rộng /1000
    double widthOf(QChar c, double fs) const {   // <0 = không dùng font này
        if (!valid) return -1.0;
        const int b = winAnsiByteFor(c.unicode());
        if (b < 0) return -1.0;
        auto it = w.constFind(b);
        if (it == w.constEnd() || it.value() <= 0) return -1.0;
        return it.value() / 1000.0 * fs;   // /Widths = 1/1000 em
    }
};

static WinAnsiMetrics readWinAnsiMetrics(QPDFObjectHandle font) {
    WinAnsiMetrics m;
    if (!font.isDictionary()) return m;
    QPDFObjectHandle fc = font.getKey("/FirstChar");
    QPDFObjectHandle ws = font.getKey("/Widths");
    if (!fc.isInteger() || !ws.isArray()) return m;
    const int first = fc.getIntValueAsInt();
    auto vec = ws.getArrayAsVector();
    for (size_t i = 0; i < vec.size(); ++i) {
        if (!vec[i].isNumber()) continue;
        m.w.insert(first + static_cast<int>(i), vec[i].getNumericValue());
    }
    m.valid = !m.w.isEmpty();
    return m;
}

// 🔴 P2 (0921): chi coi la WinAnsi khi /Encoding NOI RO là WinAnsiEncoding va
// KHONG co /Differences. Gap MacRoman / co /Differences ⇒ byte CP1252 ra glyph
// SAI ma khong bao ⇒ nguy hiem hon la roi ve DejaVu (dung doan).
static bool isConfidentWinAnsi(QPDFObjectHandle f) {
    QPDFObjectHandle enc = f.getKey("/Encoding");
    if (enc.isName()) return enc.getName() == "/WinAnsiEncoding";
    if (enc.isDictionary()) {
        if (enc.hasKey("/Differences")) return false;
        QPDFObjectHandle base = enc.getKey("/BaseEncoding");
        return base.isName() && base.getName() == "/WinAnsiEncoding";
    }
    return false;
}

// Font WinAnsi trong /Resources của /AP gốc (ưu tiên tên /FXF1 như tệp thật).
static QPDFObjectHandle pickWinAnsiFont(QPDFObjectHandle apN, QString& outName) {
    outName.clear();
    QPDFObjectHandle res = apN.getDict().getKey("/Resources");
    if (!res.isDictionary()) return QPDFObjectHandle();
    QPDFObjectHandle fonts = res.getKey("/Font");
    if (!fonts.isDictionary()) return QPDFObjectHandle();
    QPDFObjectHandle fallback;
    for (auto const& kv : fonts.getDictAsMap()) {
        QPDFObjectHandle f = kv.second;
        if (!readWinAnsiMetrics(f).valid) continue;
        if (!isConfidentWinAnsi(f)) continue;   // khong chac WinAnsi → DejaVu
        if (kv.first == "/FXF1") { outName = QStringLiteral("/FXF1"); return f; }
        if (!fallback.isInitialized()) {
            fallback = f;
            outName = QString::fromStdString(kv.first);
        }
    }
    if (!fallback.isInitialized()) outName.clear();
    return fallback;
}

// Bề rộng trộn: WinAnsi -> /Widths Arial, còn lại -> QFontMetricsF DejaVu.
static double mixedAdvance(const QString& s, const WinAnsiMetrics& wm,
                           double fs, const QFontMetricsF& fm) {
    double total = 0.0;
    for (int i = 0; i < s.size(); ++i) {
        double a = wm.widthOf(s.at(i), fs);
        if (a < 0) a = fm.horizontalAdvance(QString(s.at(i)));
        total += a;
    }
    return total;
}

struct ApRun { bool dv; QString text; };

// 🔎 0927 LUOT 7: so do do khi dung /AP cua FreeText NGOAI.
// innerW  = be rong that cua vung chu (be rong HOP tru le trai)
// innerH  = chieu cao that cua vung chu cho N dong
// textH   = chieu cao N x buoc dong
// delta   = phai GIAN them bao nhieu xuong duoi (0 = k duong)
// expanded= 1 neu da doi khung, 0 neu giu nguyen byte cho byte
struct ApFit {
    double innerW = 0.0;
    double innerH = 0.0;
    double textH  = 0.0;
    double maxLineW = 0.0;
    double bboxH   = 0.0;
    double bboxY0  = 0.0;   // y cua can duoi /BBox (thuong 0)
    double delta   = 0.0;
    double nLines  = 0.0;
    bool   expanded = false;
    bool   measurable = false;   // 0 = khong do duoc (khong tim thay BBox/re)
    double rectBefore[4] = {0, 0, 0, 0};
    double rectAfter[4]  = {0, 0, 0, 0};
};

static QList<ApRun> splitApRuns(const QString& s, const WinAnsiMetrics& wm, double fs) {
    QList<ApRun> runs;
    bool curDv = false;
    QString cur;
    for (int i = 0; i < s.size(); ++i) {
        const bool dv = wm.widthOf(s.at(i), fs) < 0;
        if (!cur.isEmpty() && dv != curDv) { runs.append({curDv, cur}); cur.clear(); }
        curDv = dv; cur += s.at(i);
    }
    if (!cur.isEmpty()) runs.append({curDv, cur});
    return runs;
}

// Chuoi literal PDF cho font WinAnsi: escape () \ và ghi byte CP1252.
static QByteArray winAnsiLiteral(const QString& s) {
    QByteArray b;
    b += '(';
    for (QChar c : s) {
        int v = winAnsiByteFor(c.unicode());
        if (v < 0) v = int('?');
        const char ch = static_cast<char>(v);
        if (ch == '(' || ch == ')' || ch == '\\') b += '\\';
        b += ch;
    }
    b += ')';
    return b;
}

// Ngat dong: chuan hoa \r\n, \r -> \n TRUOC (n la ngat dong CUNG, khong phai
// glyph — \r de lai se ra notdef 0000); bo moi ky tu dieu khien khac.
static QStringList wrapMixedLines(const QString& text, double wrapW,
                                  const WinAnsiMetrics& wm, double fs,
                                  const QFontMetricsF& fm) {
    QString t = text;
    t.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    t.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    QString norm;
    norm.reserve(t.size());
    for (QChar c : t) {
        const char16_t u = c.unicode();
        // P2 (0921): ky tu dieu cong lot vao nhanh DejaVu se ra glyph notdef.
        if (u == 0x2028 || u == 0x2029) { norm += QLatin1Char('\n'); continue; }  // phan dong
        if (u == 0x7F) continue;                       // DEL
        if (u >= 0x80 && u <= 0x9F) continue;          // C1 controls
        if (u >= 0x200B && u <= 0x200F) continue;      // zero-width / dau huong
        if (u == 0xFEFF) continue;                     // BOM (hay gap khi dan tu Word)
        if (u < 0x20 && u != '\n') continue;
        norm += c;
    }

    QStringList lines;
    for (const QString& para : norm.split(QLatin1Char('\n'))) {
        if (para.isEmpty()) { lines << QString(); continue; }
        QString cur;
        const QStringList words = para.split(QLatin1Char(' '));
        for (const QString& w : words) {
            const QString cand = cur.isEmpty() ? w : cur + QLatin1Char(' ') + w;
            if (cur.isEmpty() || mixedAdvance(cand, wm, fs, fm) <= wrapW) cur = cand;
            else { lines << cur; cur = w; }
        }
        while (mixedAdvance(cur, wm, fs, fm) > wrapW && cur.size() > 1) {
            int cut = cur.size() - 1;
            while (cut > 1 && mixedAdvance(cur.left(cut), wm, fs, fm) > wrapW) --cut;
            lines << cur.left(cut);
            cur = cur.mid(cut);
        }
        lines << cur;
    }
    return lines;
}

static bool buildForeignApContent(const QByteArray& oldRaw, const QString& rawText,
                                  const QByteArray& ttfData, int align,
                                  const WinAnsiMetrics& wm, const QString& winName,
                                  QByteArray& out, ApFit* fit = nullptr,
                                  bool allowGrow = true) {
    const QString s = QString::fromLatin1(oldRaw);
    static const QRegularExpression reBT(QStringLiteral("\\bBT\\b"));
    static const QRegularExpression reET(QStringLiteral("\\bET\\b\\s*EMC"));
    const auto mBT = reBT.match(s);
    if (!mBT.hasMatch()) return false;
    auto mET = reET.match(s, mBT.capturedEnd());
    if (!mET.hasMatch()) return false;
    const int iBT = mBT.capturedStart();
    const int iET = mET.capturedStart();   // dau token ET (giu lai trong suffix)
    const QString prefix = s.left(iBT);
    const QString block  = s.mid(iBT, iET - iBT);
    const QString suffix = s.mid(iET + 2); // bo qua chinh "ET" (body da co ET)

    // vung cat chu (clip) trong prefix: <x> <y> <w> <h> re ngay sau /Tx BMC
    const int bmc = prefix.indexOf(QLatin1String("/Tx BMC"));
    double tx = 0, tw = 0;
    int reAt = -1, reLen = 0;          // vi tri lenh `re` cua hop nen trong prefix
    double rx = 0, ry = 0, rw = 0, rh = 0;
    {
        static const QRegularExpression reRe(
            QStringLiteral("([-\\d.]+)\\s+([-\\d.]+)\\s+([-\\d.]+)\\s+([-\\d.]+)\\s+re"));
        const QString scan = bmc >= 0 ? prefix.mid(bmc) : prefix;
        const int scanOff = bmc >= 0 ? bmc : 0;
        auto it = reRe.globalMatch(scan);
        while (it.hasNext()) {
            auto m = it.next();
            tx = m.captured(1).toDouble(); tw = m.captured(3).toDouble();
            rx = m.captured(1).toDouble(); ry = m.captured(2).toDouble();
            rw = m.captured(3).toDouble(); rh = m.captured(4).toDouble();
            reAt = scanOff + m.capturedStart();
            reLen = m.capturedLength();
        }
    }

    // tham so chu cu: mau (rg), co (Tf), moc dong dau (Td), buoc dong
    static const QRegularExpression reRG(QStringLiteral("([\\d.]+)\\s+([\\d.]+)\\s+([\\d.]+)\\s+rg"));
    double cr = 0, cg = 0, cb = 0;
    if (auto m = reRG.match(block); m.hasMatch()) {
        cr = m.captured(1).toDouble(); cg = m.captured(2).toDouble(); cb = m.captured(3).toDouble();
    }
    static const QRegularExpression reTf(QStringLiteral("([-\\d.]+)\\s+Tf"));
    double fs = 15.0;
    if (auto m = reTf.match(block); m.hasMatch()) fs = m.captured(1).toDouble();
    static const QRegularExpression reTd(QStringLiteral("([-\\d.]+)\\s+([-\\d.]+)\\s+(?:Td|TD)\\b"));
    double x0 = 0, y0 = 0, lead = 0;
    {
        bool first = true;
        auto it = reTd.globalMatch(block);
        while (it.hasNext()) {
            auto m = it.next();
            const double a = m.captured(1).toDouble(), b = m.captured(2).toDouble();
            if (first) { x0 = a; y0 = b; first = false; }
            else if (lead <= 0 && b < 0) lead = -b;
        }
    }
    if (lead <= 0) lead = fs * 1.16;
    if (tw <= 1.0) tw = 100.0;

    // 🔎 0927 LUOT 7 LOI 1: BE RONG NGAT DONG PHAI LA BE RONG VUNG CHU THAT,
    // KHONG PHAI be rong cua ca HOP. Chu bat dau tai x0 (le trai) nen ve that chi
    // con tw - (x0 - tx). Do do moi 190pt cho vi du (200 - 10): dong 3 rong
    // 198.79pt se tran va bi cat mep phai neu ngat theo 200pt.
    const double innerW = (tw - (x0 - tx) > 1.0) ? (tw - (x0 - tx)) : tw;

    const QFontMetricsF fm(trDejaVuFontAtPixelSize(fs));
    const QStringList lines = wrapMixedLines(rawText, innerW, wm, fs, fm);

    // 🔎 0927 LUOT 7 LOI 2: khi nhieu dong hon, khop cua /BBox giu nguyen chieu
    // cao goc (thuong 1 dong) nen dong cuoi tran ra ngoai va bi cat mep duoi.
    // GIAN KHUNG XUONG DUOI, giu mep tren va mep trai (khong thu nho co chu).
    // innerH = (n-1)*lead + ascent + descent, xem /AP hien tai de biet can nao cao.
    // Vung chu thuc te: mep tren = y0 + ascent, mep duoi = y0 - (n-1)*lead - descent
    // => innerH = (y0+asc) - (y0-(n-1)*lead-desc) = (n-1)*lead + asc + desc.
    // 🟢 0927 LUOT 7 (sau review): ascent/descent LAY TU QFontMetricsF CUA
    // CHINH FONT (DejaVu that 0.928/0.236 em). Hang so 0.80/0.22 se gian THIEU
    // ~0.13fs va lam descender cua dong cuoi van bi cat.
    const double ascF = (std::abs(fm.ascent())  > 0.01) ? std::abs(fm.ascent())  : fs * 0.928;
    const double dscF = (std::abs(fm.descent()) > 0.01) ? std::abs(fm.descent()) : fs * 0.236;
    const double nLines  = static_cast<double>(lines.size());
    const double textH   = nLines * lead;
    const double innerH  = (nLines - 1.0) * lead + ascF + dscF;
    if (fit) {
        fit->innerW = innerW;
        fit->innerH = innerH;
        fit->textH  = textH;
        fit->nLines = nLines;
    }

    QByteArray body;
    body += "BT\n";
    body += QString(" %1 %2 %3 rg\n").arg(cr,0,'f',6).arg(cg,0,'f',6).arg(cb,0,'f',6).toUtf8();
    double prevLineX = x0;
    for (int i = 0; i < lines.size(); ++i) {
        const QString& ln = lines[i];
        const QList<ApRun> runs = splitApRuns(ln, wm, fs);
        const double lineW = mixedAdvance(ln, wm, fs, fm);
        double x = x0;
        if (align == 1)      x = tx + (tw - lineW) / 2.0;
        else if (align == 2) x = tx + tw - lineW;
        if (i == 0) body += QString("%1 %2 Td\n").arg(x,0,'f',3).arg(y0,0,'f',3).toUtf8();
        else        body += QString("%1 %2 Td\n").arg(x - prevLineX,0,'f',3).arg(-lead,0,'f',3).toUtf8();
        // Tj tu day text matrix theo be rong cua CHINH font vua dung => khong Td giua run.
        for (const ApRun& r : runs) {
            if (r.dv) {
                body += QString("/FTR %1 Tf\n").arg(fs,0,'f',4).toUtf8();
                const QString hex = glyphHexIdentityH(ttfData, r.text);
                if (!hex.isEmpty()) body += QString("<%1> Tj\n").arg(hex).toUtf8();
            } else {
                body += QString("%1 %2 Tf\n").arg(winName).arg(fs,0,'f',4).toUtf8();
                body += winAnsiLiteral(r.text);
                body += " Tj\n";
            }
        }
        prevLineX = x;
    }
    body += "ET";

    // 🔎 0927 LUOT 7 LOI 2 (tien hanh): GIAN HOP XUONG DUOI neu chu cao hon
    // khop cu. Giu mep tren + mep trai; KHONG thu nho co chu; KHONG dung /CL.
    // Chi lam khi /Matrix la don vi (khong xoay/scale) — neu xoay thi bo qua.
    double maxLineW = 0.0;
    for (const QString& ln : lines) {
        const double lw = mixedAdvance(ln, wm, fs, fm);
        if (lw > maxLineW) maxLineW = lw;
    }
    const double bboxH = rh;      // chieu cao hop nen trong content stream
    // 🔎 LOI QUAN TRONG: khong du la CHIEU CAO khop du, chu van co the TRAN
    // doc khop. y0 la baseline dong DAU tinh tu can duoi /BBox; dong cuoi nam tai
    // y0 - (n-1)*lead. Chu tran xuong duoi khi can duoi do < can duoi cua /BBox.
    // (Vi du that: y0=36, n=4, lead=13.92 => can duoi chu = -8.40, /BBox can = 0
    //  => tran 8.40pt duoi. Chi so sanh CHIEU CAO se SAI: innerH=54 < 60.)
    const double textBottom = y0 - (nLines - 1.0) * lead - dscF;
    // 🔎 0927 LUOT 7: DAU PHAI LA (can duoi hop) - (can duoi chu).
    // Vi du: can duoi chu = -8.59, can duoi hop (/BBox ry) = 0 ⇒ can ha them
    // 0 - (-8.59) = +8.59pt. Go (can duoi chu) - (can duoi hop) = -8.59 se
    // SAI DAU ⇒ khong bao gio gian ⇒ loi cat mep duoi con nguyên.
    const double needGrow  = ry - textBottom;   // ry = can duoi cua hop nen
    // allowGrow do BEN GOI truyen vao: chi 1 khi /Matrix cua /AP la DON VI
    // (khong xoay/scale). Ben goi KHONG doc duoc /Matrix o day (content stream
    // khong co dict) nen KHONG do la bang cach doan.
    const bool canGrow = allowGrow && (reAt >= 0) && (rw > 1.0);
    // VAN GIU NGUYEN CHI 1 DONG: chi 1 khi textBottom thuc su < can duoi hop.
    const double delta = (needGrow > 0.05) ? needGrow : 0.0;
    if (fit) {
        fit->maxLineW = maxLineW;
        fit->bboxH = bboxH;
        fit->bboxY0 = ry;
        fit->delta = delta;
        fit->expanded = false;
        fit->measurable = canGrow;
    }
    if (delta > 0.0 && canGrow) {
        // Ha can duoi: hop nen `re` (va /BBox do ben goi cap nhat theo cung delta).
        const double nrh = bboxH + delta;
        const double nry = ry - delta;
        QString pre = prefix;
        if (reAt >= 0) {
            pre.replace(reAt, reLen,
                        QString("%1 %2 %3 %4 re")
                            .arg(rx, 0, 'f', 3).arg(nry, 0, 'f', 3)
                            .arg(rw, 0, 'f', 3).arg(nrh, 0, 'f', 3));
        }
        out = pre.toLatin1() + body + suffix.toLatin1();
        if (fit) { fit->expanded = true; fit->bboxH = nrh; fit->bboxY0 = nry; }
        return true;
    }
    out = prefix.toLatin1() + body + suffix.toLatin1();
    return true;
}

// 🔎 0927 LUOT 7: do khop khung cua FreeText NGOAI bang cach DOC LAI tep DA
// LUU (/AP, /Rect thuc te trong file) — KHONG dung so tinh luc dung. Do lai bang
// chinh bo do cua buildForeignApContent() (mixedAdvance + lead) thi so do moi
// dung nghia: innerW la be rong VUNG CHU THAT cua /BBox tru le trai.
bool trMeasureForeignApFit(const QString& path, int pageIndex, int annotIndex,
                           ForeignApFit* out) {
    if (!out) return false;
    *out = ForeignApFit();
    try {
        QPDF pdf;
        pdf.processFile(path.toUtf8().constData());
        auto pages = QPDFPageDocumentHelper(pdf).getAllPages();
        if (pageIndex < 0 || pageIndex >= static_cast<int>(pages.size())) {
            out->reason = "chi so trang ngoai pham vi"; return false;
        }
        QPDFObjectHandle page = pages[pageIndex].getObjectHandle();
        QPDFObjectHandle annots = page.getKey("/Annots");
        if (!annots.isArray()) { out->reason = "trang khong co /Annots"; return false; }
        auto vec = annots.getArrayAsVector();
        int idx = annotIndex;
        if (idx < 0) {
            for (int i = 0; i < static_cast<int>(vec.size()); ++i) {
                QPDFObjectHandle c = vec[i];
                if (c.isDictionary() && c.getKey("/Subtype").getName() == "/FreeText") {
                    idx = i; break;
                }
            }
        }
        if (idx < 0 || idx >= static_cast<int>(vec.size())) {
            out->reason = "khong tim thay FreeText"; return false;
        }
        QPDFObjectHandle a = vec[idx];
        if (!a.isDictionary() || a.getKey("/Subtype").getName() != "/FreeText") {
            out->reason = "annot khong phai FreeText"; return false;
        }
        QPDFObjectHandle rv = a.getKey("/Rect");
        if (!rv.isArray() || rv.getArrayAsVector().size() != 4) {
            out->reason = "thieu /Rect"; return false;
        }
        auto rvv = rv.getArrayAsVector();
        for (int i = 0; i < 4; ++i) out->rectAfter[i] = rvv[i].getNumericValue();
        for (int i = 0; i < 4; ++i) out->rectBefore[i] = out->rectAfter[i];

        QPDFObjectHandle ap = a.getKey("/AP");
        if (!ap.isDictionary()) { out->reason = "thieu /AP"; return false; }
        QPDFObjectHandle n = ap.getKey("/N");
        if (!n.isStream()) { out->reason = "thieu /AP /N"; return false; }
        QPDFObjectHandle bbox = n.getDict().getKey("/BBox");
        if (!bbox.isArray() || bbox.getArrayAsVector().size() != 4) {
            out->reason = "thieu /BBox"; return false;
        }
        auto bv = bbox.getArrayAsVector();
        const double bx0 = bv[0].getNumericValue();
        const double by0 = bv[1].getNumericValue();
        const double bx1 = bv[2].getNumericValue();
        const double by1 = bv[3].getNumericValue();
        const double bboxW = bx1 - bx0;
        out->bboxH = by1 - by0;
        out->bboxY0 = by0;

#if QPDF_MAJOR_VERSION >= 11
        std::shared_ptr<Buffer> sbuf = n.getStreamData(qpdf_dl_all);
#else
        PointerHolder<Buffer> sbuf = n.getStreamData(qpdf_dl_all);
#endif
        const QByteArray raw(reinterpret_cast<const char*>(sbuf->getBuffer()),
                             static_cast<int>(sbuf->getSize()));
        const QString txt = QString::fromLatin1(raw);

        // tham so chu: cỡ chữ (fs) va buoc dong (lead) tu khoi BT..ET
        const int bT = txt.indexOf(QStringLiteral("BT"));
        const int eT = txt.indexOf(QStringLiteral("ET"), bT < 0 ? 0 : bT);
        const QString block = (bT < 0 || eT < 0) ? txt
            : txt.mid(bT, eT - bT);
        static const QRegularExpression reTf(QStringLiteral("([-\\d.]+)\\s+Tf"));
        double fs = 0.0;
        if (auto m = reTf.match(block); m.hasMatch()) fs = m.captured(1).toDouble();
        if (fs <= 0.0) fs = 12.0;
        static const QRegularExpression reTd(
            QStringLiteral("([-\\d.]+)\\s+([-\\d.]+)\\s+Td"));
        double lead = 0.0;
        {
            bool first = true;
            auto it = reTd.globalMatch(block);
            while (it.hasNext()) {
                auto m = it.next();
                if (first) { first = false; continue; }
                const double b = m.captured(2).toDouble();
                if (lead <= 0 && b < 0) lead = -b;
            }
        }
        if (lead <= 0) lead = fs * 1.16;
        out->fontSize = fs;
        out->lead = lead;

        // \U0001f50e 0927 LUOT 7 (sau review): LE TRAI phai la can trai cua HOP `re`
        // (trong /AP da luu), KHONG phai x cua lenh Td dau tien: khi /Q = 1/2
        // (can giua / can phai) thi Td dau la vi tri CUA DONG do, dung no se
        // lam innerW sai. Do le = can phai cua `re` - can trai cua `re` ... thuc
        // te vung chu bat dau tai can trai cua `re` => innerW = be rong cua `re`.
        // Doc `re` tu content stream (prefix truoc BT).
        double boxX = bx0, boxW = bboxW, boxY = by0;
        {
            const int bm = txt.indexOf(QStringLiteral("/Tx BMC"));
            const QString pre = (bm >= 0) ? txt.left(bm) : txt.left(txt.indexOf(QStringLiteral("BT")));
            static const QRegularExpression reRe(
                QStringLiteral("([-\\d.]+)\\s+([-\\d.]+)\\s+([-\\d.]+)\\s+([-\\d.]+)\\s+re"));
            auto it = reRe.globalMatch(pre);
            while (it.hasNext()) {
                auto m = it.next();
                boxX = m.captured(1).toDouble();
                boxY = m.captured(2).toDouble();
                boxW = m.captured(3).toDouble();
            }
        }
        const double x0 = boxX;   // chu bat dau ngay can trai hop
        out->innerW = (boxW > 1.0) ? boxW : ((bboxW - (x0 - bx0) > 1.0) ? bboxW - (x0 - bx0) : bboxW);
        // Baseline dong DAU (y cua lenh Td dau tien) — dung de tinh can duoi
        // cua vung chu trong he toa do cua /AP.
        double baseY = by0;
        {
            auto m = reTd.match(block);
            if (m.hasMatch()) baseY = m.captured(2).toDouble();
        }

        // Do lai tung dong theo CHUNG bo do cua khi dung /AP: doc lai /Contents,
        // bo chu WinAnsi thi do bang Arial (/Widths cua font goc), con lai
        // DejaVu (QFontMetricsF) — dung wrapMixedLines nen khong bao gio lines
        // cua file lech so do.
        QString contents;
        if (a.getKey("/Contents").isString())
            contents = QString::fromStdString(a.getKey("/Contents").getUTF8Value());
        QString winName;
        const WinAnsiMetrics wm = readWinAnsiMetrics(pickWinAnsiFont(n, winName));
        const QFontMetricsF fm(trDejaVuFontAtPixelSize(fs));
        const QStringList lines = wrapMixedLines(contents, out->innerW, wm, fs, fm);
        out->nLines = lines.size();
        double maxW = 0.0;
        for (const QString& ln : lines) {
            const double lw = mixedAdvance(ln, wm, fs, fm);
            if (lw > maxW) maxW = lw;
        }
        out->maxLineW = maxW;
        out->textH  = lines.size() * lead;
        const double mAsc = (std::abs(fm.ascent())  > 0.01) ? std::abs(fm.ascent())  : fs * 0.928;
        const double mDsc = (std::abs(fm.descent()) > 0.01) ? std::abs(fm.descent()) : fs * 0.236;
        out->innerH = (lines.size() - 1) * lead + mAsc + mDsc;
        // 🔎 0927 LUOT 7 (sau review): KHONG duoc so textH voi innerH —
        // textH = n*lead, innerH = (n-1)*lead + asc + desc nen chenh nhau
        // lead-1.164fs ≈ -0.004fs ⇒ clipped LUON = 1, co nghia.
        // Dung so voi KHUNG THAT DA LUU:
        //   ngang: dong nao rong hon innerW
        //   doc  : can duoi chu (y0 - (n-1)*lead - desc) co xuong duoi can /BBox
        const double textBottom = baseY - (lines.size() - 1) * lead - mDsc;
        out->clipped = ((maxW > out->innerW + 0.5) ||
                        (textBottom < by0 - 0.5)) ? 1 : 0;
        out->ok = true;
        return true;
    } catch (const std::exception& e) {
        out->reason = QString::fromUtf8(e.what());
        return false;
    }
}

extern QMutex s_pdfiumMutex;

void AnnotationManager::invalidateNoteObjCache_locked(int pageIndex) {
    for (auto it = m_noteObjIdxCache.begin(); it != m_noteObjIdxCache.end(); ) {
        if (it.key().first == pageIndex) it = m_noteObjIdxCache.erase(it);
        else ++it;
    }
}

// ── FPDF file writer helper ───────────────────────────────────────────────────
struct FileWriter {
    FPDF_FILEWRITE base;
    QFile* file;
    static int WriteBlock(FPDF_FILEWRITE* self, const void* data, unsigned long size) {
        auto* fw = reinterpret_cast<FileWriter*>(self);
        return fw->file->write(reinterpret_cast<const char*>(data),
                               static_cast<qint64>(size)) == static_cast<qint64>(size) ? 1 : 0;
    }
};

// ── Helpers ───────────────────────────────────────────────────────────────────

// Rotation transforms moved to core/PdfCoords.h

static QString readAnnotString(FPDF_ANNOTATION annot, const char* key) {
    unsigned long len = FPDFAnnot_GetStringValue(annot, key, nullptr, 0);
    if (len <= 2) return {};
    std::vector<char16_t> buf(len / 2 + 1, 0);
    FPDFAnnot_GetStringValue(annot, key, reinterpret_cast<FPDF_WCHAR*>(buf.data()), len);
    const QString raw = QString::fromUtf16(buf.data());
    // 🔴 0927 BƯỜ 1 để 5: /Contents của FreeText ta ghi \r (đã chuấn hoá
    // Acrobat) ⇒ đọc ra đânh để để về 'n' để màn hình + overlay +
    // trWrapFreeText củng thấy đúng dòng. KHOÁNG đổng với tá cả khác.
    if (qstrcmp(key, "Contents") == 0) return trUnescapeContents(raw);
    return raw;
}

static QString subtypeName(FPDF_ANNOTATION_SUBTYPE t) {
    switch (t) {
        case FPDF_ANNOT_TEXT:        return "Note";
        case FPDF_ANNOT_FREETEXT:    return "FreeText";
        case FPDF_ANNOT_HIGHLIGHT:   return "Highlight";
        case FPDF_ANNOT_UNDERLINE:   return "Underline";
        case FPDF_ANNOT_STRIKEOUT:   return "Strikethrough";
        case FPDF_ANNOT_SQUARE:      return "Rectangle";
        case FPDF_ANNOT_CIRCLE:      return "Ellipse";
        case FPDF_ANNOT_LINE:        return "Line";
        case FPDF_ANNOT_INK:         return "Freehand";
        case FPDF_ANNOT_STAMP:       return "Stamp";
        case FPDF_ANNOT_WIDGET:      return "Widget";
        default:                     return "Annotation";
    }
}

// ── Insert Image (SPEC_INSERT_IMAGE_2026-08-30) ──────────────────────────────
// Tao FPDF_PAGEOBJECT IMAGE tu QImage (giu kenh alpha - Format_ARGB32_Premultiplied
// giong het FPDFBitmap_BGRA). Matrix fill dung rect trong toa do PDF CHUA xoay.
// Ben goi PHAI giu s_pdfiumMutex + mo trang.
static FPDF_PAGEOBJECT makeStampImageObject(FPDF_DOCUMENT doc, FPDF_PAGE page,
                                            const QImage& src,
                                            double a, double d, double e, double f) {
    QImage img = src.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    if (img.isNull()) return nullptr;
    // FPDFBitmap_CreateEx KHONG sao chep: chi tro toi bo dem cua img. SetBitmap
    // giu con tro do, ma img (bien cuc bo) chet khi ham ket thuc ⇒ doc vung chet.
    // Dung FPDFBitmap_Create de PDFium SO HUU bo nho, roi copy tung dong vao
    // (stride cua PDFium co the khac cua QImage).
    FPDF_BITMAP bmp = FPDFBitmap_Create(img.width(), img.height(), /*alpha=*/1);
    if (!bmp) return nullptr;
    {
        const int dstStride = FPDFBitmap_GetStride(bmp);
        uint8_t* dst = static_cast<uint8_t*>(FPDFBitmap_GetBuffer(bmp));
        for (int y = 0; y < img.height(); ++y)
            memcpy(dst + y * dstStride, img.constScanLine(y),
                   qMin<int>(dstStride, img.bytesPerLine()));
    }
    FPDF_PAGEOBJECT obj = FPDFPageObj_NewImageObj(doc);
    if (!obj) { FPDFBitmap_Destroy(bmp); return nullptr; }
    if (!FPDFImageObj_SetBitmap(&page, 1, obj, bmp)) {
        FPDFBitmap_Destroy(bmp);
        FPDFPageObj_Destroy(obj);
        return nullptr;
    }
    FPDFBitmap_Destroy(bmp);
    if (!FPDFImageObj_SetMatrix(obj, a, 0.0, 0.0, d, e, f)) {
        FPDFPageObj_Destroy(obj);
        return nullptr;
    }
    return obj;
}

// Doc anh goc cua Stamp annot (object IMAGE dau tien) ra QImage de overlay ve
// + de snapshot/hoan tac. Rong = khong co object imgae (vi du Stamp FORM cua
// phan mem khac — khong ve duoc qua overlay). Ben goi PHAI giu s_pdfiumMutex.
//
// PNG trong suot: PDFium luu alpha thanh SMask; FPDFImageObj_GetBitmap() (fpdf_edit
// .h:807-809) BO QUA mask + matrix => tra bitmap rong voi anh trong suot => annot
// bi loai khoi danh sach ve (chi con vien select). Dung FPDFImageObj_GetRendered
// Bitmap() (fpdf_edit.h:820-833, "takes the image mask and image matrix into
// account") truoc; NULL moi lui ve GetBitmap (du phong cho anh khong mask).
static void captureStampImage(FPDF_DOCUMENT doc, FPDF_PAGE page,
                              FPDF_ANNOTATION annot, QImage& out) {
    out = QImage();
    const int n = FPDFAnnot_GetObjectCount(annot);
    for (int i = 0; i < n; ++i) {
        FPDF_PAGEOBJECT obj = FPDFAnnot_GetObject(annot, i);
        if (!obj) continue;
        if (FPDFPageObj_GetType(obj) != FPDF_PAGEOBJ_IMAGE) continue;
        const char* src = "FAIL";
        FPDF_BITMAP bmp = FPDFImageObj_GetRenderedBitmap(doc, page, obj);
        if (bmp) {
            // Do phan giai cua GetRenderedBitmap = ty le matrix (diem PDF ~ px).
            // Stamp ve nho => bitmap ti hon anh goc. Phong matrix tam (nhu
            // VectorLayer.cpp:892-922) de lay lai do phan giai gan nhat roi
            // TRA LAI NGAY — buoc nay mới ra mask ratio dung + alpha sạch.
            const int rw0 = FPDFBitmap_GetWidth(bmp), rh0 = FPDFBitmap_GetHeight(bmp);
            FPDF_IMAGEOBJ_METADATA md{};
            unsigned int natW = 0, natH = 0;
            if (FPDFImageObj_GetImageMetadata(obj, page, &md)) { natW = md.width; natH = md.height; }
            FS_MATRIX om{};
            if (rw0 > 0 && rh0 > 0 && natW > 0 && natH > 0
                && FPDFPageObj_GetMatrix(obj, &om) != 0) {
                const double k = qMin(qMin(double(natW) / double(rw0),
                                           double(natH) / double(rh0)),
                                      std::sqrt(4000000.0 / double(qMax(1, rw0 * rh0))));
                if (k > 1.5) {
                    FS_MATRIX big{ float(om.a * k), float(om.b * k),
                                   float(om.c * k), float(om.d * k), om.e, om.f };
                    if (FPDFPageObj_SetMatrix(obj, &big)) {
                        FPDF_BITMAP bmp2 = FPDFImageObj_GetRenderedBitmap(doc, page, obj);
                        FPDFPageObj_SetMatrix(obj, &om);
                        if (bmp2) { FPDFBitmap_Destroy(bmp); bmp = bmp2; }
                    }
                }
            }
            src = "rendered";
        } else {
            bmp = FPDFImageObj_GetBitmap(obj);
            if (bmp) src = "legacy";
        }
        if (!bmp) {
            qDebug().noquote() << "[imgstamp] capture w=0 h=0 fmt=0 src=FAIL";
            continue;
        }
        const int w = FPDFBitmap_GetWidth(bmp), h = FPDFBitmap_GetHeight(bmp);
        qDebug().noquote() << "[imgstamp] capture w=" << w << " h=" << h
                           << " fmt=" << FPDFBitmap_GetFormat(bmp) << " src=" << src;
        const int fmt = FPDFBitmap_GetFormat(bmp);
        const int stride = FPDFBitmap_GetStride(bmp);
        const unsigned char* buf =
            reinterpret_cast<const unsigned char*>(FPDFBitmap_GetBuffer(bmp));
        if (w <= 0 || h <= 0 || !buf) { FPDFBitmap_Destroy(bmp); continue; }
        const bool hasAlpha = (fmt == FPDFBitmap_BGRA);
        QImage img(w, h, hasAlpha ? QImage::Format_ARGB32_Premultiplied
                                  : QImage::Format_RGB32);
        for (int y = 0; y < h && y < img.height(); ++y) {
            const QRgb* s = reinterpret_cast<const QRgb*>(buf + qint64(y) * stride);
            QRgb* d = reinterpret_cast<QRgb*>(img.scanLine(y));
            if (hasAlpha) {
                std::memcpy(d, s, size_t(w) * 4);
            } else {
                for (int x = 0; x < w; ++x)
                    d[x] = qRgb(qRed(s[x]), qGreen(s[x]), qBlue(s[x]));
            }
        }
        FPDFBitmap_Destroy(bmp);
        // Giu bo nho: overlay ve theo /Rect nen chi can nguon vua phai.
        const int kMax = 1024;
        if (img.width() > kMax || img.height() > kMax)
            img = img.scaled(QSize(kMax, kMax), Qt::KeepAspectRatio, Qt::SmoothTransformation);
        out = img;
        break;
    }
}

// Parse a FreeText DA string "/Helv <size> Tf <r> <g> <b> rg".
// Returns true if at least size or colour was parsed.
static bool parseDA(const QString& da, QColor& outColor, float& outSize) {
    QStringList parts = da.split(' ', Qt::SkipEmptyParts);
    bool gotSize = false, gotColor = false;
    for (int i = 0; i < parts.size(); ++i) {
        if (parts[i] == QLatin1String("Tf") && i >= 1) {
            outSize = parts[i - 1].toFloat();
            gotSize = true;
        } else if (parts[i] == QLatin1String("rg") && i >= 3) {
            float r = parts[i - 3].toDouble();
            float g = parts[i - 2].toDouble();
            float b = parts[i - 1].toDouble();
            outColor = QColor::fromRgbF(r, g, b);
            gotColor = true;
        }
    }
    return gotSize || gotColor;
}

// ── FreeText: font + o + ve dung CHUNG cho hai view ─────────────────────────
QFont trDejaVuFontAtPixelSize(double px) {
    static QString family;
    static bool loaded = false;
    if (!loaded) {
        loaded = true;
        int id = QFontDatabase::addApplicationFont(":/fonts/DejaVuSans.ttf");
        if (id >= 0) {
            QStringList fams = QFontDatabase::applicationFontFamilies(id);
            if (!fams.isEmpty()) family = fams.first();
        }
    }
    QFont f(family.isEmpty() ? QStringLiteral("DejaVu Sans") : family);
    // 0903: setPixelSize lam TRON ve so nguyen — o co nho sai lech dang ke.
    // QFont ho tro co le: dung setPixelSize cho tri lon, con lai de painter lo.
    f.setPixelSize(qMax(1, qRound(px)));
    f.setHintingPreference(QFont::PreferNoHinting);  // khong ep net vao luoi pixel
    return f;
}

// 🔴 0927 BƯỚC 1 — CHUẨN HOÁ XUỐNG DÒNG cho /Contents:
// ghi \r (quy ước Acrobat cho FreeText) để file lưu ra luôn đúng đấu
// chất lặp dòng. Khi đọc ra (readAnnotString/loadPage/setAnnotContents/
// snapshot) chuẩn hoá về ‘\n’ để màn hình + overlay + trWrapFreeText
// cùng thấy đúng số dòng.
QString trContentsToCr(const QString& text) {
    QString t = text;
    t.replace(QStringLiteral("\r\n"), QStringLiteral("\r"));
    t.replace(QLatin1Char('\n'), QLatin1Char('\r'));
    return t;
}

QString trUnescapeContents(const QString& text) {
    QString t = text;
    t.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    t.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    return t;
}

// 🔴 0927 BƯỚC 1 — HÀM NGẮT DÒNG DUY NHẤT cho FreeText CỦA TA.
// Tách cứng theo ‘\n’ (đã chuẩn hoá \r\n và \r trước — xem
// trUnescapeContents), từng đoạn ngắt mềm theo bề rộng wrapW, ngắt tại dấu
// cách; một từ dài hơn ô thì ngắt theo KÝ TỰ (từ không chứa dấu, không
// bảo giờ biến mất khỏi màn hình — vỡ WriteMode dễ chứ). CÙNG fm với /AP
// (trDejaVuFontAtPixelSize) ⇒ màn hình và file không lệch.
QStringList trWrapFreeText(const QString& text, double wrapW, const QFontMetricsF& fm) {
    QStringList lines;
    const QString t = trUnescapeContents(text);
    // wrapW <= 0 (ô quá hẹp / chưa biết bề rộng) ⇒ KHÔNG ngắt: còn 1 dòng như cũ.
    if (wrapW <= 0.0) { lines << t; return lines; }
    const QStringList paras = t.split(QLatin1Char('\n'));
    for (const QString& para : paras) {
        if (para.isEmpty()) { lines << QString(); continue; }
        QString cur;
        const QStringList words = para.split(QLatin1Char(' '));
        for (const QString& w : words) {
            const QString cand = cur.isEmpty() ? w : cur + QLatin1Char(' ') + w;
            if (cur.isEmpty() || fm.horizontalAdvance(cand) <= wrapW) cur = cand;
            else { lines << cur; cur = w; }
        }
        // Từ dài hơn ô: cắt để từng ra từng một không khớp wrapW.
        while (cur.size() > 1 && fm.horizontalAdvance(cur) > wrapW) {
            int cut = cur.size() - 1;
            while (cut > 1 && fm.horizontalAdvance(cur.left(cut)) > wrapW) --cut;
            lines << cur.left(cut);
            cur = cur.mid(cut);
        }
        lines << cur;
    }
    return lines;
}

// Nong o hien thi (Y-down, pt) cho DU CHO chu o fontSizePt: chieu ngang theo
// advance THAT (QFontMetricsF, cung font voi /AP), chieu cao theo so dong wrap
// trong chieu ngang do — khong dung he so co dinh (length*5.5, h=18) nua.
// 🔴 0927: horizontalAdvance(text) đo CẢ chuỗi như MỘT dòng ⇒ có '\n' là ô
// rộng sai. Nay lấy MAX advance trên TỪNG dòng sau khi ngắt mềm (dùng hàm
// trên) — cùng công thức mà /AP dùng nên màn hình và file cùng số dòng.
QRectF trFreeTextFitRect(const QRectF& dispRect, const QString& text, float fontSizePt) {
    if (text.isEmpty() || fontSizePt <= 0.0f) return dispRect;
    const double pad = 2.0;                       // le pt moi ben (khop pad overlay)
    QFont f = trDejaVuFontAtPixelSize(fontSizePt); // zoom=1 => 1pt=1px
    QFontMetricsF fm(f);
    // Bề rộng để ngắt mềm = bề rộng TRONG Ô đang tồn tại (w − 2·pad); nếu
    // dùng luôn bề rộng LỚN nhất vừa ra thì ta sẽ không bao giờ phải ngắt
    // mềm ở lần nong ô — chỉ dùng cho chuỗi 1 dòng.
    const double candW = qMax(0.0, dispRect.width() - 2.0 * kTrFreeTextWrapPad);
    const QStringList lines = trWrapFreeText(text, candW, fm);
    double maxAdv = 0.0;
    for (const QString& ln : lines)
        maxAdv = qMax(maxAdv, fm.horizontalAdvance(ln));
    const double w = qMax(dispRect.width(), maxAdv + 2.0 * pad);
    // 🔴 0927 LƯỢT 2: chiều cao = apPad (trên) + n·lineSpacing (mọi dòng) +
    // apPad (dưới) — KHÔNG phải n·lineSpacing + 2·pad(=4pt). Lý do: /AP đặt
    // mốc nền dòng ĐẦU ở y = h − apPad − lineAdv rồi hạ mỗi dòng −lineAdv, nên
    // dòng CUỐI có mốc ở y = h − apPad − n·lineAdv. Muốn mốc dòng cuối ≥ apPad
    // (không cắt descender, không tràn ô) thì h ≥ n·lineSpacing + 2·apPad.
    // Đo 27/09: với công thức cũ, h = 73.84 và mốc dòng cuối rơi đúng y = 0
    // ⇒ PyMuPDF render bị CẮT chữ ("trăm điểm" mất nửa dưới).
    const double h = qMax(dispRect.height(),
                          lines.size() * fm.lineSpacing() + 2.0 * kTrFreeTextWrapPad);
    return QRectF(dispRect.topLeft(), QSizeF(w, h));
}

// Ve chu FreeText trong dRect (px). KHUNG quyet cho xuong dong (Qt::TextWordWrap
// bo theo chieu ngang inner). CO CHU LUON = fontSizePt*zoom — KHONG bao gio tu
// thu nho. Noi dung cao hon khung => ve TRAN xuong duoi (view khong setClipRect)
// de user thay con chu va tu keo khung cao them. pixelSize giong ghost preview.
void drawFreeTextOverlay(QPainter& p, const QRectF& dRect, const QString& text,
                         float fontSizePt, double zoom, const QColor& penColor) {
    if (text.isEmpty()) return;
    const double pad = qMax(1.0, 2.0 * zoom);
    const QRectF inner = dRect.adjusted(pad, pad, -pad, -pad);
    const double availW = inner.width();
    if (availW < 1.0) return;

    const int flags = Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap;
    const double ps = qMax(1.0, static_cast<double>(fontSizePt) * zoom);
    QFont f = trDejaVuFontAtPixelSize(ps);
    QFontMetricsF fm(f);
    // Chieu cao anh = max(chieu cao khung, chieu cao chu tu nhien tai availW) =>
    // chu tran khung van hien. Tran 50k px de tranh phat anh khong lo tu text ben.
    const double textH = fm.boundingRect(QRectF(0, 0, availW, 1e6), flags, text).height();
    const double imgH = (std::min)(qMax(inner.height(), textH), 50000.0);
    {   // 0903 CHAN DOAN — in ra dung thu ham nay dang dung
        static QString _last;
        const QString cur = QString("fam=%1 ps=%2 zoom=%3 rect=%4x%5 txt=%6")
            .arg(f.family()).arg(ps,0,'f',1).arg(zoom,0,'f',3)
            .arg(dRect.width(),0,'f',1).arg(dRect.height(),0,'f',1).arg(text.left(20));
        if (cur != _last) { _last = cur; qDebug().noquote() << "[fttext]" << cur; }
    }
    // 🔴 0903 textimg: KHONG BAO GIO p.drawText len painter cua view nua. Atlas
    // glyph GL bi lop vector GL tao/xoa texture cap lai ID => chu < ~64px mau
    // tu ban ve (soc cheo net CAD). Ve chu len QImage (rasterizer CPU, khong
    // dung atlas) roi dan anh = quad texture thuong (giong Stamp — da chung minh
    // chay dung). Hai view cung goi ham nay => ket qua giong het.
    const QColor col = penColor.isValid() ? penColor : QColor(Qt::black);
    const double dpr = p.device() ? p.device()->devicePixelRatioF() : 1.0;
    // Khoa cache: text + ps + mau + BE RONG khung (availW — quyet cach wrap) +
    // chieu cao anh (imgH) + dpr. Doi be rong khung => doi wrap => doi khoa =>
    // dung anh moi. Tran 64, LRU. Chi GUI thread (paintEvent 2 view) => khong mutex.
    const QString key = text + QChar(1) + QString::number(ps, 'g', 17) + QChar(1)
                      + QString::number(static_cast<quint64>(col.rgba()), 16) + QChar(1)
                      + QString::number(availW, 'g', 17) + QChar(1)
                      + QString::number(imgH, 'g', 17) + QChar(1)
                      + QString::number(dpr, 'g', 17);
    static QHash<QString, QImage> s_textCache;
    static QStringList s_textLru;
    if (!s_textCache.contains(key)) {
        const int w = qMax(1, static_cast<int>(std::ceil(availW * dpr)));
        const int h = qMax(1, static_cast<int>(std::ceil(imgH * dpr)));
        QImage img(w, h, QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::transparent);
        img.setDevicePixelRatio(dpr);   // painter tren anh tu quy chieu px logic
        {
            QPainter ip(&img);
            // Gir nguyen khu rang cưa + font + wrap nhu phien ban drawText cu.
            ip.setRenderHint(QPainter::Antialiasing, true);
            ip.setRenderHint(QPainter::TextAntialiasing, true);
            ip.setFont(f);
            ip.setPen(col);
            ip.drawText(QRectF(0.0, 0.0, availW, imgH), flags, text);
        }
        s_textCache.insert(key, img);
        s_textLru.append(key);
        while (s_textLru.size() > 64) s_textCache.remove(s_textLru.takeFirst());
    } else {
        s_textLru.removeAll(key);
        s_textLru.append(key);
    }
    p.save();
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    // setDevicePixelRatio tren anh => drawImage dan dung kich thuoc px logic.
    p.drawImage(inner.topLeft(), s_textCache.value(key));
    p.restore();
}

// ── AnnotationManager ─────────────────────────────────────────────────────────

AnnotationManager::AnnotationManager(QObject* parent) : QObject(parent) {
    qRegisterMetaType<QList<AnnotInfo>>("QList<AnnotInfo>");
}

FPDF_FONT AnnotationManager::unicodeFont() {
    if (m_unicodeFont) return m_unicodeFont;
    if (!m_doc) return nullptr;
    if (m_unicodeFontData.isEmpty()) {
        QFile f(":/fonts/DejaVuSans.ttf");
        if (!f.open(QIODevice::ReadOnly)) {
            qWarning() << "[inote] cannot open embedded font from qrc";
            return nullptr;
        }
        m_unicodeFontData = f.readAll();
    }
    m_unicodeFont = FPDFText_LoadFont(
        m_doc,
        reinterpret_cast<const uint8_t*>(m_unicodeFontData.constData()),
        static_cast<uint32_t>(m_unicodeFontData.size()),
        FPDF_FONT_TRUETYPE,
        /*cid=*/1);
    qDebug() << "[inote] unicode font loaded=" << (m_unicodeFont != nullptr)
             << "bytes=" << m_unicodeFontData.size();
    return m_unicodeFont;
}

void AnnotationManager::setDocument(FPDF_DOCUMENT doc, const QString& filePath) {
    {
        // 🔴 VIỆC 1: GUI chi duoc tryLock(0). Neu khong lay duoc, pending gen
        // se flush luc saveDocument, forgetDocument(tai lieu cu) o ~PdfDocument::close.
        TryPdfiumLock lk(__FILE__, __LINE__);
        if (lk.held()) {
            const auto pend = m_pendingGen;
            for (int p : pend) flushGenerate_locked(p);
            if (m_doc && m_doc != doc) {
                PageCache::forgetDocument(m_doc);
            }
        } else {
            qDebug().noquote() << "[annot] setDocument SKIP flush — khoa ban";
        }
    }
    // 🔴 0927 LƯỢT 3: /AP trong bộ nhớ gắn với MỘT tài liệu. Khi chuyển sang
    // tài liệu khác (mở tệp khác, hoặc save xong loadTabFile quay lại tệp gốc)
    // thì annot cũ không còn nằm trong doc ⇒ bỏ, nếu không lần save sau sẽ vá
    // nhầm /AP của chú thích đã lưu.
    if (m_doc != doc) {
        m_ownApInMem.clear();
        m_ownApBox.clear();
        m_apOwnPending.clear();
        m_ownApFailed = false;
    }
    m_doc  = doc;
    m_path = filePath;
    m_unicodeFont = nullptr;
    m_unicodeFontData.clear();
}

AnnotationManager::~AnnotationManager() { shutdownHeavy(); }

// 🔴 0929 LƯỢT 33h: nền gọi TRƯỚC doc->close() (còn cần m_doc để flush + forgetDocument);
// ~AnnotationManager trên UI gọi lại → no-op (guard). Idempotent.
void AnnotationManager::shutdownHeavy() {
    if (m_heavyShutdown) return;
    m_heavyShutdown = true;
    {
        // 🔴 VIỆC 1: destructor chay o background (closeJob: delete t) hoac GUI
        // (MainWindow::~MainWindow). Neu GUI khong lay duoc khoa, bo qua flush
        // (pending gen duoc flush luc save, forgetDocument o ~PdfDocument::close).
        TryPdfiumLock lk(__FILE__, __LINE__);
        if (lk.held()) {
            const auto pend = m_pendingGen;
            for (int p : pend) flushGenerate_locked(p);
            if (m_doc) PageCache::forgetDocument(m_doc);
        } else {
            qDebug().noquote() << "[annot] ~AnnotationManager SKIP flush — khoa ban";
        }
    }
}

FPDF_PAGE AnnotationManager::acquireSharedPage(int pageIndex) {
    return PageCache::acquire(m_doc, pageIndex);
}

void AnnotationManager::pinPage_locked(int pageIndex) {
    PageCache::acquire(m_doc, pageIndex);
    // R1: cap doi acquire() — warm cache, release ngay sau khi nap (trang o lai MRU).
    PageCache::PageBorrow _b(m_doc, pageIndex);
}

void AnnotationManager::pinPage(int pageIndex) { TimedPdfiumLock lk(__FILE__, __LINE__); pinPage_locked(pageIndex); }

bool AnnotationManager::isSharedPage(int pageIndex) const {
    return PageCache::tryAcquire(m_doc, pageIndex) != nullptr;
}

void AnnotationManager::releaseSharedPage() {
    TimedPdfiumLock lk(__FILE__, __LINE__);
    releaseSharedPage_locked();
}

void AnnotationManager::releaseSharedPage_locked() {
    if (m_doc) PageCache::forgetDocument(m_doc);
}

// ── LÁT C 0902: delta cho duong CẬP NHẬT CÓ CHỌN LỌC ─────────────────────────
// Xem ly do + hop dong idempotent o AnnotationManager.h (struct VisualDelta).

void AnnotationManager::recordCreatedVisual_locked(FPDF_PAGE page, int pageIndex) {
    const int n = FPDFPage_GetAnnotCount(page);
    if (n <= 0) return;
    FPDF_ANNOTATION a = FPDFPage_GetAnnot(page, n - 1);   // annot VUA TAO nam cuoi
    if (!a) return;
    AnnotVisual av;
    const bool ok = buildVisual(page, a, pageIndex, av);
    FPDFPage_CloseAnnot(a);
    if (!ok || av.uid.isEmpty()) return;   // khong dinh vi duoc bang uid → mac duong nap lai
    QMutexLocker dl(&m_deltaMutex);
    m_visualDeltas[pageIndex].append({av, false});
}

QString AnnotationManager::annotUid_locked(FPDF_PAGE page, int index) {
    FPDF_ANNOTATION a = FPDFPage_GetAnnot(page, index);
    if (!a) return {};
    const QString uid = readAnnotString(a, "TRUID");
    FPDFPage_CloseAnnot(a);
    return uid;
}

void AnnotationManager::recordVisualDelta(int page, const AnnotVisual& av) {
    if (av.uid.isEmpty()) return;
    QMutexLocker dl(&m_deltaMutex);
    m_visualDeltas[page].append({av, false});
}

QList<VisualDelta> AnnotationManager::visualDeltas(int page) const {
    QMutexLocker dl(&m_deltaMutex);
    return m_visualDeltas.value(page);
}

void AnnotationManager::flushGenerate_locked(int pageIndex) {
    if (!m_pendingGen.contains(pageIndex) || !m_doc) return;
    FPDF_PAGE p = PageCache::acquire(m_doc, pageIndex);
    if (p) {
        PageCache::PageBorrow _b(m_doc, pageIndex);   // R1: cap doi acquire()
        QElapsedTimer _gt; _gt.start();
        setOwnNoteObjectsActive(p, true);
        FPDFPage_GenerateContent(p);
        qDebug().noquote() << "[perf] genContent page=" << pageIndex << "ms=" << _gt.elapsed();
    }
    m_pendingGen.remove(pageIndex);
}

QList<AnnotInfo> AnnotationManager::loadPage(int pageIndex, bool* outOk) {
    QList<AnnotInfo> result;
    if (outOk) *outOk = false;
    if (!m_doc) return result;

    // 🔴 VIỆC 1 (SPEC_SMOOTH_123 31/08): LUONG GIAO DIEN chi duoc tryLock(0).
    // Lay duoc thi lam; KHONG lay duoc thi tra du lieu cu + day viec that sang
    // luong nen (ben goi goi loadPage voi outOk==nullptr tren QtConcurrent se
    // blocking hop le). TUYET DOI khong con "thu 3 lan roi cho that" (livelock).
    const bool isMain = (QThread::currentThread() == QCoreApplication::instance()->thread());
    if (isMain && outOk) {
        TryPdfiumLock lock(__FILE__, __LINE__);
        if (!lock.held()) {
            qDebug().noquote() << "[annot] loadPage SKIP — khoa ban, day sang luong nen page=" << pageIndex;
            return result;   // *outOk = false — ben goi GIU du lieu cu
        }
        result = loadPage_locked(pageIndex);
        if (outOk) *outOk = true;
        return result;
    }

    TimedPdfiumLock lock(__FILE__, __LINE__);
    result = loadPage_locked(pageIndex);
    if (outOk) *outOk = true;
    return result;
}

QList<AnnotInfo> AnnotationManager::loadPage_locked(int pageIndex) {
    QList<AnnotInfo> result;
    if (!m_doc) return result;
    FPDF_PAGE page = PageCache::acquire(m_doc, pageIndex);
    if (!page) return result;
    PageCache::PageBorrow _b(m_doc, pageIndex);   // R1: cap doi acquire()

    double pageH = FPDF_GetPageHeight(page);
    double pageW = FPDF_GetPageWidth(page);
    int rot = FPDFPage_GetRotation(page);
    const QPointF box = pdfBoxOrigin(page);
    int count = FPDFPage_GetAnnotCount(page);

    QElapsedTimer _parseT;
    _parseT.start();

    // 🔴 LƯỢT 33d (hồi quy real=0): BỎ `if (m_stopScan) break` trong vòng annot.
    // m_stopScan là tín hiệu dừng VÒNG QUÉT (loadAllStreaming chặn ở ranh giới trang,
    // đủ nhanh — mỗi trang parse µs–ms). Để nó nằm đây biến stopScan (tab nền bị
    // đóng băng ở syncThumbnailPoolsToActiveTab) thành "mọi loadPage trực tiếp của
    // tab trả rỗng" — probe real=0, và đường đọc annot đơn trang cũng chết theo.
    for (int i = 0; i < count; ++i) {
        FPDF_ANNOTATION annot = FPDFPage_GetAnnot(page, i);
        if (!annot) continue;

        AnnotInfo info;
        info.pageIndex = pageIndex;
        info.indexInPage = i;
        info.type   = subtypeName(FPDFAnnot_GetSubtype(annot));
        {
            QString trtool = readAnnotString(annot, "TRTOOL");
            if (!trtool.isEmpty()) info.type = trtool;
        }
        info.text   = readAnnotString(annot, "Contents");
        info.author = readAnnotString(annot, "T");

        FS_RECTF r{};
        if (FPDFAnnot_GetRect(annot, &r)) {
            QPointF d1 = pdfToDisp(r.left, r.bottom, pageW, pageH, rot, box.x(), box.y());
            QPointF d2 = pdfToDisp(r.right, r.top, pageW, pageH, rot, box.x(), box.y());
            info.rect = QRectF(d1, d2).normalized();
        }

        unsigned int cr = 0, cg = 0, cb = 0, ca = 255;
        FPDFAnnot_GetColor(annot, FPDFANNOT_COLORTYPE_Color, &cr, &cg, &cb, &ca);
        info.color = QColor(cr, cg, cb, ca);
        auto sub = FPDFAnnot_GetSubtype(annot);
        if (sub == FPDF_ANNOT_FREETEXT) {
            QColor daColor; float daSize;
            if (parseDA(readAnnotString(annot, "DA"), daColor, daSize))
                info.color = daColor;
        } else if (sub == FPDF_ANNOT_TEXT) {
            info.color = QColor(255, 220, 0);
        }
        info.isDraft = FPDFAnnot_HasKey(annot, "TRSD") != 0;
        info.uid = readAnnotString(annot, "TRUID");
        info.isOwn = FPDFAnnot_HasKey(annot, "TRUID") != 0 || FPDFAnnot_HasKey(annot, "TRID") != 0;

        result.append(info);
        FPDFPage_CloseAnnot(annot);
    }

    qDebug().noquote() << "[perf] loadPage parse page=" << pageIndex << "annots=" << result.size() << "ms=" << _parseT.elapsed();
    return result;
}

QList<AnnotInfo> AnnotationManager::loadAll(int pageCount) {
    QList<AnnotInfo> all;
    for (int i = 0; i < pageCount; ++i) {
        all.append(loadPage(i));
        QThread::yieldCurrentThread();
    }
    return all;
}

void AnnotationManager::loadAllStreaming(int pageCount, int startPage) {
    if (!m_doc) return;
    startPage = qBound(0, startPage, pageCount - 1);

    QElapsedTimer totalTimer;
    totalTimer.start();
    int pagesWithAnnots = 0;
    int totalAnnots = 0;
    int pagesScanned = 0;

    // Spiral scan order: startPage first, then alternate outward
    QVector<int> order;
    order.reserve(pageCount);
    order.append(startPage);
    for (int d = 1; d < pageCount; ++d) {
        if (startPage + d < pageCount) order.append(startPage + d);
        if (startPage - d >= 0) order.append(startPage - d);
    }

    for (int i : order) {
        if (m_stopScan.load()) {
            qDebug().noquote() << "[comments] scan CANCEL";
            return;
        }
        // 🔴 LƯỢT 33b (J — tab nặng chặn tab mới): annot scan của tab NỀN dừng ở
        // ranh giới trang. loadPage một trang nặng giữ s_pdfiumMutex ~1,7 s
        // (đo r31b: AnnotationManager.cpp:1478 giu ms=1688), đúng lúc tab vừa mở
        // cần khoá. Scan sẽ chạy lại khi tab thành hiện hành (onCommentsRequested).
        {
            const FPDF_DOCUMENT a = PageCache::activeDoc();
            // 🔴 LƯỢT 33e (mục 3 — reviewer lỗi 3): CHỈ tab NỀN THẬT (a != null &&
            // m_doc != a) mới được đặt m_stopScan. activeDoc NULL = lúc chuyển tiếp
            // (đang mở tab khác) — đặt cờ ở đó lây cờ hủy sang tab khi nó thành hiện
            // hành. null + khẩn ⇒ NHƯỜNG qua vòng ngủ dưới (có trần), KHÔNG hủy vĩnh
            // viễn; null + không khẩn ⇒ quét tiếp.
            if (m_doc && a && m_doc != a) {
                // 🔴 LƯỢT 33d: đánh dấu đã hủy — finished handler giữ
                // annotCacheValid=false ⇒ tab quay lại sẽ được quét tiếp.
                m_stopScan.store(true);
                qDebug().noquote() << "[comments] scan DUNG ly do=tab-nen";
                return;
            }
        }
        // 🔴 LƯỢT 33d (J): trang ĐẦU của tab hiện hành đang chờ ảnh nền
        // (g_pdfiumUrgentPage >= 0, ContinuousView xoá lúc ACCEPT) ⇒ scan NHƯỜNG ở
        // ranh giới trang — loadPage một trang nặng giữ khoá ~1,7 s ngay trước lượt
        // vẽ trang 0. Trần ngủ 2 s chống đói.
        // 🔴 LƯỢT 33e (mục 3): thêm pdfiumUrgentPending() — activeDoc null + đang mở
        // doc khẩn ⇒ NHƯỜNG tại ranh giới trang (đúng nguyên tắc "chỉ nhường, không
        // bỏ vĩnh viễn"), hết khẩn hoặc hết trần 2 s là quét tiếp.
        if (i != g_pdfiumUrgentPage().load(std::memory_order_acquire)) {
            int guard = 0;
            while (!m_stopScan.load() && guard++ < 100
                   && (pdfiumUrgentPending()
                       || (g_pdfiumUrgentPage().load(std::memory_order_acquire) >= 0
                           && QDateTime::currentMSecsSinceEpoch()
                              <= g_pdfiumUrgentPageDeadline().load(std::memory_order_acquire))))
                QThread::msleep(20);
        }
        QElapsedTimer pageTimer;
        pageTimer.start();
        QList<AnnotInfo> pageAnnots = loadPage(i);
        qint64 pageMs = pageTimer.elapsed();

        if (pageMs > 150)
            qDebug().noquote() << "[comments] slow page=" << i << "ms=" << pageMs;

        if (!pageAnnots.isEmpty()) {
            ++pagesWithAnnots;
            totalAnnots += pageAnnots.size();
            qDebug().noquote() << "[comments] scan page=" << i << "annots=" << pageAnnots.size();
            emit pageAnnotsLoaded(i, pageAnnots);
        }
        ++pagesScanned;
        if (pagesScanned % 5 == 0)
            emit scanProgress(pagesScanned, pageCount);

        // Giu nhip nhuong buoc giua cac trang: nha khoa (loadPage da unlock),
        // roi msleep truoc khi lay lai. QMutex barging — lay lai NGAY khong
        // cuu duoc ai, PHAI nghi mot nhip (bai hoc 19/08).
        QThread::yieldCurrentThread();
        if (m_userBusy.load()) {
            // Uu tien THAP: nguoi dung dang thao tac (markup/scroll/zoom) →
            // nhuong buoc rong rai hon, tra khoa cho giao dien.
            QThread::msleep(30);
            qDebug().noquote() << "[comments] scan yield page=" << i << "userBusy";
        } else {
            QThread::msleep(1);
            qDebug().noquote() << "[comments] scan yield page=" << i;
        }
        if (m_stopScan.load()) {
            qDebug().noquote() << "[comments] scan CANCEL";
            return;
        }
    }
    emit scanProgress(pageCount, pageCount);
    qint64 totalMs = totalTimer.elapsed();
    qDebug().noquote() << "[comments] scan finished pages=" << pageCount
             << "pagesWithAnnots=" << pagesWithAnnots << "totalAnnots=" << totalAnnots << "ms=" << totalMs;
}

bool AnnotationManager::buildVisual(FPDF_PAGE page, FPDF_ANNOTATION annot, int pageIndex, AnnotVisual& out) {
    int sub = FPDFAnnot_GetSubtype(annot);
    const bool trOwn = (FPDFAnnot_HasKey(annot, "TRUID") != 0) || (FPDFAnnot_HasKey(annot, "TRID") != 0);
    if ((FPDFAnnot_GetFlags(annot) & FPDF_ANNOT_FLAG_HIDDEN) && !trOwn) return false;
    if (sub == FPDF_ANNOT_POPUP) return false;

    bool drawable = (sub == FPDF_ANNOT_INK || sub == FPDF_ANNOT_SQUARE ||
                     sub == FPDF_ANNOT_CIRCLE || sub == FPDF_ANNOT_HIGHLIGHT ||
                     sub == FPDF_ANNOT_LINE || sub == FPDF_ANNOT_POLYGON ||
                     sub == FPDF_ANNOT_FREETEXT || sub == FPDF_ANNOT_TEXT ||
                     sub == FPDF_ANNOT_STAMP);
    if (!drawable) return false;

    double Wd = FPDF_GetPageWidth(page);
    double Hd = FPDF_GetPageHeight(page);
    int rot = FPDFPage_GetRotation(page);
    const QPointF box = pdfBoxOrigin(page);

    out.page = pageIndex;
    out.subtype = sub;
    out.uid = readAnnotString(annot, "TRUID");
    out.hasAP = FPDFAnnot_HasKey(annot, "AP") != 0;

    FS_RECTF r{};
    if (FPDFAnnot_GetRect(annot, &r))
        out.rect = pdfRectToDisp(QRectF(r.left, r.bottom, r.right - r.left, r.top - r.bottom), Wd, Hd, rot, box.x(), box.y());

    unsigned int cr = 0, cg = 0, cb = 0, ca = 255;
    out.hasColor = FPDFAnnot_HasKey(annot, "C") != 0;
    if (FPDFAnnot_GetColor(annot, FPDFANNOT_COLORTYPE_Color, &cr, &cg, &cb, &ca)) {
        out.stroke = QColor(cr, cg, cb, ca);
    } else {
        unsigned long tn = FPDFAnnot_GetStringValue(annot, "TRC", nullptr, 0);
        if (tn > 2) {
            std::vector<unsigned short> tb(tn / 2 + 1, 0);
            FPDFAnnot_GetStringValue(annot, "TRC", reinterpret_cast<FPDF_WCHAR*>(tb.data()), tn);
            QString trc = QString::fromUtf16(reinterpret_cast<const char16_t*>(tb.data()));
            QStringList parts = trc.split(',');
            if (parts.size() == 3)
                out.stroke = QColor(parts[0].toUInt(), parts[1].toUInt(), parts[2].toUInt());
        }
    }

    unsigned int fr = 255, fg = 255, fb = 255, fa = 0;
    out.hasFill = FPDFAnnot_HasKey(annot, "IC") != 0;
    if (FPDFAnnot_HasKey(annot, "IC") &&
        FPDFAnnot_GetColor(annot, FPDFANNOT_COLORTYPE_InteriorColor, &fr, &fg, &fb, &fa) &&
        fa > 0)
        out.fill = QColor(fr, fg, fb, fa);
    else
        out.fill = QColor(Qt::transparent);

    float bh = 0.f, bv = 0.f, bw = 2.f;
    if (FPDFAnnot_GetBorder(annot, &bh, &bv, &bw))
        out.border = bw;

    if (sub == FPDF_ANNOT_INK) {
        int nStrokes = FPDFAnnot_GetInkListCount(annot);
        for (int s = 0; s < nStrokes; ++s) {
            unsigned long pc = FPDFAnnot_GetInkListPath(annot, s, nullptr, 0);
            if (pc == 0) continue;
            std::vector<FS_POINTF> pts(pc);
            FPDFAnnot_GetInkListPath(annot, s, pts.data(), pc);
            QVector<QPointF> stroke;
            for (auto& p : pts)
                stroke.append(pdfToDisp(p.x, p.y, Wd, Hd, rot, box.x(), box.y()));
            out.ink.append(stroke);
        }
    } else if (sub == FPDF_ANNOT_HIGHLIGHT) {
        size_t nQuads = FPDFAnnot_CountAttachmentPoints(annot);
        for (size_t q = 0; q < nQuads; ++q) {
            FS_QUADPOINTSF qp{};
            if (FPDFAnnot_GetAttachmentPoints(annot, q, &qp)) {
                QPointF tl = pdfToDisp(qp.x1, qp.y1, Wd, Hd, rot, box.x(), box.y());
                QPointF tr = pdfToDisp(qp.x2, qp.y2, Wd, Hd, rot, box.x(), box.y());
                QPointF bl = pdfToDisp(qp.x3, qp.y3, Wd, Hd, rot, box.x(), box.y());
                QPointF br = pdfToDisp(qp.x4, qp.y4, Wd, Hd, rot, box.x(), box.y());
                double l = std::min({tl.x(), tr.x(), bl.x(), br.x()});
                double t = std::min({tl.y(), tr.y(), bl.y(), br.y()});
                double r2 = std::max({tl.x(), tr.x(), bl.x(), br.x()});
                double b2 = std::max({tl.y(), tr.y(), bl.y(), br.y()});
                out.quads.append(QRectF(l, t, r2 - l, b2 - t));
            }
        }
    } else if (sub == FPDF_ANNOT_LINE) {
        FS_POINTF ptA{}, ptB{};
        if (FPDFAnnot_GetLine(annot, &ptA, &ptB)) {
            QPointF dA = pdfToDisp(ptA.x, ptA.y, Wd, Hd, rot, box.x(), box.y());
            QPointF dB = pdfToDisp(ptB.x, ptB.y, Wd, Hd, rot, box.x(), box.y());
            out.rect = QRectF(dA, dB);
        }
    }

    if (sub == FPDF_ANNOT_FREETEXT || sub == FPDF_ANNOT_TEXT) {
        out.text = readAnnotString(annot, "Contents");
        if (sub == FPDF_ANNOT_FREETEXT) {
            QColor daColor; float daSize;
            if (parseDA(readAnnotString(annot, "DA"), daColor, daSize)) {
                if (daSize > 0) out.fontSize = daSize;
                if (daColor.isValid()) out.stroke = daColor;
            }
        } else {
            out.isNote = true;
        }
    }

    // Insert Image (SPEC_INSERT_IMAGE_2026-08-30): Stamp annot cung them anh goc
    // de overlay ve (giu kenh alpha). Stamp FORM cua phan mem khac khong co object
    // IMAGE → khong bao hinh, de raster/foreign layer lo ve (khong ve doi).
    if (sub == FPDF_ANNOT_STAMP) {
        captureStampImage(m_doc, page, annot, out.image);
        if (out.image.isNull()) return false;
    }

    // 🔴 2026-09-02 (FTREAL): FreeText CUA TA khong con co HIDDEN → la annot THAT,
    // overlay ve (paintByOverlay=true), nen raster an no qua OwnAnnotHideGuard.
    // FreeText NGOAI (khong TRUID) gi NGUYEN han vi cu: nen raster/lop bu lo.
    // FreeText CO TRUID + co co HIDDEN = tep CU (vat the TRNote trong noi dung
    // trang da ve no o nen) → phai giu paintByOverlay=false, neu khong VE TRUNG.
    // Note (FPDF_ANNOT_TEXT) khong dong trong luot nay.
    if (sub == FPDF_ANNOT_TEXT)
        out.paintByOverlay = false;
    else if (sub == FPDF_ANNOT_FREETEXT)
        out.paintByOverlay = FPDFAnnot_HasKey(annot, "TRUID") != 0 &&
                             !(FPDFAnnot_GetFlags(annot) & FPDF_ANNOT_FLAG_HIDDEN);

    // AutoCAD sinh /Square annot VO HINH (SHX text): /Border=[0,0,0], khong /C,
    // /IC, /AP. Theo chuan khong duoc ve gi len trang (Adobe cung khong ve).
    // Bo qua overlay de khoi bi QPen ep thanh vien 1px do. Phai du CA BON dieu,
    // thieu mot lai co the giau nham annot that. Annot tu app luon co /C (duoc
    // FPDFAnnot_SetColor khi tao) nen khong bao gio dinh luat nay.
    // 🔴 0902: chot nay viet cho annot /Square VO HINH cua AutoCAD, nhung no ap cho
    // MOI subtype — ke ca FreeText. FreeText CO CHU la CO NOI DUNG NHIN THAY DUOC,
    // khong can /C, /IC hay vien. Ap chot nay cho no = tat overlay => chu bien mat.
    // Do duoc 02/09: FreeText moi (chua co /AP vi SetAP that bai, /Border [0,0,0])
    // dinh du CA BON dieu kien => paintByOverlay bi tat => khong ai ve chu.
    if (!out.hasAP && !out.hasColor && !out.hasFill && out.border == 0.0f
        && !(sub == FPDF_ANNOT_FREETEXT && !out.text.isEmpty()))
        out.paintByOverlay = false;

    return true;
}

QList<AnnotVisual> AnnotationManager::loadPageVisuals(int page, bool* outOverlayCapable,
                                                      bool* hasForeign, bool* aborted,
                                                      bool* unloadable, bool heavyPage) {
    QList<AnnotVisual> result;
    if (aborted) *aborted = false;
    // 🔴 LƯỢT 33f (mục 3 — reviewer): phân biệt "thử lại có ích" (tab nền / khoá bận)
    // với "trang hỏng vĩnh viễn" (FPDF_LoadPage null) — vòng hẹn 500 ms vô trần của
    // 33e lặp mãi trên trang hỏng. Ben goi dua tren co nay de danh dau + dung thu.
    if (unloadable) *unloadable = false;
    // 🔴 LƯỢT 33e (mục 1 — reviewer loi 1): MOI duong tra ve ma KHONG doc duoc gi
    // PHAI gan aborted=true. Ben goi (MainWindow finished handler) vui tam ket qua
    // aborted — KHONG duoc ghi visualsCache/visualsRev. Ban 33d tra rong + ghi cache
    // ⇒ tab quay lai bi CACHE HIT danh sach rong ⇒ markup + chu thich ngoai bien mat
    // VINH VIEN (refreshAnnotVisuals ap vao ket qua cu).
    if (!m_doc) {
        if (outOverlayCapable) *outOverlayCapable = false;
        if (hasForeign) *hasForeign = false;
        if (aborted) *aborted = true;
        return result;
    }
    QElapsedTimer _perf;
    _perf.start();

    // 🔴 VIỆC 1 (SPEC_SMOOTH_123 31/08): GUI chi duoc tryLock(0). Neu GUI khong lay duoc
    // khoa → tra rong + overlayCapable=false (ben goi GIU du lieu cu, khong xoa markup),
    // viec that se chay o QtConcurrent (ben goi da co refreshAnnotVisuals). Duong GUI
    // goi truc tiep nay la DEFENSE-IN-DEPTH — MainWindow da day cache-miss sang nen.
    // 🔴 GUI khong duoc blocking-lock (SPEC_SMOOTH_123): chi tryLock(0), khong lay
    // duoc thi tra rong + day sang luong nen (ben goi da co refreshAnnotVisuals).
    // 🔴 0928 LƯỢT 19: HOAN NGUYÊN block ngủ 2 s "nhường urgent" thêm ở LƯỢT 18
    // (VIỆC 3) — do trung hoa (8549 vs 8678 ms, nhieu), ly do ghi ro ben duoi
    // REPORT L18.3: burst xep hang khoa TRUOC khi render kip dat scope urgent.
    // GUI khong bao gio ngu o day: chi luong nen.
    // 🔴 LƯỢT 33b (J): luong NEN (annot visuals) NHƯỜNG khi co luot ve khac dang GIU
    // bo dem urgent — tranh 3 lan FPDF_LoadPage cua CUNG mot trang quai vat (render +
    // annot + thumbnail) xep hang tren MỘT s_pdfiumMutex va chan tab vua mo. GUI khong
    // ngu o day; chi luong nen nhuong, tran ~2s chong doi.
    // 🔴 LƯỢT 33d (J): tab NỀN KHÔNG BẮT ĐẦU loadPageVisuals — FPDF_LoadPage trang
    // quái vật của nó giữ khoá ~1,7 s (đo r33b JKL: :1502 giu ms=1697 đúng lúc f2
    // vừa mở cần khoá). Trần ngủ 75×20 ms của vòng nhường dưới ĐÃ bị trang quái vật
    // đi qua rồi vẫn giành khoá — chốt nền ở đây cắt gốc. Visuals sẽ dựng lại khi
    // tab thành hiện hành (overlay refresh theo lượt vẽ).
    // 🔴 LƯỢT 33e (mục 3 — reviewer loi 3): activeDoc NULL (dau chuyen tiep / tab dang
    // mo) KHONG con la nen. Chi nen THAT (a != null && m_doc != a) moi BO; null + khan
    // ⇒ NHUONG (roi vao vong ngu duoi), null khong khan ⇒ chay tiep. BO phai gan
    // aborted=true (muc 1) — ket qua rong khong bi cache nua, tab quay lai duocquet lai.
    {
        const FPDF_DOCUMENT a = PageCache::activeDoc();
        if (m_doc && a && m_doc != a) {
            qDebug().noquote() << "[annot] loadPageVisuals BO page=" << page
                               << "ly do=tab-nen";
            if (outOverlayCapable) *outOverlayCapable = false;
            if (hasForeign) *hasForeign = false;
            if (aborted) *aborted = true;
            return result;
        }
    }
    if (QThread::currentThread() != QCoreApplication::instance()->thread()) {
        int guard = 0;
        while ((pdfiumUrgentPending()
                || (g_pdfiumUrgentPage().load(std::memory_order_acquire) >= 0
                    && QDateTime::currentMSecsSinceEpoch()
                       <= g_pdfiumUrgentPageDeadline().load(std::memory_order_acquire)))
               && !m_stopScan.load()          // 🔴 LƯỢT 33d (K): shutdownTab/đổi tab
               && guard++ < 75)               // phải cắt giấc ngủ này ngay, không chờ 1,5 s
            QThread::msleep(20);
        // 🔴 LƯỢT 33d (J): sau giấc nhường, tab có thể ĐÃ thành nền — kiểm lại
        // trước khi giành khoá (bản 33b bỏ qua bước này, tỉnh dậy rồi FPDF_LoadPage
        // trang quái vật 1,7 s ngay trong cửa sổ f2 cần khoá).
        // 🔴 LƯỢT 33e (muc 3): null + van khan (mo doc keo dai qua tran nhuong) ⇒ BO
        // CO CỜ aborted — khong phai bo vinh vien: duong doi tab (refreshAnnotVisuals)
        // va duong thu lai trong finished handler se quet lai khi khan da xong.
        const FPDF_DOCUMENT a2 = PageCache::activeDoc();
        if (m_doc && (a2 ? m_doc != a2 : pdfiumUrgentPending())) {
            qDebug().noquote() << "[annot] loadPageVisuals BO page=" << page
                               << "ly do=tab-nen-sau-nhuong";
            if (outOverlayCapable) *outOverlayCapable = false;
            if (hasForeign) *hasForeign = false;
            if (aborted) *aborted = true;
            return result;
        }
        // 🔴 LƯỢT 33f (muc 1 — do full probe r33f): trang QUAI VAT (>=1M object —
        // pre-count cua render da biet) parse MAIN-DOC 1,65 s DEM KHOA: do
        // `[annot] AnnotationManager.cpp:1582 giu ms= 1651` +
        // `[perf] loadPageVisuals page=3 count= 0 ms= 1651` (KHONG co chu thich nao!)
        // trong luc flip trang ke tiep `lockwait ms= 1650` ⇒ page 4 tu 1135 len 2695 ms.
        // Giac nhuong 1,5 s o tren ket thuc giua luc render van GIU BO DEM ⇒ tranh
        // khoa giua hai lat la dung goc gai. Van con render active ⇒ BO CO CO —
        // dung tranh; thu lai co tran/backoff (muc 3) chi chay khi trang duoc xem,
        // quay lai trang se duoc quet luc render yen lang. Trang nhe khong bi chan.
        if (heavyPage && pdfiumUrgentPending()) {
            qDebug().noquote() << "[annot] loadPageVisuals BO page=" << page
                               << "ly do=quai-vat-trong-luc-render-ban";
            if (outOverlayCapable) *outOverlayCapable = false;
            if (hasForeign) *hasForeign = false;
            if (aborted) *aborted = true;
            return result;
        }
    }
    TryPdfiumLock lock(__FILE__, __LINE__);
    if (!lock.held()) {
        qDebug().noquote() << "[annot] loadPageVisuals SKIP — khoa ban page=" << page
                           << "(day sang luong nen)";
        if (outOverlayCapable) *outOverlayCapable = false;
        if (hasForeign) *hasForeign = false;
        if (aborted) *aborted = true;        // 🔴 LƯỢT 33e (muc 1): khoa ban cung la BO
        return result;
    }
    QElapsedTimer _w; _w.start();
    FPDF_PAGE fpage = PageCache::acquire(m_doc, page);
    if (!fpage) {
        if (outOverlayCapable) *outOverlayCapable = false;
        if (hasForeign) *hasForeign = false;
        if (aborted) *aborted = true;        // 🔴 LƯỢT 33f (muc 3): doc trang hong — KHONG thu lai (unloadable)
        if (unloadable) *unloadable = true;  // 🔴 LƯỢT 33f (muc 3): hong VINH VIEN — dung thu lai
        return result;
    }
    PageCache::PageBorrow _b(m_doc, page);   // R1: cap doi acquire()

    int count = FPDFPage_GetAnnotCount(fpage);
    bool capable = true;

    for (int i = 0; i < count; ++i) {
        FPDF_ANNOTATION annot = FPDFPage_GetAnnot(fpage, i);
        if (!annot) continue;

        int sub = FPDFAnnot_GetSubtype(annot);
        // TAP drawable duoc dinh nghia O MOT CHO duy nhat: isOverlayDrawnAnnot()
        // (OwnAnnotHideGuard.h) — KHONG dinh nghia lai o day (SPEC_OVERLAY_LAYER 31/08).
        // 🔴 2026-09-02 (FTREAL): FreeText CUA TA (TRUID, khong HIDDEN) DA THUOC TAP
        // drawable — buildVisual() cho paintByOverlay=true. Tep CU (FreeText HIDDEN +
        // vat the TRNote) van ngoai tap, nen khong bi an o nen cung khong bi overlay ve.
        // ⚠️ Neu sau nay doi tap nay, phai sua CA HAI cho: isOverlayDrawnAnnot() VA
        //    `paintByOverlay` trong buildVisual().
        //
        // 🔴 capable co nghia: "overlay ve duoc MỌI markup CỦA TA (co TRUID) tren trang".
        //    Annot NGOAI (KHONG TRUID) KHONG duoc hạ capable — no nam trong anh nen, ve
        //    MOT LAN luc mo trang, thêm/xoa markup cua ta khong dung toi no (SPEC_OVERLAY_LAYER).
        //    Annot khong /AP khong tao ra pixel nao => khong duoc hạ capable
        //    (mat fast-path vo co). LINK va POPUP cung khong ve ra pixel nao (Link
        //    khong co /AP, PDFium cung khong tu sinh AP) => giu nguyen loai tru.
        //    Chi hạ capable khi annot CO TRUID (markup cua ta) ma overlay khong ve duoc
        //    (vi du FreeText do chinh ta tao) — truong hop do van phai di duong raster.
        //    Dung chung isOverlayDrawnAnnot() voi anh nen (PdfRenderer) de KHONG co hai
        //    ban logic. (Bệnh cũ 30/08 da tra gia: FreeText ngoai /AP lam ca trang mat
        //    fast-path => ve 1 rectangle lai render 2,54 trieu doi tuong 6 giay.)
        if (FPDFAnnot_HasKey(annot, "TRUID")
            && !isOverlayDrawnAnnot(annot)
            && FPDFAnnot_HasKey(annot, "AP") != 0
            && !(FPDFAnnot_GetFlags(annot) & FPDF_ANNOT_FLAG_HIDDEN)
            && sub != FPDF_ANNOT_POPUP && sub != FPDF_ANNOT_LINK) {
            capable = false;
            qDebug() << "[annot] page=" << page << "overlayCapable=0 reason=subtype=" << sub;
        }

        AnnotVisual av;
        if (buildVisual(fpage, annot, page, av))
            result.append(av);

        FPDFPage_CloseAnnot(annot);
    }

    int foreignCount = 0;
    if (hasForeign) {
        for (int i = 0; i < count; ++i) {
            FPDF_ANNOTATION a = FPDFPage_GetAnnot(fpage, i);
            if (!a) continue;
            // Chi tinh annot THUC SU co hinh de ve (co /AP). Annot khong co /AP
            // (Link, Widget rong...) khong tao ra pixel nao => dung lop bu la vo ich.
            if (FPDFAnnot_HasKey(a, "TRUID") == 0
                && !(FPDFAnnot_GetFlags(a) & FPDF_ANNOT_FLAG_HIDDEN)
                && FPDFAnnot_GetSubtype(a) != FPDF_ANNOT_POPUP
                && FPDFAnnot_HasKey(a, "AP") != 0)
                ++foreignCount;
            FPDFPage_CloseAnnot(a);
        }
        *hasForeign = (foreignCount > 0);
    }

    // 🔴 LÁT C 0902: anh cat nay DA bao tram moi thay doi ghi truoc luc no lay khoa
    // (ca hai deu nam trong s_pdfiumMutex) → delta cu khong con nghia, don bo.
    // Moi thay doi SAU nay se vao hang delta khi luong tao/xoa lay duoc khoa.
    { QMutexLocker dl(&m_deltaMutex); m_visualDeltas.remove(page); }

    lock.unlock();

    qint64 ms = _perf.elapsed();
    qDebug().noquote() << QString("[perf] loadPageVisuals page=%1 count=%2 capable=%3 ms=%4")
                              .arg(page).arg(result.size()).arg(capable ? 1 : 0).arg(ms);
    if (outOverlayCapable) *outOverlayCapable = capable;
    return result;
}

bool AnnotationManager::createPopupNote(int pageIndex, QPointF pointDisp,
                                         const QString& text, const QString& author) {
    if (!m_doc) return false;
    QElapsedTimer _w; _w.start();
    TimedPdfiumLock lock(__FILE__, __LINE__);
    if (_w.elapsed() > 300)
        qDebug().noquote() << "[lockwait] ms=" << _w.elapsed()
                           << "at" << __FILE__ << ":" << __LINE__ << "main="
                           << ((QThread::currentThread() == QCoreApplication::instance()->thread()) ? 1 : 0);
    FPDF_PAGE page = PageCache::acquire(m_doc, pageIndex);
    if (!page) { m_lastError = "Cannot load page"; return false; }
    PageCache::PageBorrow _b(m_doc, pageIndex);   // R1: cap doi acquire()
    AnnotInfo info;
    bool ok = createPopupNote_locked(page, pageIndex, pointDisp, text, author, &info);
    if (ok) {
        setOwnNoteObjectsActive(page, true);
        QElapsedTimer _gt; _gt.start();
        FPDFPage_GenerateContent(page);
        qDebug().noquote() << "[perf] genContent page=" << pageIndex << "ms=" << _gt.elapsed();
        // Trang da bi sua: bo entry cu (cac he khac khong dung handle cu) roi nap lai cho am.
        PageCache::bumpAnnotGeneration(m_doc, pageIndex);
        recordCreatedVisual_locked(page, pageIndex);   // LÁT C 0902: delta, µs trong khoa DA giu
    }
    lock.unlock();
    if (ok) {
        bumpPageRevision(pageIndex);
        emit annotationAdded(pageIndex, info);
    }
    return ok;
}

bool AnnotationManager::createInlineNote(int pageIndex, QRectF rectPdf,
                                          const QString& textIn, const QString& author,
                                          bool withBackground, QColor textColor,
                                          float fontSize) {
    if (!m_doc) return false;
    QElapsedTimer _totalT; _totalT.start();
    QElapsedTimer _w; _w.start();
    TimedPdfiumLock lock(__FILE__, __LINE__);
    if (_w.elapsed() > 300)
        qDebug().noquote() << "[lockwait] ms=" << _w.elapsed()
                           << "at" << __FILE__ << ":" << __LINE__ << "main="
                           << ((QThread::currentThread() == QCoreApplication::instance()->thread()) ? 1 : 0);
    FPDF_PAGE page = PageCache::acquire(m_doc, pageIndex);
    if (!page) { m_lastError = "Cannot load page"; return false; }
    PageCache::PageBorrow _b(m_doc, pageIndex);   // R1: cap doi acquire()
    AnnotInfo info;
    bool ok = createInlineNote_locked(page, pageIndex, rectPdf, textIn, author,
                                       withBackground, textColor, fontSize, &info);
    if (ok) {
        setOwnNoteObjectsActive(page, true);
        QElapsedTimer _gt2; _gt2.start();
        FPDFPage_GenerateContent(page);
        qDebug().noquote() << "[perf] genContent page=" << pageIndex << "ms=" << _gt2.elapsed();
        invalidateNoteObjCache_locked(pageIndex);
    }
    int finalAnnotCount = FPDFPage_GetAnnotCount(page);
    if (ok) {
        PageCache::bumpAnnotGeneration(m_doc, pageIndex);
        recordCreatedVisual_locked(page, pageIndex);   // LÁT C 0902
    }
    lock.unlock();
    if (ok) {
        bumpPageRevision(pageIndex);
        emit annotationAdded(pageIndex, info);
    }
    qDebug() << "[inote] done annotCount=" << finalAnnotCount;
    qDebug().noquote() << "[perf] note createInlineNote total ms=" << _totalT.elapsed();
    return ok;
}

bool AnnotationManager::rebuildTextNote(int pageIndex, int index, QColor newColor, float newFontSize) {
    if (!m_doc) return false;
    // 🔴 VIỆC 1 (0921): CỬA DUY NHẤT TRƯỚC khi lấy khoá/chạm annot.
    if (!guardWrite(pageIndex, index, AnnotWrite::Style)) return false;
    QElapsedTimer _totalT; _totalT.start();

    QElapsedTimer _w; _w.start();
    TimedPdfiumLock lock(__FILE__, __LINE__);
    if (_w.elapsed() > 300)
        qDebug().noquote() << "[lockwait] ms=" << _w.elapsed()
                           << "at" << __FILE__ << ":" << __LINE__ << "main="
                           << ((QThread::currentThread() == QCoreApplication::instance()->thread()) ? 1 : 0);
    FPDF_PAGE page = PageCache::acquire(m_doc, pageIndex);
    if (!page) return false;
    PageCache::PageBorrow _b(m_doc, pageIndex);   // R1: cap doi acquire()

    FPDF_ANNOTATION annot = FPDFPage_GetAnnot(page, index);
    if (!annot) return false;
    if (FPDFAnnot_GetSubtype(annot) != FPDF_ANNOT_FREETEXT) {
        FPDFPage_CloseAnnot(annot); return false;
    }
    // VIỆC 1: quyền sở hữu đã chốt ở guardWrite() đầu hàm.

    FS_RECTF r{};
    FPDFAnnot_GetRect(annot, &r);
    QString contents = readAnnotString(annot, "Contents");
    QString author = readAnnotString(annot, "T");
    QString tridStr = readAnnotString(annot, "TRID");
    QString oldUid = readAnnotString(annot, "TRUID");
    unsigned int noteId = tridStr.toUInt();
    double pageH = FPDF_GetPageHeight(page);
    double pageW = FPDF_GetPageWidth(page);
    int rot = FPDFPage_GetRotation(page);
    const QPointF box = pdfBoxOrigin(page);

    QPointF d1 = pdfToDisp(r.left, r.bottom, pageW, pageH, rot, box.x(), box.y());
    QPointF d2 = pdfToDisp(r.right, r.top, pageW, pageH, rot, box.x(), box.y());
    QRectF dispRect = QRectF(d1, d2).normalized();
    double wDisp = (std::max)(24.0, contents.length() * newFontSize * 0.55 + 8.0);
    double hDisp = newFontSize * 1.5 + 4.0;
    QRectF fitRect(dispRect.topLeft(), QSizeF(wDisp, hDisp));

    FPDFPage_CloseAnnot(annot);

    if (noteId)
        removeNotePageObjects_locked(page, noteId);

    bool dummyNeedsGen = false;
    removeAnnot_locked(page, index, &dummyNeedsGen);

    AnnotInfo info;
    const bool created = createInlineNote_locked(page, pageIndex, fitRect, contents, author,
                                                 false, newColor, newFontSize, &info);
    // 🔴 P2 (0921): TẠO THẤT BẠI ⇒ annot cũ đã bị xoá; TUYỆT ĐỐI không đóng dấu /TRUID lên
    // `GetAnnot(nc-1)` vì đó là annot CUỐI TRANG — có thể là CỦA ĐỐI TÁC — sẽ bị nhận vơ.
    if (!created) {
        m_lastError = QStringLiteral("rebuildTextNote: không tạo lại được ghi chú");
        return false;
    }

    if (!oldUid.isEmpty()) {
        int nc = FPDFPage_GetAnnotCount(page);
        if (nc > 0) {
            FPDF_ANNOTATION a2 = FPDFPage_GetAnnot(page, nc - 1);
            if (a2) {
                FPDFAnnot_SetStringValue(a2, "TRUID", reinterpret_cast<FPDF_WIDESTRING>(oldUid.utf16()));
                m_lastCreatedUid = oldUid;
                FPDFPage_CloseAnnot(a2);
            }
        }
    }

    setOwnNoteObjectsActive(page, true);
    QElapsedTimer _gt; _gt.start();
    FPDFPage_GenerateContent(page);
    qDebug().noquote() << "[perf] genContent page=" << pageIndex << "ms=" << _gt.elapsed();
    invalidateNoteObjCache_locked(pageIndex);
    PageCache::bumpAnnotGeneration(m_doc, pageIndex);
    recordCreatedVisual_locked(page, pageIndex);   // LÁT C 0902: uid giu nguyen → thay trong cache

    lock.unlock();
    bumpPageRevision(pageIndex);
    emit annotationAdded(pageIndex, info);
    qDebug().noquote() << "[perf] note rebuildTextNote total ms=" << _totalT.elapsed();
    return true;
}

bool AnnotationManager::moveAnnot(int pageIndex, int index, double dxU, double dyU) {
    if (!m_doc) return false;
    // 🔴 VIỆC 1 + VIỆC 3-A2 (0921): CỬA DUY NHẤT, gọi TRƯỚC `FPDFAnnot_SetRect`.
    // annot NGOÀI ⇒ TỪ CHỐI. Lý do: PDFium chỉ dịch được /Rect; các trường hình học
    // khác (/QuadPoints, /L, /Vertices, /CL, /Popup) KHÔNG có API dịch ⇒ Acrobat sẽ
    // vẽ lại từ hình học gốc và kéo annot về chỗ cũ. "Trường nào không dịch được thì
    // TỪ CHỐI kéo" (owner) ⇒ từ chối mọi annot ngoài, không im lặng SetRect.
    if (!guardWrite(pageIndex, index, AnnotWrite::Geometry)) {
        qDebug().noquote() << "[guard] moveAnnot TU CHOI (annot ngoai) page=" << pageIndex
                           << "idx=" << index;
        return false;
    }
    QElapsedTimer _totalT; _totalT.start();
    TimedPdfiumLock lock(__FILE__, __LINE__);
    FPDF_PAGE page = PageCache::acquire(m_doc, pageIndex);
    if (!page) return false;
    PageCache::PageBorrow _b(m_doc, pageIndex);   // R1: cap doi acquire()
    FPDF_ANNOTATION a = FPDFPage_GetAnnot(page, index);
    if (!a) return false;

    FS_RECTF r{};
    const bool gotRect = FPDFAnnot_GetRect(a, &r);
    const FS_RECTF rOrig = r;          // để HOÀN LẠI nếu bước sau thất bại
    bool ok = gotRect;
    const int sub = FPDFAnnot_GetSubtype(a);

    if (sub == FPDF_ANNOT_INK && !FPDFAnnot_HasKey(a, "TRUID") && !FPDFAnnot_HasKey(a, "TRID")) {
        // Ink cua phan mem khac co /AP rieng — remove+recreate se lam mat.
        // 🔴 P1 (0921): TU CHOI *TRUOC* khi cham /Rect. Truoc day SetRect da chay roi
        // moi return false ⇒ /Rect da dich va nguoi dung luu vi ly do khac thi ban dich
        // di vao tep (khong bumpPageRevision, khong dirty). Gio khong cham gi ca.
        FPDFPage_CloseAnnot(a);
        return false;
    }

    if (ok) {
        r.left += (float)dxU; r.right  += (float)dxU;
        r.top  += (float)dyU; r.bottom += (float)dyU;
        ok = FPDFAnnot_SetRect(a, &r);
    }

    if (sub == FPDF_ANNOT_INK) {
        // INK: /InkList la toa do tuyet doi — dich /Rect khong ke net theo.
        // PDFium khong co API sua InkList tai cho → snapshot + remove + recreate.
        AnnotSnapshot s;
        s.subtype = sub;
        FS_RECTF sr{};
        if (FPDFAnnot_GetRect(a, &sr)) { s.rl = sr.left; s.rt = sr.top; s.rr = sr.right; s.rb = sr.bottom; }
        s.hasColor = FPDFAnnot_HasKey(a, "C") && FPDFAnnot_GetColor(a, FPDFANNOT_COLORTYPE_Color, &s.r, &s.g, &s.b, &s.a) != 0;
        s.hasFill = FPDFAnnot_HasKey(a, "IC") && FPDFAnnot_GetColor(a, FPDFANNOT_COLORTYPE_InteriorColor, &s.fr, &s.fg, &s.fb, &s.fa) != 0 && s.fa > 0;
        { float bh=0,bv=0,bw=0; if(FPDFAnnot_GetBorder(a,&bh,&bv,&bw)) s.border=bw; }
        { unsigned long l=FPDFAnnot_GetStringValue(a,"Contents",nullptr,0);
          if(l>2){std::vector<unsigned short> b(l/2+1,0); FPDFAnnot_GetStringValue(a,"Contents",reinterpret_cast<FPDF_WCHAR*>(b.data()),l);
          s.contents=QString::fromUtf16(reinterpret_cast<const char16_t*>(b.data()));} }
        { unsigned long l=FPDFAnnot_GetStringValue(a,"DA",nullptr,0);
          if(l>2){std::vector<unsigned short> b(l/2+1,0); FPDFAnnot_GetStringValue(a,"DA",reinterpret_cast<FPDF_WCHAR*>(b.data()),l);
          s.da=QString::fromUtf16(reinterpret_cast<const char16_t*>(b.data()));} }
        { unsigned long l=FPDFAnnot_GetStringValue(a,"TRUID",nullptr,0);
          if(l>2){std::vector<unsigned short> b(l/2+1,0); FPDFAnnot_GetStringValue(a,"TRUID",reinterpret_cast<FPDF_WCHAR*>(b.data()),l);
          s.uid=QString::fromUtf16(reinterpret_cast<const char16_t*>(b.data()));} }
        int nStrokes = FPDFAnnot_GetInkListCount(a);
        for (int i = 0; i < nStrokes; ++i) {
            unsigned long pc = FPDFAnnot_GetInkListPath(a, i, nullptr, 0);
            if (pc > 0) {
                std::vector<FS_POINTF> pts(pc);
                FPDFAnnot_GetInkListPath(a, i, pts.data(), pc);
                QVector<QPointF> qp;
                for (auto& p : pts) qp.append(QPointF(p.x + dxU, p.y + dyU));
                s.ink.append(qp);
            }
        }
        s.isDraft = FPDFAnnot_HasKey(a, "TRSD") != 0;
        s.valid = true;
        FPDFPage_CloseAnnot(a);

        if (!FPDFPage_RemoveAnnot(page, index)) {
            // 🔴 P1 (0921): /Rect da dich nhung khong remove duoc annot ⇒ HOÀN LẠI
            // /Rect gốc để tệp không mang theo một bản dịch dở dang.
            if (gotRect) {
                FPDF_ANNOTATION ar = FPDFPage_GetAnnot(page, index);
                if (ar) { FS_RECTF back = rOrig; FPDFAnnot_SetRect(ar, &back); FPDFPage_CloseAnnot(ar); }
            }
            return false;
        }

        FPDF_ANNOTATION a2 = FPDFPage_CreateAnnot(page, FPDF_ANNOT_INK);
        if (a2) {
            FS_RECTF nr{s.rl, s.rt, s.rr, s.rb}; // s.* da la rect DA DICH o dau ham — cong them nua se thanh 2x.
            FPDFAnnot_SetRect(a2, &nr);
            if (s.hasColor) {
                FPDFAnnot_SetColor(a2, FPDFANNOT_COLORTYPE_Color, s.r, s.g, s.b, s.a);
                QString trc = QString("%1,%2,%3").arg(s.r).arg(s.g).arg(s.b);
                FPDFAnnot_SetStringValue(a2, "TRC", reinterpret_cast<FPDF_WIDESTRING>(trc.utf16()));
            }
            if (s.hasFill)
                FPDFAnnot_SetColor(a2, FPDFANNOT_COLORTYPE_InteriorColor, s.fr, s.fg, s.fb, s.fa);
            FPDFAnnot_SetBorder(a2, 0.0f, 0.0f, s.border);
            if (!s.contents.isEmpty())
                FPDFAnnot_SetStringValue(a2, "Contents", reinterpret_cast<FPDF_WIDESTRING>(s.contents.utf16()));
            if (!s.da.isEmpty())
                FPDFAnnot_SetStringValue(a2, "DA", reinterpret_cast<FPDF_WIDESTRING>(s.da.utf16()));
            if (!s.uid.isEmpty())
                FPDFAnnot_SetStringValue(a2, "TRUID", reinterpret_cast<FPDF_WIDESTRING>(s.uid.utf16()));
            if (s.isDraft)
                FPDFAnnot_SetStringValue(a2, "TRSD", reinterpret_cast<FPDF_WIDESTRING>(QStringLiteral("1").utf16()));
            for (const auto& stroke : s.ink) {
                std::vector<FS_POINTF> pts;
                for (const auto& p : stroke) pts.push_back(FS_POINTF{static_cast<float>(p.x()), static_cast<float>(p.y())});
                if (pts.size() >= 2) FPDFAnnot_AddInkStroke(a2, pts.data(), pts.size());
            }
            FPDFPage_CloseAnnot(a2);
        }
        PageCache::bumpAnnotGeneration(m_doc, pageIndex);
        
        if (a2) bumpPageRevision(pageIndex);
        return a2 != nullptr;
    }

    unsigned int noteId = readAnnotString(a, "TRID").toUInt();
    FPDFPage_CloseAnnot(a);

    int moved = 0;
    if (noteId) moved = translateNotePageObjects_locked(page, pageIndex, noteId, dxU, dyU);
    if (moved > 0) {
        setOwnNoteObjectsActive(page, true);
        QElapsedTimer _gt; _gt.start();
        FPDFPage_GenerateContent(page);
        qDebug().noquote() << "[perf] genContent page=" << pageIndex << "ms=" << _gt.elapsed();
    }

    if (ok) {
        PageCache::bumpAnnotGeneration(m_doc, pageIndex);
        
    }
    if (ok) bumpPageRevision(pageIndex);
    qDebug().noquote() << "[perf] note moveAnnot total ms=" << _totalT.elapsed();
    return ok;
}

QString AnnotationManager::insertStampImage(int pageIndex, const QImage& image,
                                            QRectF rectDisp) {
    if (!m_doc || image.isNull()) { m_lastError = "Image is empty"; return QString(); }
    QElapsedTimer _w; _w.start();
    TimedPdfiumLock lock(__FILE__, __LINE__);
    if (_w.elapsed() > 300)
        qDebug().noquote() << "[lockwait] ms=" << _w.elapsed()
                           << "at" << __FILE__ << ":" << __LINE__ << "main="
                           << ((QThread::currentThread() == QCoreApplication::instance()->thread()) ? 1 : 0);
    FPDF_PAGE page = PageCache::acquire(m_doc, pageIndex);
    if (!page) { m_lastError = "Cannot load page"; return QString(); }
    PageCache::PageBorrow _b(m_doc, pageIndex);   // R1: cap doi acquire()

    const double pageW = FPDF_GetPageWidth(page);
    const double pageH = FPDF_GetPageHeight(page);
    const int    rot   = FPDFPage_GetRotation(page);
    const QPointF box  = pdfBoxOrigin(page);

    // rectDisp (Y-down) -> rect PDF chua xoay qua 4 goc (dung doi phap createInlineNote).
    QPointF tl = dispToPdf(rectDisp.left(),  rectDisp.top(),    pageW, pageH, rot, box.x(), box.y());
    QPointF tr = dispToPdf(rectDisp.right(), rectDisp.top(),    pageW, pageH, rot, box.x(), box.y());
    QPointF bl = dispToPdf(rectDisp.left(),  rectDisp.bottom(), pageW, pageH, rot, box.x(), box.y());
    QPointF br = dispToPdf(rectDisp.right(), rectDisp.bottom(), pageW, pageH, rot, box.x(), box.y());
    double xu_min = (std::min)({tl.x(), tr.x(), bl.x(), br.x()});
    double xu_max = (std::max)({tl.x(), tr.x(), bl.x(), br.x()});
    double yu_min = (std::min)({tl.y(), tr.y(), bl.y(), br.y()});
    double yu_max = (std::max)({tl.y(), tr.y(), bl.y(), br.y()});
    // Toi thieu 20x20 pt (de quan ly bang tay nam):
    if (xu_max - xu_min < 20.0) { double c = (xu_min + xu_max) / 2.0; xu_min = c - 10.0; xu_max = c + 10.0; }
    if (yu_max - yu_min < 20.0) { double c = (yu_min + yu_max) / 2.0; yu_min = c - 10.0; yu_max = c + 10.0; }

    FPDF_ANNOTATION annot = FPDFPage_CreateAnnot(page, FPDF_ANNOT_STAMP);
    if (!annot) { m_lastError = "Cannot create Stamp annotation"; PageCache::bumpAnnotGeneration(m_doc, pageIndex); return QString(); }

    FS_RECTF rr{ static_cast<float>(xu_min), static_cast<float>(yu_max),
                 static_cast<float>(xu_max), static_cast<float>(yu_min) };
    FPDFAnnot_SetRect(annot, &rr);

    const double w = xu_max - xu_min, h = yu_max - yu_min;
    FPDF_PAGEOBJECT obj = makeStampImageObject(m_doc, page, image,
                                               w / double(qMax(1, image.width())),
                                               h / double(qMax(1, image.height())),
                                               xu_min, yu_min);
    if (!obj) {
        m_lastError = "Cannot build image object";
        FPDFPage_CloseAnnot(annot);
        PageCache::bumpAnnotGeneration(m_doc, pageIndex);
        return QString();
    }

    // Anh goc vao annot content stream: /AP tu dong sinh ra tu object nay,
    // giu nguyen kenh alpha (overlay hien thi doc lai qua captureStampImage).
    FPDFAnnot_AppendObject(annot, obj);

    m_lastCreatedUid = generateUid();
    FPDFAnnot_SetStringValue(annot, "TRUID", reinterpret_cast<FPDF_WIDESTRING>(m_lastCreatedUid.utf16()));
    FPDFAnnot_SetStringValue(annot, "TRTOOL", reinterpret_cast<FPDF_WIDESTRING>(QStringLiteral("Stamp").utf16()));

    FPDFPage_CloseAnnot(annot);

    // Sinh /AP ngay de file luu ra hien hinh + giu kenh alpha (ProcessAnnotation
    // trong GenerateContent tao /AP/N form cua annot). PHAI truoc invalidate —
    // invalidate pha huy page dang dang giu.
    QElapsedTimer _gt; _gt.start();
    FPDFPage_GenerateContent(page);
    qDebug().noquote() << "[imgstamp] insert page=" << pageIndex
                       << "genMs=" << _gt.elapsed() << "uid=" << m_lastCreatedUid;

    // Chup index + snapshot TRUOC invalidate — nguoi goi (MainWindow) khong phai goi
    // findAnnotIndexByUid/snapshotAnnot sau commit (trang vua invalidate => re-load 2 giay).
    m_lastCreatedIndex = FPDFPage_GetAnnotCount(page) - 1;
    m_lastCreatedSnapshot = snapshotAnnot_locked(page, pageIndex, m_lastCreatedIndex);

    PageCache::bumpAnnotGeneration(m_doc, pageIndex);
    recordCreatedVisual_locked(page, pageIndex);   // LÁT C 0902: Stamp co anh trong visual → overlay ve ngay

    lock.unlock();
    bumpPageRevision(pageIndex);
    return m_lastCreatedUid;
}

bool AnnotationManager::setAnnotRectDisplay(int pageIndex, int index, QRectF rectDisp) {
    if (!m_doc) return false;
    // 🔴 VIỆC 1 + VIỆC 3-A1 (0921): CỬA DUY NHẤT, gọi TRƯỚC `FPDFAnnot_SetRect`.
    // FreeText/Stamp NGOÀI co giãn chỉ đổi /Rect mà /AP giữ nguyên ⇒ hình bị kéo giãn
    // méo; owner chốt: không cho co giãn annot ngoài. Từ chối tại backend.
    if (!guardWrite(pageIndex, index, AnnotWrite::Rect)) {
        qDebug().noquote() << "[guard] setAnnotRectDisplay TU CHOI (annot ngoai) page=" << pageIndex
                           << "idx=" << index;
        return false;
    }
    TimedPdfiumLock lock(__FILE__, __LINE__);
    FPDF_PAGE page = PageCache::acquire(m_doc, pageIndex);
    if (!page) return false;
    PageCache::PageBorrow _b(m_doc, pageIndex);   // R1: cap doi acquire()
    FPDF_ANNOTATION a = FPDFPage_GetAnnot(page, index);
    if (!a) return false;

    const double pageW = FPDF_GetPageWidth(page);
    const double pageH = FPDF_GetPageHeight(page);
    const int    rot   = FPDFPage_GetRotation(page);
    const QPointF box  = pdfBoxOrigin(page);
    QPointF tl = dispToPdf(rectDisp.left(),  rectDisp.top(),    pageW, pageH, rot, box.x(), box.y());
    QPointF tr = dispToPdf(rectDisp.right(), rectDisp.top(),    pageW, pageH, rot, box.x(), box.y());
    QPointF bl = dispToPdf(rectDisp.left(),  rectDisp.bottom(), pageW, pageH, rot, box.x(), box.y());
    QPointF br = dispToPdf(rectDisp.right(), rectDisp.bottom(), pageW, pageH, rot, box.x(), box.y());
    double xu_min = (std::min)({tl.x(), tr.x(), bl.x(), br.x()});
    double xu_max = (std::max)({tl.x(), tr.x(), bl.x(), br.x()});
    double yu_min = (std::min)({tl.y(), tr.y(), bl.y(), br.y()});
    double yu_max = (std::max)({tl.y(), tr.y(), bl.y(), br.y()});
    if (xu_max - xu_min < 20.0) { double c = (xu_min + xu_max) / 2.0; xu_min = c - 10.0; xu_max = c + 10.0; }
    if (yu_max - yu_min < 20.0) { double c = (yu_min + yu_max) / 2.0; yu_min = c - 10.0; yu_max = c + 10.0; }

    FS_RECTF nr{ static_cast<float>(xu_min), static_cast<float>(yu_max),
                 static_cast<float>(xu_max), static_cast<float>(yu_min) };
    bool ok = FPDFAnnot_SetRect(a, &nr);
    FPDFPage_CloseAnnot(a);
    PageCache::bumpAnnotGeneration(m_doc, pageIndex);

    if (ok) bumpPageRevision(pageIndex);
    return ok;
}

int AnnotationManager::annotCount(int pageIndex) {
    if (!m_doc) return 0;
    TimedPdfiumLock lock(__FILE__, __LINE__);
    FPDF_PAGE page = PageCache::acquire(m_doc, pageIndex);
    if (!page) return 0;
    PageCache::PageBorrow _b(m_doc, pageIndex);   // R1: cap doi acquire()
    int count = FPDFPage_GetAnnotCount(page);
    return count;
}

bool AnnotationManager::isOwnAnnot(int pageIndex, int index) {
    if (!m_doc) return false;
    TimedPdfiumLock lock(__FILE__, __LINE__);
    FPDF_PAGE page = PageCache::acquire(m_doc, pageIndex);
    if (!page) return false;
    PageCache::PageBorrow _b(m_doc, pageIndex);   // R1: cap doi acquire()
    FPDF_ANNOTATION annot = FPDFPage_GetAnnot(page, index);
    if (!annot) return false;
    bool own = FPDFAnnot_HasKey(annot, "TRUID") || FPDFAnnot_HasKey(annot, "TRID");
    FPDFPage_CloseAnnot(annot);
    return own;
}

// VIỆC 1 (0921): bản KHÔNG tự lấy khoá — gọi từ vùng đã giữ s_pdfiumMutex.
// 🔴 0927 LUOT 6 (VIEC 1) - MOC NEO FONT DejaVu cho FreeText NGOAI.
// 🔴 DO REVIEWER 27/09 BAT: setAnnotContents() (duong sua CHUOT PHAI sidebar)
// danh dau /TR_AP_REBUILD nhung THIEU khoi nay. rebuildForeignFreeTextAp() goi
// findDejaVuFont(page) de tim font de nhung vao /AP moi; tren trang "sach" (chua
// tung co FreeText cua ta) /Resources khong co DejaVu => failed++ => SAVE THAT BAI
// du nguoi dung chi sua chu. retextNote() da co khoi nay tu 21/09 nen chi save
// duong chuot phai chiu loi. TACH RA MOT HAM de hai duong khong the lech nhau.
void AnnotationManager::ensureDejaVuFontAnchor_locked(FPDF_PAGE page, int pageIndex) {
    if (m_apAnchorPages.contains(pageIndex)) return;
    FPDF_FONT font = unicodeFont();
    if (font) {
        FPDF_PAGEOBJECT anchor = FPDFPageObj_CreateTextObj(m_doc, font, 15.0);
        if (anchor) {
            static const FPDF_WCHAR s_space[2] = { ' ', 0 };
            FPDFText_SetText(anchor, const_cast<FPDF_WCHAR*>(s_space));
            FPDFPageObj_Transform(anchor, 1.0, 0.0, 0.0, 1.0, 0.0, 0.0);
            FPDFPage_InsertObject(page, anchor);
        }
    }
    m_apAnchorPages.insert(pageIndex);
}

bool AnnotationManager::guardWrite_locked(FPDF_ANNOTATION annot, AnnotWrite what, QString* whyNot) {
    auto deny = [&](const QString& m) {
        if (whyNot) *whyNot = m;
        m_lastError = m;
        return false;
    };
    if (!annot) return deny(QStringLiteral("guardWrite: không có annot"));
    const bool own = FPDFAnnot_HasKey(annot, "TRUID") || FPDFAnnot_HasKey(annot, "TRID");
    if (own) return true;
    switch (what) {
        case AnnotWrite::Delete:
            // owner chốt (VIỆC 3-A3): VẪN cho xoá annot ngoài, nhưng "backup rồi xoá" —
            // trách nhiệm sao lưu nguyên vẹn thuộc về caller trước khi gọi removeAnnot.
            return true;
        case AnnotWrite::Contents:
            // 🔴 0927 LƯỢT 6 / VIỆC 1 (BƯỚC 2 SPEC 0927): MỞ KHOÁ cho GUI.
            // Lý do mở được an toàn: retextNote() nhánh FreeText NGOÀI KHÔNG xoá
            // annot, chỉ ghi /Contents + /TR_AP_REBUILD, rồi lúc SAVE
            // rebuildForeignFreeTextAp() vá /AP bằng buildForeignApContent (giữ
            // nền/viền/mũi tên gốc) và lưu /TR_AP_ORIG + /TR_CONTENTS_ORIG
            // đúng MỘT lần. Không còn "chữ mới + /AP cũ = hai bản sự thật".
            // Lưu ý: khoá này CHỈ mở cho Contents — Style/Rect/Geometry vẫn
            // TỪ CHỐI mặc định ở nhánh default bên dưới.
            return true;
        case AnnotWrite::Uid:
            // Ngoại lệ CHỈ harness (mô phỏng annot của ta); GUI không bao giờ bật.
            if (m_allowTestStampUid) return true;
            return deny(QStringLiteral("guardWrite: TỪ CHỐI đóng dấu /TRUID lên annot phần mềm khác"));
        default:
            return deny(QStringLiteral("guardWrite: TỪ CHỐI ghi vào annot của phần mềm khác "
                                       "(chỉ annot có /TRUID hoặc /TRID mới được sửa)"));
    }
}

// VIỆC 1: cửa duy nhất cho mọi hàm ghi. Tự lấy khoá + mở trang + lấy annot rồi
// kiểm sở hữu. PHẢI gọi TRƯỚC lệnh ghi PDFium đầu tiên, và KHÔNG được gọi khi
// đang giữ s_pdfiumMutex (dùng guardWrite_locked cho các helper _locked).
bool AnnotationManager::guardWrite(int pageIndex, int index, AnnotWrite what, QString* whyNot) {
    if (!m_doc) {
        if (whyNot) *whyNot = QStringLiteral("guardWrite: chưa mở tài liệu");
        m_lastError = QStringLiteral("guardWrite: chưa mở tài liệu");
        return false;
    }
    TimedPdfiumLock lock(__FILE__, __LINE__);
    FPDF_PAGE page = PageCache::acquire(m_doc, pageIndex);
    if (!page) {
        if (whyNot) *whyNot = QStringLiteral("guardWrite: không mở được trang");
        m_lastError = QStringLiteral("guardWrite: không mở được trang");
        return false;
    }
    PageCache::PageBorrow _b(m_doc, pageIndex);   // R1: cặp đôi acquire()
    FPDF_ANNOTATION annot = FPDFPage_GetAnnot(page, index);
    const bool ok = guardWrite_locked(annot, what, whyNot);
    if (annot) FPDFPage_CloseAnnot(annot);
    return ok;
}

bool AnnotationManager::retextNote(int pageIndex, int index, const QString& newText) {
    if (!m_doc) return false;
    // 🔴 VIỆC 1 (0921): CỬA DUY NHẤT TRƯỚC khi lấy khoá/chạm /Contents.
    // annot ngoài ⇒ TỪ CHỐI (trừ khi harness bật m_allowForeignFreeTextEdit, lúc đó
    // nhánh FreeText ngoài bên dưới mới xử lý vá /AP).
    if (!guardWrite(pageIndex, index, AnnotWrite::Contents)) return false;
    QElapsedTimer _totalT; _totalT.start();

    QElapsedTimer _w; _w.start();
    TimedPdfiumLock lock(__FILE__, __LINE__);
    if (_w.elapsed() > 300)
        qDebug().noquote() << "[lockwait] ms=" << _w.elapsed()
                           << "at" << __FILE__ << ":" << __LINE__ << "main="
                           << ((QThread::currentThread() == QCoreApplication::instance()->thread()) ? 1 : 0);
    FPDF_PAGE page = PageCache::acquire(m_doc, pageIndex);
    if (!page) return false;
    PageCache::PageBorrow _b(m_doc, pageIndex);   // R1: cap doi acquire()

    FPDF_ANNOTATION annot = FPDFPage_GetAnnot(page, index);
    if (!annot) return false;

    int subtype = FPDFAnnot_GetSubtype(annot);
    if (subtype != FPDF_ANNOT_FREETEXT && subtype != FPDF_ANNOT_TEXT) {
        FPDFPage_CloseAnnot(annot); return false;
    }
    if (!FPDFAnnot_HasKey(annot, "TRUID") && !FPDFAnnot_HasKey(annot, "TRID")) {
        // 📐 NẤC 1 (0921): FreeText NGOÀI — KHÔNG xoá/dựng lại (mất /AP). Chỉ sửa
        // /Contents + đánh dấu /TR_AP_REBUILD; QPDF vá /AP lúc save (giữ nền/viền).
        // 🔴 0927 LƯỢT 6 / VIỆC 1 (BƯỚC 2 SPEC 0927): MỞ KHOÁ CHÍNH THỨC — GUI đã cho
        // sửa FreeText / FreeTextCallout của PHẦN MỀM KHÁC qua đường này.
        // CHỈ FreeText mới đi qua đây; Text (FPDF_ANNOT_TEXT) vẫn TỪ CHỐI
        // (không đồng cơ chữ đẩy vào /AP như FreeText).
        if (subtype != FPDF_ANNOT_FREETEXT) { FPDFPage_CloseAnnot(annot); return false; }
        // 📝 (0921) PHẠM VI restore: /AP gốc được lưu lúc vá; /Contents gốc phải lưu
        // NGAY ĐÂY (trước khi đè) vì lúc vá /Contents đã là bản mới. Nhờ vậy
        // restoreForeignAp mới trả được cả /Contents + /RC, không còn "hai bản sự thật
        // ngược chiều". Ghi đúng MỘT lần như /TR_AP_ORIG.
        if (!FPDFAnnot_HasKey(annot, "TR_CONTENTS_ORIG")) {
            const QString oldC = readAnnotString(annot, "Contents");
            FPDFAnnot_SetStringValue(annot, "TR_CONTENTS_ORIG",
                reinterpret_cast<FPDF_WIDESTRING>(oldC.utf16()));
        }
        const QString txt = newText.normalized(QString::NormalizationForm_C);
        // 🔴 0927 LƯỢT 6: ghi xuống dòng bằng '\r' (quy ước Acrobat cho FreeText) — cùng
        // hàm trContentsToCr như đã dùng cho FreeText CỦA TA. Chuỗi ĐỂ ĐÂY để TRƯỚC
        // PDFium (buildForeignApContent) quy chuẩn hoá → đúng số dòng màn hình
        // với tệp — không đánh dấu '\n' lạt vào giữa (sẽ ra glyph notdef).
        FPDFAnnot_SetStringValue(annot, "Contents",
            reinterpret_cast<FPDF_WIDESTRING>(trContentsToCr(txt).utf16()));
        FPDFAnnot_SetStringValue(annot, "TR_AP_REBUILD",
            reinterpret_cast<FPDF_WIDESTRING>(QStringLiteral("1").utf16()));
        ensureDejaVuFontAnchor_locked(page, pageIndex);
        m_hasForeignApEdits = true;
        FPDFPage_CloseAnnot(annot);
        FPDFPage_GenerateContent(page);
        PageCache::bumpAnnotGeneration(m_doc, pageIndex);
        lock.unlock();
        bumpPageRevision(pageIndex);
        AnnotInfo info;
        info.pageIndex = pageIndex;
        info.type   = "FreeText";
        info.text   = txt;
        emit annotationAdded(pageIndex, info);
        qDebug().noquote() << "[ftap] danh dau FreeText ngoai page=" << pageIndex
                           << " text=" << txt.left(40);
        return true;
    }

    FS_RECTF r{};
    FPDFAnnot_GetRect(annot, &r);
    QString author = readAnnotString(annot, "T");
    QString tridStr = readAnnotString(annot, "TRID");
    unsigned int noteId = tridStr.toUInt();
    QString uidStr = readAnnotString(annot, "TRUID");
    double pageH = FPDF_GetPageHeight(page);
    double pageW = FPDF_GetPageWidth(page);
    int rot = FPDFPage_GetRotation(page);
    const QPointF box = pdfBoxOrigin(page);

    float parsedFontSize = 11.0f;
    QColor parsedColor = Qt::black;
    if (subtype == FPDF_ANNOT_FREETEXT) {
        QString da = readAnnotString(annot, "DA");
        QStringList parts = da.split(' ', Qt::SkipEmptyParts);
        if (parts.size() >= 6 && parts[0] == QLatin1String("/Helv") && parts[2] == QLatin1String("Tf")) {
            parsedFontSize = parts[1].toFloat();
            parsedColor = QColor::fromRgbF(parts[3].toDouble(), parts[4].toDouble(), parts[5].toDouble());
        }
    }

    FPDFPage_CloseAnnot(annot);

    QPointF d1 = pdfToDisp(r.left, r.bottom, pageW, pageH, rot, box.x(), box.y());
    QPointF d2 = pdfToDisp(r.right, r.top, pageW, pageH, rot, box.x(), box.y());
    QRectF oldDispRect = QRectF(d1, d2).normalized();

    QRectF fitRect = oldDispRect;
    if (subtype == FPDF_ANNOT_FREETEXT) {
        double wDisp = (std::max)(24.0, static_cast<double>(newText.length()) * parsedFontSize * 0.55 + 8.0);
        double hDisp = parsedFontSize * 1.5 + 4.0;
        fitRect = QRectF(oldDispRect.topLeft(), QSizeF(wDisp, hDisp));
    }

    if (noteId)
        removeNotePageObjects_locked(page, noteId);

    bool dummyNeedsGen = false;
    removeAnnot_locked(page, index, &dummyNeedsGen);

    AnnotInfo info;
    const bool created = (subtype == FPDF_ANNOT_FREETEXT)
        ? createInlineNote_locked(page, pageIndex, fitRect, newText, author, false, parsedColor, parsedFontSize, &info)
        : createPopupNote_locked(page, pageIndex, fitRect.topLeft(), newText, author, &info);
    // 🔴 P2 (0921): TẠO THẤT BẠI ⇒ annot cũ đã bị xoá; TUYỆT ĐỐI không đóng dấu /TRUID lên
    // `GetAnnot(nc-1)` vì đó là annot CUỐI TRANG — có thể là CỦA ĐỐI TÁC — sẽ bị nhận vơ.
    if (!created) {
        m_lastError = QStringLiteral("retextNote: không tạo lại được ghi chú");
        return false;
    }

    if (!uidStr.isEmpty()) {
        int nc = FPDFPage_GetAnnotCount(page);
        if (nc > 0) {
            FPDF_ANNOTATION a2 = FPDFPage_GetAnnot(page, nc - 1);
            if (a2) {
                FPDFAnnot_SetStringValue(a2, "TRUID", reinterpret_cast<FPDF_WIDESTRING>(uidStr.utf16()));
                m_lastCreatedUid = uidStr;
                FPDFPage_CloseAnnot(a2);
            }
        }
    }

    setOwnNoteObjectsActive(page, true);
    QElapsedTimer _gt; _gt.start();
    FPDFPage_GenerateContent(page);
    qDebug().noquote() << "[perf] genContent page=" << pageIndex << "ms=" << _gt.elapsed();
    invalidateNoteObjCache_locked(pageIndex);
    PageCache::bumpAnnotGeneration(m_doc, pageIndex);
    recordCreatedVisual_locked(page, pageIndex);   // LÁT C 0902: uid giu nguyen → thay trong cache

    lock.unlock();
    bumpPageRevision(pageIndex);
    emit annotationAdded(pageIndex, info);
    qDebug().noquote() << "[perf] note retextNote total ms=" << _totalT.elapsed();
    return true;
}

bool AnnotationManager::removeAnnot(int pageIndex, int index) {
    if (!m_doc) return false;
    // VIỆC 1 + VIỆC 3-A3 (0921): CỬA DUY NHẤT. Delete là NGOẠI LỆ owner chốt:
    // annot ngoài VẪN được xoá, nhưng theo luật "backup rồi xoá" — người gọi phải
    // sao lưu nguyên vẹn (mọi khoá + stream) trước, để Ctrl+Z dựng lại byte-bằng.
    if (!guardWrite(pageIndex, index, AnnotWrite::Delete)) return false;
    TimedPdfiumLock lock(__FILE__, __LINE__);
    FPDF_PAGE page = PageCache::acquire(m_doc, pageIndex);
    if (!page) return false;
    PageCache::PageBorrow _b(m_doc, pageIndex);   // R1: cap doi acquire()
    // LÁT C 0902: doc uid TRUOC khi xoá (xoá xong la het doc duoc). Annot NGOAI
    // (khong TRUID) khong co delta → rev lech → MainWindow nap lai ca trang nhu cu.
    const QString uidBefore = annotUid_locked(page, index);
    bool needsGen = false;
    bool ok = removeAnnot_locked(page, index, &needsGen);
    if (ok && !uidBefore.isEmpty()) {
        AnnotVisual av; av.uid = uidBefore;
        QMutexLocker dl(&m_deltaMutex);
        m_visualDeltas[pageIndex].append({av, true});
    }
    if (ok && needsGen) {
        setOwnNoteObjectsActive(page, true);
        QElapsedTimer _gt; _gt.start();
        FPDFPage_GenerateContent(page);
        qDebug().noquote() << "[perf] genContent page=" << pageIndex << "ms=" << _gt.elapsed();
        invalidateNoteObjCache_locked(pageIndex);
    }
    if (ok) {
        PageCache::bumpAnnotGeneration(m_doc, pageIndex);
        
        bumpPageRevision(pageIndex);
    }
    return ok;
}

bool AnnotationManager::setAnnotStyle(int pageIndex, int index, QColor color, float width, bool fill, int fillAlpha) {
    // 🔴 VIỆC 1 (0921): qua CỬA DUY NHẤT guardWrite TRƯỚC khi chạm removeAnnot/addSnapshot.
    // annot NGOÀI ⇒ TỪ CHỐI ngay (đường cũ snapshot→removeAnnot→addSnapshot XOÁ RỒI
    // DỰNG LẠI ⇒ mất /AP gốc, mất QuadPoints/đỉnh PolyLine/khoá riêng).
    if (!guardWrite(pageIndex, index, AnnotWrite::Style)) return false;
    AnnotSnapshot s = snapshotAnnot(pageIndex, index);
    if (!s.valid) return false;
    if (s.subtype == FPDF_ANNOT_FREETEXT) {
        QColor daColor; float daSize = 11.0f;
        if (!parseDA(s.da, daColor, daSize) || daSize <= 0) daSize = 11.0f;
        return rebuildTextNote(pageIndex, index, color, daSize);
    }
    // PDFium KHONG tao lai duoc Line/Polygon/PolyLine (xem FPDFAnnot_IsSupportedSubtype).
    // remove-then-add se MAT annot ⇒ chan TRUOC khi remove, ke ca annot CUA TA.
    if (!FPDFAnnot_IsSupportedSubtype(static_cast<FPDF_ANNOTATION_SUBTYPE>(s.subtype)))
        return false;
    s.hasColor = true;
    s.r = static_cast<unsigned int>(color.red());
    s.g = static_cast<unsigned int>(color.green());
    s.b = static_cast<unsigned int>(color.blue());
    s.a = static_cast<unsigned int>(color.alpha());
    s.border = width;
    if (fill) {
        s.hasFill = true;
        s.fr = s.r; s.fg = s.g; s.fb = s.b;
        s.fa = static_cast<unsigned int>(qBound(0, fillAlpha, 255));
    } else {
        s.hasFill = false;
    }
    if (!removeAnnot(pageIndex, index)) return false;
    return addSnapshot(pageIndex, s);
}

AnnotSnapshot AnnotationManager::snapshotAnnot(int pageIndex, int index) {
    AnnotSnapshot s;
    if (!m_doc) return s;
    TimedPdfiumLock lock(__FILE__, __LINE__);
    FPDF_PAGE page = PageCache::acquire(m_doc, pageIndex);
    if (!page) return s;
    PageCache::PageBorrow _b(m_doc, pageIndex);   // R1: cap doi acquire()
    return snapshotAnnot_locked(page, pageIndex, index);
}

AnnotSnapshot AnnotationManager::snapshotAnnot_locked(FPDF_PAGE page, int pageIndex, int index) {
    AnnotSnapshot s;
    if (!page) return s;
    FPDF_ANNOTATION annot = FPDFPage_GetAnnot(page, index);
    if (annot) {
        s.subtype = FPDFAnnot_GetSubtype(annot);
        FS_RECTF r{};
        if (FPDFAnnot_GetRect(annot, &r)) { s.rl = r.left; s.rt = r.top; s.rr = r.right; s.rb = r.bottom; }
        // Insert Image: Stamp giu luon anh goc de hoan tac/xoa-dung lai dung net.
        if (s.subtype == FPDF_ANNOT_STAMP)
            captureStampImage(m_doc, page, annot, s.stamp);
        s.hasColor = FPDFAnnot_HasKey(annot, "C") && FPDFAnnot_GetColor(annot, FPDFANNOT_COLORTYPE_Color, &s.r, &s.g, &s.b, &s.a) != 0;
        s.hasFill = FPDFAnnot_HasKey(annot, "IC") && FPDFAnnot_GetColor(annot, FPDFANNOT_COLORTYPE_InteriorColor, &s.fr, &s.fg, &s.fb, &s.fa) != 0 && s.fa > 0;
        if (!s.hasColor) {
            unsigned long tn = FPDFAnnot_GetStringValue(annot, "TRC", nullptr, 0);
            if (tn > 2) {
                std::vector<unsigned short> tb(tn / 2 + 1, 0);
                FPDFAnnot_GetStringValue(annot, "TRC", reinterpret_cast<FPDF_WCHAR*>(tb.data()), tn);
                QString trc = QString::fromUtf16(reinterpret_cast<const char16_t*>(tb.data()));
                QStringList parts = trc.split(',');
                if (parts.size() == 3) {
                    s.r = parts[0].toUInt();
                    s.g = parts[1].toUInt();
                    s.b = parts[2].toUInt();
                    s.a = 255;
                    s.hasColor = true;
                }
            }
        }

        float bh=0.f, bv=0.f, bw=0.f; if (FPDFAnnot_GetBorder(annot, &bh, &bv, &bw)) s.border = bw;
        unsigned long len = FPDFAnnot_GetStringValue(annot, "Contents", nullptr, 0);
        if (len > 2) {
            std::vector<unsigned short> buf(len / 2 + 1, 0);
            FPDFAnnot_GetStringValue(annot, "Contents", reinterpret_cast<FPDF_WCHAR*>(buf.data()), len);
            s.contents = QString::fromUtf16(reinterpret_cast<const char16_t*>(buf.data()));
        }
            { unsigned long ulen = FPDFAnnot_GetStringValue(annot, "TRUID", nullptr, 0);
          if (ulen > 2) {
              std::vector<unsigned short> ubuf(ulen / 2 + 1, 0);
              FPDFAnnot_GetStringValue(annot, "TRUID", reinterpret_cast<FPDF_WCHAR*>(ubuf.data()), ulen);
              s.uid = QString::fromUtf16(reinterpret_cast<const char16_t*>(ubuf.data()));
          } }
    { unsigned long dlen = FPDFAnnot_GetStringValue(annot, "DA", nullptr, 0);
          if (dlen > 2) {
              std::vector<unsigned short> dbuf(dlen / 2 + 1, 0);
              FPDFAnnot_GetStringValue(annot, "DA", reinterpret_cast<FPDF_WCHAR*>(dbuf.data()), dlen);
              s.da = QString::fromUtf16(reinterpret_cast<const char16_t*>(dbuf.data()));
          } }
        int nStrokes = FPDFAnnot_GetInkListCount(annot);
        for (int i = 0; i < nStrokes; ++i) {
            unsigned long pc = FPDFAnnot_GetInkListPath(annot, i, nullptr, 0);
            if (pc > 0) {
                std::vector<FS_POINTF> pts(pc);
                FPDFAnnot_GetInkListPath(annot, i, pts.data(), pc);
                QVector<QPointF> qp;
                for (auto& p : pts) qp.append(QPointF(p.x, p.y));
                s.ink.append(qp);
            }
        }
        s.isDraft = FPDFAnnot_HasKey(annot, "TRSD") != 0;
        s.valid = true;
        FPDFPage_CloseAnnot(annot);
    }
    return s;
}

bool AnnotationManager::addSnapshot(int pageIndex, const AnnotSnapshot& s) {
    if (!m_doc || !s.valid) return false;
    TimedPdfiumLock lock(__FILE__, __LINE__);
    FPDF_PAGE page = PageCache::acquire(m_doc, pageIndex);
    if (!page) return false;
    PageCache::PageBorrow _b(m_doc, pageIndex);   // R1: cap doi acquire()
    FPDF_ANNOTATION annot = FPDFPage_CreateAnnot(page, static_cast<FPDF_ANNOTATION_SUBTYPE>(s.subtype));
    if (annot) {
        FS_RECTF r{ s.rl, s.rt, s.rr, s.rb };
        FPDFAnnot_SetRect(annot, &r);
        if (s.hasColor) {
            FPDFAnnot_SetColor(annot, FPDFANNOT_COLORTYPE_Color, s.r, s.g, s.b, s.a);
            QString trc = QString("%1,%2,%3").arg(s.r).arg(s.g).arg(s.b);
            FPDFAnnot_SetStringValue(annot, "TRC", reinterpret_cast<FPDF_WIDESTRING>(trc.utf16()));
        }
        if (s.hasFill)
            FPDFAnnot_SetColor(annot, FPDFANNOT_COLORTYPE_InteriorColor, s.fr, s.fg, s.fb, s.fa);
        FPDFAnnot_SetBorder(annot, 0.0f, 0.0f, s.border);
        if (!s.contents.isEmpty())
            FPDFAnnot_SetStringValue(annot, "Contents", reinterpret_cast<FPDF_WIDESTRING>(s.contents.utf16()));
        if (!s.da.isEmpty())
            FPDFAnnot_SetStringValue(annot, "DA", reinterpret_cast<FPDF_WIDESTRING>(s.da.utf16()));
        if (!s.uid.isEmpty())
            FPDFAnnot_SetStringValue(annot, "TRUID", reinterpret_cast<FPDF_WIDESTRING>(s.uid.utf16()));
        if (s.isDraft)
            FPDFAnnot_SetStringValue(annot, "TRSD", reinterpret_cast<FPDF_WIDESTRING>(QStringLiteral("1").utf16()));
        for (const auto& stroke : s.ink) {
            std::vector<FS_POINTF> pts;
            for (const auto& p : stroke) pts.push_back(FS_POINTF{ static_cast<float>(p.x()), static_cast<float>(p.y()) });
            if (pts.size() >= 2) FPDFAnnot_AddInkStroke(annot, pts.data(), pts.size());
        }
        // Insert Image: dung lai Stamp can gan lai anh goc (giu kenh alpha).
        if (s.subtype == FPDF_ANNOT_STAMP && !s.stamp.isNull()) {
            const double w = double(s.rr - s.rl);
            const double h = double(s.rt - s.rb);
            FPDF_PAGEOBJECT obj = makeStampImageObject(
                m_doc, page, s.stamp,
                w / double(qMax(1, s.stamp.width())),
                h / double(qMax(1, s.stamp.height())),
                s.rl, s.rb);
            if (obj) FPDFAnnot_AppendObject(annot, obj);
            // Sinh /AP ngay (nhu insertStampImage) de file luu ra hien dung.
            FPDFPage_CloseAnnot(annot);
            FPDFPage_GenerateContent(page);
        } else {
            FPDFPage_CloseAnnot(annot);
        }
        // GenerateContent deferred to save time — annotations in /Annots are
        // preserved by FPDF_SaveAsCopy without explicit GenerateContent.
    }
    bool ok = (annot != nullptr);
    if (ok) {
        PageCache::bumpAnnotGeneration(m_doc, pageIndex);
        recordCreatedVisual_locked(page, pageIndex);   // LÁT C 0902: undo/redo hien lai ngay
    }
    if (ok) bumpPageRevision(pageIndex);
    return ok;
}

bool AnnotationManager::getAnnotEditState(int pageIndex, int index,
                                          QString& outType, QColor& outColor,
                                          float& outWidth, float& outFontSize,
                                          bool* outHasFill, int* outFillAlpha) {
    if (!m_doc) return false;
    TimedPdfiumLock lock(__FILE__, __LINE__);
    FPDF_PAGE page = PageCache::acquire(m_doc, pageIndex);
    if (!page) return false;
    PageCache::PageBorrow _b(m_doc, pageIndex);   // R1: cap doi acquire()
    FPDF_ANNOTATION annot = FPDFPage_GetAnnot(page, index);
    if (!annot) return false;

    auto sub = FPDFAnnot_GetSubtype(annot);
    outType = subtypeName(sub);
    outWidth = 2.0f;
    outFontSize = 0.0f;

    // Colour: prefer C key, then DA for FreeText, then fixed for Note, then TRC, then black
    unsigned int cr = 0, cg = 0, cb = 0, ca = 255;
    bool hasCColor = FPDFAnnot_HasKey(annot, "C") &&
                     FPDFAnnot_GetColor(annot, FPDFANNOT_COLORTYPE_Color, &cr, &cg, &cb, &ca) != 0;
    if (hasCColor) {
        outColor = QColor(cr, cg, cb, ca);
    } else if (sub == FPDF_ANNOT_FREETEXT) {
        QColor daColor; float daSize;
        if (parseDA(readAnnotString(annot, "DA"), daColor, daSize)) {
            outColor = daColor;
            if (daSize > 0) outFontSize = daSize;
        } else {
            outColor = Qt::black;
        }
        if (outFontSize <= 0) outFontSize = 11.0f;
    } else if (sub == FPDF_ANNOT_TEXT) {
        outColor = QColor(255, 220, 0);
    } else {
        unsigned long tn = FPDFAnnot_GetStringValue(annot, "TRC", nullptr, 0);
        if (tn > 2) {
            std::vector<unsigned short> tb(tn / 2 + 1, 0);
            FPDFAnnot_GetStringValue(annot, "TRC", reinterpret_cast<FPDF_WCHAR*>(tb.data()), tn);
            QString trc = QString::fromUtf16(reinterpret_cast<const char16_t*>(tb.data()));
            QStringList parts = trc.split(',');
            if (parts.size() == 3)
                outColor = QColor(parts[0].toUInt(), parts[1].toUInt(), parts[2].toUInt());
            else
                outColor = Qt::black;
        } else {
            outColor = Qt::black;
        }
    }

    float bh = 0.f, bv = 0.f, bw = 0.f;
    if (FPDFAnnot_GetBorder(annot, &bh, &bv, &bw))
        outWidth = bw;

    if (sub == FPDF_ANNOT_FREETEXT && outFontSize <= 0) {
        QColor daColor; float daSize;
        if (parseDA(readAnnotString(annot, "DA"), daColor, daSize) && daSize > 0)
            outFontSize = daSize;
        else
            outFontSize = 11.0f;
    }

    {
        unsigned int fr = 255, fg = 255, fb = 255, fa = 255;
        bool hasIC = FPDFAnnot_HasKey(annot, "IC") &&
                     FPDFAnnot_GetColor(annot, FPDFANNOT_COLORTYPE_InteriorColor, &fr, &fg, &fb, &fa) != 0;
        bool hasFill = (hasIC && fa > 0);
        if (outHasFill) *outHasFill = hasFill;
        if (outFillAlpha) *outFillAlpha = static_cast<int>(fa);
    }

    FPDFPage_CloseAnnot(annot);
    return true;
}

// A4 (0921): updateNote da bi XOA HAN.
// - Grep toan bo src/ (tru .bak): KHONG co noi goi nao. No ghi /Contents vao BAT KY
//   annot nao (khong kiem so huu) roi tu saveDocument() ⇒ vua la ma chet, vua la cua
//   ghi khong qua guardWrite. Owner cho phep "XOA han, hoac cho qua guardWrite" —
//   chon XOA HAN de khong con duong ghi ngoai luong nao ton tai.

int AnnotationManager::findAnnotIndexByUid(int pageIndex, const QString& uid) {
    if (!m_doc || uid.isEmpty()) return -1;
    TimedPdfiumLock lock(__FILE__, __LINE__);
    FPDF_PAGE page = PageCache::acquire(m_doc, pageIndex);
    if (!page) return -1;
    PageCache::PageBorrow _b(m_doc, pageIndex);   // R1: cap doi acquire()
    int count = FPDFPage_GetAnnotCount(page);
    for (int i = 0; i < count; ++i) {
        FPDF_ANNOTATION annot = FPDFPage_GetAnnot(page, i);
        if (!annot) continue;
        unsigned long len = FPDFAnnot_GetStringValue(annot, "TRUID", nullptr, 0);
        if (len > 2) {
            std::vector<unsigned short> buf(len / 2 + 1, 0);
            FPDFAnnot_GetStringValue(annot, "TRUID", reinterpret_cast<FPDF_WCHAR*>(buf.data()), len);
            QString existing = QString::fromUtf16(reinterpret_cast<const char16_t*>(buf.data()));
            if (existing == uid) {
                FPDFPage_CloseAnnot(annot);
                return i;
            }
        }
        FPDFPage_CloseAnnot(annot);
    }
    return -1;
}

int AnnotationManager::findAnnotIndexByAnyUid(int pageIndex, const QString& uid) {
    if (!m_doc || uid.isEmpty()) return -1;
    TimedPdfiumLock lock(__FILE__, __LINE__);
    FPDF_PAGE page = PageCache::acquire(m_doc, pageIndex);
    if (!page) return -1;
    PageCache::PageBorrow _b(m_doc, pageIndex);   // R1: cap doi acquire()
    int count = FPDFPage_GetAnnotCount(page);
    for (int i = 0; i < count; ++i) {
        FPDF_ANNOTATION annot = FPDFPage_GetAnnot(page, i);
        if (!annot) continue;
        for (const char* key : {"TRUID", "TRXUID"}) {
            unsigned long len = FPDFAnnot_GetStringValue(annot, key, nullptr, 0);
            if (len > 2) {
                std::vector<unsigned short> buf(len / 2 + 1, 0);
                FPDFAnnot_GetStringValue(annot, key, reinterpret_cast<FPDF_WCHAR*>(buf.data()), len);
                QString existing = QString::fromUtf16(reinterpret_cast<const char16_t*>(buf.data()));
                if (existing == uid) {
                    FPDFPage_CloseAnnot(annot);
                    return i;
                }
            }
        }
        FPDFPage_CloseAnnot(annot);
    }
    return -1;
}

QString AnnotationManager::ensureExternalUid(int pageIndex, int index) {
    if (!m_doc) return {};
    // 🔴 VIỆC 3-A5 (0921) — QUYẾT ĐỊNH: /TRXUID KHÔNG được ghi vào annot ngoài.
    // Lập luận: (1) /TRXUID là khoá theo dõi CỦA TA, ghi vào annot đối tác là tự ý
    // sửa vật thể của họ — chính cái luật chung này cấm. (2) Nó chẳng giúp gì cho
    // undo move, vì moveAnnot nay đã TỪ CHỐI annot ngoài (A2) ⇒ không có thao tác
    // nào cần uid để hoàn tác. (3) Chỗ cũ gọi ensureExternalUid TRƯỚC moveAnnot nên
    // dù moveAnnot từ chối, /TRXUID đã kịp ghi ⇒ lời hứa "không chạm gì" sai ở tầng
    // file. Từ đây trả rỗng cho annot ngoài, chỉ cấp uid cho annot CỦA TA.
    if (!guardWrite(pageIndex, index, AnnotWrite::Uid)) return {};
    TimedPdfiumLock lock(__FILE__, __LINE__);
    FPDF_PAGE page = PageCache::acquire(m_doc, pageIndex);
    if (!page) return {};
    PageCache::PageBorrow _b(m_doc, pageIndex);   // R1: cap doi acquire()
    FPDF_ANNOTATION annot = FPDFPage_GetAnnot(page, index);
    if (!annot) return {};
    QString existing = readAnnotString(annot, "TRXUID");
    if (!existing.isEmpty()) {
        FPDFPage_CloseAnnot(annot);
        return existing;
    }
    QString newUid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    FPDFAnnot_SetStringValue(annot, "TRXUID", reinterpret_cast<FPDF_WIDESTRING>(newUid.utf16()));
    FPDFPage_CloseAnnot(annot);
    PageCache::bumpAnnotGeneration(m_doc, pageIndex);

    bumpPageRevision(pageIndex);
    return newUid;
}

bool AnnotationManager::setAnnotUid(int pageIndex, int index, const QString& uid) {
    if (!m_doc || uid.isEmpty()) return false;
    // VIỆC 1 (0921): CỬA DUY NHẤT TRƯỚC khi ghi /TRUID. Chặn annot ngoài bị "đóng dấu"
    // thành của ta (đường undo DeleteNote/AddSnapshot cũ gán TRUID lên annot cuối trang).
    if (!guardWrite(pageIndex, index, AnnotWrite::Uid)) return false;
    TimedPdfiumLock lock(__FILE__, __LINE__);
    FPDF_PAGE page = PageCache::acquire(m_doc, pageIndex);
    if (!page) return false;
    PageCache::PageBorrow _b(m_doc, pageIndex);   // R1: cap doi acquire()
    FPDF_ANNOTATION annot = FPDFPage_GetAnnot(page, index);
    if (!annot) return false;
    FPDFAnnot_SetStringValue(annot, "TRUID", reinterpret_cast<FPDF_WIDESTRING>(uid.utf16()));
    FPDFPage_CloseAnnot(annot);
    PageCache::bumpAnnotGeneration(m_doc, pageIndex);

    bumpPageRevision(pageIndex);
    return true;
}

bool AnnotationManager::setAnnotContents(int pageIndex, int index, const QString& text) {
    if (!m_doc) return false;
    // 🔴 VIỆC 1 (0921): qua CỬA DUY NHẤT guardWrite TRƯỚC khi chạm /Contents.
    // annot ngoài ⇒ TỪ CHỐI (chữ mới + /AP cũ = hai bản sự thật); annot của ta ⇒ qua.
    if (!guardWrite(pageIndex, index, AnnotWrite::Contents)) return false;
    QElapsedTimer _w; _w.start();
    TimedPdfiumLock lock(__FILE__, __LINE__);
    if (_w.elapsed() > 300)
        qDebug().noquote() << "[lockwait] ms=" << _w.elapsed()
                           << "at" << __FILE__ << ":" << __LINE__ << "main="
                           << ((QThread::currentThread() == QCoreApplication::instance()->thread()) ? 1 : 0);
    FPDF_PAGE page = PageCache::acquire(m_doc, pageIndex);
    if (!page) return false;
    PageCache::PageBorrow _b(m_doc, pageIndex);   // R1: cap doi acquire()
    FPDF_ANNOTATION annot = FPDFPage_GetAnnot(page, index);
    if (!annot) return false;
    // 🔴 0927 LƯỢT 6 / VIỆC 1: annot NGOÀI mà bị gọi vào đây (sidebar in-place,
    // undo) sẽ tạo "hai bản sự thật": /Contents mới + /AP cũ. Đánh dấu
    // /TR_AP_REBUILD + lưu /TR_CONTENTS_ORIG đúng MỘT lần ⇒ lúc save
    // rebuildForeignFreeTextAp() vá /AP khớp với chữ. ĐÚNG CÙNG CÁCH
    // retextNote() làm cho FreeText ngoài.
    const bool foreign = !FPDFAnnot_HasKey(annot, "TRUID") && !FPDFAnnot_HasKey(annot, "TRID");
    if (foreign && FPDFAnnot_GetSubtype(annot) == FPDF_ANNOT_FREETEXT) {
        if (!FPDFAnnot_HasKey(annot, "TR_CONTENTS_ORIG")) {
            const QString oldC = readAnnotString(annot, "Contents");
            FPDFAnnot_SetStringValue(annot, "TR_CONTENTS_ORIG",
                reinterpret_cast<FPDF_WIDESTRING>(oldC.utf16()));
        }
        FPDFAnnot_SetStringValue(annot, "TR_AP_REBUILD",
            reinterpret_cast<FPDF_WIDESTRING>(QStringLiteral("1").utf16()));
        ensureDejaVuFontAnchor_locked(page, pageIndex);   // 0927 L6: xem ghi chu ham
        m_hasForeignApEdits = true;
    }
    FPDFAnnot_SetStringValue(annot, "Contents", reinterpret_cast<FPDF_WIDESTRING>(text.utf16()));
    FPDFPage_CloseAnnot(annot);
    PageCache::bumpAnnotGeneration(m_doc, pageIndex);

    lock.unlock();
    bumpPageRevision(pageIndex);
    return true;
}

QString AnnotationManager::generateUid() {
    return QString::number(m_nextNoteId++) + "_" + QString::number(QDateTime::currentMSecsSinceEpoch());
}

void AnnotationManager::bumpPageRevision(int page) {
    m_pageRev[page] = m_pageRev.value(page, 0) + 1;
    emit pageContentChanged(page);
}

bool AnnotationManager::createSignatureDraft(int pageIndex, QRectF rectPt, const QString& text) {
    if (!m_doc) return false;

    QElapsedTimer _w; _w.start();
    TimedPdfiumLock lock(__FILE__, __LINE__);
    if (_w.elapsed() > 300)
        qDebug().noquote() << "[lockwait] ms=" << _w.elapsed()
                           << "at" << __FILE__ << ":" << __LINE__ << "main="
                           << ((QThread::currentThread() == QCoreApplication::instance()->thread()) ? 1 : 0);
    FPDF_PAGE page = PageCache::acquire(m_doc, pageIndex);
    if (!page) { m_lastError = "Cannot load page"; return false; }
    PageCache::PageBorrow _b(m_doc, pageIndex);   // R1: cap doi acquire()

    FPDF_ANNOTATION annot = FPDFPage_CreateAnnot(page, FPDF_ANNOT_FREETEXT);
    if (!annot) { m_lastError = "Cannot create annotation"; return false; }

    FS_RECTF rect{
        static_cast<float>(rectPt.left()),
        static_cast<float>(rectPt.top()),
        static_cast<float>(rectPt.right()),
        static_cast<float>(rectPt.bottom())
    };
    FPDFAnnot_SetRect(annot, &rect);

    FPDFAnnot_SetStringValue(annot, "DA",
        reinterpret_cast<FPDF_WIDESTRING>(QStringLiteral("/Helv 9 Tf 0 0 1 rg").utf16()));

    FPDFAnnot_SetStringValue(annot, "Contents",
        reinterpret_cast<FPDF_WIDESTRING>(text.utf16()));

    FPDFAnnot_SetColor(annot, FPDFANNOT_COLORTYPE_Color, 220, 235, 255, 255);
    FPDFAnnot_SetBorder(annot, 0.0f, 0.0f, 1.0f);

    FPDFAnnot_SetStringValue(annot, "TRSD",
        reinterpret_cast<FPDF_WIDESTRING>(QStringLiteral("1").utf16()));
    m_lastCreatedUid = generateUid();
    FPDFAnnot_SetStringValue(annot, "TRUID",
        reinterpret_cast<FPDF_WIDESTRING>(m_lastCreatedUid.utf16()));

    FPDFPage_CloseAnnot(annot);
    setOwnNoteObjectsActive(page, true);
    QElapsedTimer _gt; _gt.start();
    FPDFPage_GenerateContent(page);
    qDebug().noquote() << "[perf] genContent page=" << pageIndex << "ms=" << _gt.elapsed();
    PageCache::bumpAnnotGeneration(m_doc, pageIndex);
    recordCreatedVisual_locked(page, pageIndex);   // LÁT C 0902

    lock.unlock();

    bumpPageRevision(pageIndex);
    return saveDocument();
}

QRectF AnnotationManager::findSignatureDraftRect(int pageIndex, int* outIndex) {
    auto list = loadPage(pageIndex);
    for (int i = 0; i < list.size(); ++i) {
        if (list[i].isDraft) {
            *outIndex = i;
            return list[i].rect;
        }
    }
    *outIndex = -1;
    return {};
}

// ── _locked helpers (caller holds s_pdfiumMutex and has `page` open) ────────

int AnnotationManager::removeNotePageObjects_locked(FPDF_PAGE page, unsigned int noteId) {
    int count = FPDFPage_CountObjects(page);
    int removed = 0;
    std::vector<FPDF_PAGEOBJECT> toRemove;

    for (int i = 0; i < count; ++i) {
        FPDF_PAGEOBJECT obj = FPDFPage_GetObject(page, i);
        if (!obj) continue;
        int nmarks = FPDFPageObj_CountMarks(obj);
        if (nmarks <= 0) continue;
        for (int m = 0; m < nmarks; ++m) {
            FPDF_PAGEOBJECTMARK mark = FPDFPageObj_GetMark(obj, static_cast<unsigned long>(m));
            if (!mark) continue;

            unsigned long nameLen = 0;
            if (!FPDFPageObjMark_GetName(mark, nullptr, 0, &nameLen)) continue;
            std::vector<unsigned short> nameBuf(nameLen / 2 + 1, 0);
            if (!FPDFPageObjMark_GetName(mark, reinterpret_cast<FPDF_WCHAR*>(nameBuf.data()),
                                          nameLen, &nameLen)) continue;
            QString markName = QString::fromUtf16(reinterpret_cast<const char16_t*>(nameBuf.data()));
            if (markName != QLatin1String("TRNote")) continue;

            int val = 0;
            if (!FPDFPageObjMark_GetParamIntValue(mark, "id", &val)) continue;
            if (static_cast<unsigned int>(val) == noteId) {
                toRemove.push_back(obj);
                break;
            }
        }
    }

    for (auto obj : toRemove) {
        if (FPDFPage_RemoveObject(page, obj)) {
            FPDFPageObj_Destroy(obj);
            ++removed;
        }
    }
    return removed;
}

bool AnnotationManager::objectHasNoteId(FPDF_PAGEOBJECT obj, unsigned int noteId) {
    if (!obj) return false;
    int nmarks = FPDFPageObj_CountMarks(obj);
    for (int m = 0; m < nmarks; ++m) {
        FPDF_PAGEOBJECTMARK mark = FPDFPageObj_GetMark(obj, static_cast<unsigned long>(m));
        if (!mark) continue;
        unsigned long nameLen = 0;
        if (!FPDFPageObjMark_GetName(mark, nullptr, 0, &nameLen)) continue;
        std::vector<unsigned short> nameBuf(nameLen / 2 + 1, 0);
        if (!FPDFPageObjMark_GetName(mark, reinterpret_cast<FPDF_WCHAR*>(nameBuf.data()),
                                      nameLen, &nameLen)) continue;
        QString markName = QString::fromUtf16(reinterpret_cast<const char16_t*>(nameBuf.data()));
        if (markName != QLatin1String("TRNote")) continue;
        int val = 0;
        if (!FPDFPageObjMark_GetParamIntValue(mark, "id", &val)) continue;
        if (static_cast<unsigned int>(val) == noteId) return true;
    }
    return false;
}

int AnnotationManager::translateNotePageObjects_locked(FPDF_PAGE page, int pageIndex,
                                                        unsigned int noteId,
                                                        double dx, double dy) {
    QElapsedTimer _t; _t.start();
    int count = FPDFPage_CountObjects(page);
    int translated = 0;
    int objsVisited = 0;

    // Cache key (pageIndex, noteId). pageIndex do caller truyen truc tiep
    // (PageCache la chu so huu FPDF_PAGE — khong con quet pin LRU/scratch).
    auto key = QPair<int,quint32>(pageIndex, noteId);

    bool usedCache = false;
    if (pageIndex >= 0) {
        auto it = m_noteObjIdxCache.constFind(key);
        if (it != m_noteObjIdxCache.constEnd() && !it->isEmpty()) {
            bool cacheOk = true;
            for (int oi : *it) {
                FPDF_PAGEOBJECT o = FPDFPage_GetObject(page, oi);
                if (!o || !objectHasNoteId(o, noteId)) { cacheOk = false; break; }
            }
            if (cacheOk) {
                const QVector<int>& idxs = it.value();
                for (int i : idxs) {
                    FPDF_PAGEOBJECT obj = FPDFPage_GetObject(page, i);
                    if (obj) {
                        FPDFPageObj_Transform(obj, 1, 0, 0, 1, dx, dy);
                        ++translated;
                    }
                }
                usedCache = true;
                objsVisited = idxs.size();
            } else {
                m_noteObjIdxCache.erase(m_noteObjIdxCache.find(key));
            }
        }
    }

    if (!usedCache) {
        QVector<int> hitIdxs;
        for (int i = 0; i < count; ++i) {
            FPDF_PAGEOBJECT obj = FPDFPage_GetObject(page, i);
            if (objectHasNoteId(obj, noteId)) {
                FPDFPageObj_Transform(obj, 1, 0, 0, 1, dx, dy);
                ++translated;
                hitIdxs.append(i);
            }
        }
        objsVisited = count;
        if (pageIndex >= 0)
            m_noteObjIdxCache.insert(QPair<int,quint32>(pageIndex, noteId), hitIdxs);
    }

    qDebug().noquote() << "[perf] translateNote page=" << pageIndex << "objs=" << objsVisited
                       << "ms=" << _t.elapsed();
    return translated;
}

bool AnnotationManager::removeAnnot_locked(FPDF_PAGE page, int index, bool* outNeedsGen) {
    bool subNeedsGen = false;
    FPDF_ANNOTATION annot = FPDFPage_GetAnnot(page, index);
    // VIỆC 1 (0921): CỬA DUY NHẤT (bản _locked — caller đã giữ khoá), chốt thứ hai ngay
    // sát lệnh xoá để phep thu --annotwrite-audit bắt được. Caller công khai
    // (removeAnnot / rebuildTextNote / retextNote) đều đã qua guardWrite trước đó.
    if (!guardWrite_locked(annot, AnnotWrite::Delete)) {
        if (annot) FPDFPage_CloseAnnot(annot);
        return false;
    }
    if (annot) {
        auto sub = FPDFAnnot_GetSubtype(annot);
        subNeedsGen = (sub == FPDF_ANNOT_TEXT || sub == FPDF_ANNOT_FREETEXT);
        if (subNeedsGen) {
            QString tridStr = readAnnotString(annot, "TRID");
            unsigned int noteId = tridStr.toUInt();
            if (noteId) {
                int count = FPDFPage_CountObjects(page);
                QVector<FPDF_PAGEOBJECT> toRemove;
                int foundCount = 0;
                for (int i = 0; i < count && foundCount < 6; ++i) {
                    FPDF_PAGEOBJECT obj = FPDFPage_GetObject(page, i);
                    if (!obj) continue;
                    int nMarks = FPDFPageObj_CountMarks(obj);
                    for (int m = 0; m < nMarks; ++m) {
                        FPDF_PAGEOBJECTMARK mark = FPDFPageObj_GetMark(obj, static_cast<unsigned long>(m));
                        if (!mark) continue;
                        unsigned long nameLen = 0;
                        if (!FPDFPageObjMark_GetName(mark, nullptr, 0, &nameLen)) continue;
                        std::vector<unsigned short> nameBuf(nameLen / 2 + 1, 0);
                        if (!FPDFPageObjMark_GetName(mark, reinterpret_cast<FPDF_WCHAR*>(nameBuf.data()), nameLen, &nameLen)) continue;
                        QString markName = QString::fromUtf16(reinterpret_cast<const char16_t*>(nameBuf.data()));
                        if (markName != QLatin1String("TRNote")) continue;
                        int val = 0;
                        if (!FPDFPageObjMark_GetParamIntValue(mark, "id", &val)) continue;
                        if (static_cast<unsigned int>(val) == noteId) {
                            toRemove.append(obj);
                            ++foundCount;
                            break;
                        }
                    }
                }
                for (auto obj : toRemove) {
                    FPDFPage_RemoveObject(page, obj);
                    FPDFPageObj_Destroy(obj);
                }
            }
        }
        FPDFPage_CloseAnnot(annot);
    }
    bool ok = FPDFPage_RemoveAnnot(page, index);
    if (outNeedsGen) *outNeedsGen = subNeedsGen;
    return ok;
}

bool AnnotationManager::createInlineNote_locked(FPDF_PAGE page, int pageIndex, QRectF rectPdf,
                                                 const QString& textIn, const QString& author,
                                                 bool withBackground, QColor textColor, float fontSize,
                                                 AnnotInfo* outInfo) {
    const QString text = textIn.normalized(QString::NormalizationForm_C);
    if (!m_doc) return false;

    // 🔴 FT-SIZE 0902: o PHAI DU CHU. Nong theo advance/chieu cao THAT cua chu o
    // fontSize (QFontMetricsF, cung font voi /AP va overlay) — thay cho chieu cao
    // co dinh 18pt cua goi. Day la goc cua "chu bi cat cuth": o 18pt khong chua
    // du chu 24pt => overlay clip chi con man net.
    rectPdf = trFreeTextFitRect(rectPdf, text, fontSize);

    double pageH = FPDF_GetPageHeight(page);
    double pageW = FPDF_GetPageWidth(page);
    int rot = FPDFPage_GetRotation(page);
    const QPointF box = pdfBoxOrigin(page);

    qDebug() << "[inote] enter page=" << pageIndex << "rot=" << rot << "disp=" << pageW << "x" << pageH << "text=" << text;

    QPointF tl = dispToPdf(rectPdf.left(),  rectPdf.top(),    pageW, pageH, rot, box.x(), box.y());
    QPointF tr = dispToPdf(rectPdf.right(), rectPdf.top(),    pageW, pageH, rot, box.x(), box.y());
    QPointF bl = dispToPdf(rectPdf.left(),  rectPdf.bottom(), pageW, pageH, rot, box.x(), box.y());
    QPointF br = dispToPdf(rectPdf.right(), rectPdf.bottom(), pageW, pageH, rot, box.x(), box.y());
    float xu_min = static_cast<float>((std::min)({tl.x(), tr.x(), bl.x(), br.x()}));
    float xu_max = static_cast<float>((std::max)({tl.x(), tr.x(), bl.x(), br.x()}));
    float yu_min = static_cast<float>((std::min)({tl.y(), tr.y(), bl.y(), br.y()}));
    float yu_max = static_cast<float>((std::max)({tl.y(), tr.y(), bl.y(), br.y()}));
    float w = xu_max - xu_min;
    float h = yu_max - yu_min;

    qDebug() << "[inote] rectU x[" << xu_min << "," << xu_max << "] y[" << yu_min << "," << yu_max << "] w=" << w << "h=" << h;

    FPDF_FONT font = unicodeFont();
    qDebug() << "[inote] font loaded=" << (font != nullptr);
    unsigned int noteId = m_nextNoteId++;

    // ── FreeText THAT (2026-09-02): khong con chen vat the TRNote, khong con co
    // HIDDEN. Annot tu ve qua /AP (PDFium) — giong Line/Rect/Circle/Ink/Stamp.
    // ── Moc neo dang ky phông: PDFium chi dua phông vao /Resources/Font cua TRANG
    // khi GenerateContent thay page object dung den no. Mot text object chua duy
    // nui dau cach, KHONG gan mark TRNote, o goc (0,0) → khong ve ra pixel thay
    // duoc, nhung dang ky du /FXF1 (ten PDFium dat cho phông doc-level nay) vao
    // tai nguyen trang — noi /AP ke thua (da chung minh: AP khong can /Resources
    // rieng). Khong co moc neo → /FXF1 trong /AP tro vao khong khi.
    if (font) {
        FPDF_PAGEOBJECT anchor = FPDFPageObj_CreateTextObj(m_doc, font, fontSize);
        if (anchor) {
            static const FPDF_WCHAR s_space[2] = { ' ', 0 };
            FPDFText_SetText(anchor, const_cast<FPDF_WCHAR*>(s_space));
            FPDFPageObj_Transform(anchor, 1.0, 0.0, 0.0, 1.0, 0.0, 0.0);
            FPDFPage_InsertObject(page, anchor);
        }
    }

    FPDF_ANNOTATION annot = FPDFPage_CreateAnnot(page, FPDF_ANNOT_FREETEXT);
    if (!annot) { m_lastError = "Cannot create annotation"; return false; }

    // 🔴 0927 LƯỢT 3 — LỖI MỞ LẠI TỆP ĐÃ LƯU (đo trên ft3_out.pdf).
    // FPDFPage_CreateAnnot(FPDF_ANNOT_FREETEXT) để lại /F = 2 tức bit HIDDEN.
    // Ý đồ thiết kế 02/09 (FTREAL) là FreeText CỦA TA là annot THẬT: KHÔNG
    // HIDDEN ⇒ isOverlayDrawnAnnot() trả true ⇒ overlay của app vẽ, còn
    // OwnAnnotHideGuard giấu nó khỏi ảnh nền để khỏi vẽ hai lần. Nhưng vì
    // CreateAnnot để sẵn HIDDEN, điều kiện `!HIDDEN` ở paintByOverlay
    // (AnnotationManager.cpp ~1167) fail ⇒ KHÔNG ảnh nền (PDFium bỏ annot
    // HIDDEN) cũng KHÔNG overlay ⇒ mở lại tệp đã lưu là MẤT TRẮN chữ, dù
    // /AP stream đúng 5 dòng. Sửa ở gốc: xoá bit HIDDEN ngay khi tạo.
    // (Note/Text icon thì CỐ Ý GIỮ HIDDEN — xem createIconNote, dòng ~2926.)
    FPDFAnnot_SetFlags(annot, FPDFAnnot_GetFlags(annot) & ~FPDF_ANNOT_FLAG_HIDDEN);

    // 🔴 0927 LƯỢT 2 — BUG #3 (đo 27/09). FS_RECTF là {left, top, right, bottom}
    // (đã đọc fpdfview.h:156). Chỗ này điền {xu_min, yu_min, xu_max, yu_max} ⇒
    // top/bottom BỊ ĐẢO ⇒ PDF ghi ra /Rect[60 242 260 168.15625] (ll > ury).
    // Hậu quả: /BBox của /AP sao chép /Rect nên cũng đảo ⇒ KHÔNG renderer nào
    // vẽ được (đo: PyMuPDF render vùng annot = 0 pixel đen, get_text = 0 từ).
    // Mọi chỗ SetRect khác trong file này đều dùng (xu_min, yu_max, xu_max,
    // yu_min) — chỗ này là chỗ DUY NHẤT điền sai.
    FS_RECTF rect{ xu_min, yu_max, xu_max, yu_min };
    FPDFAnnot_SetRect(annot, &rect);
    qDebug() << "[inote] real FreeText rect=" << rect.left << rect.bottom << rect.right << rect.top;

    QString daQ = QString("/Helv %1 Tf %2 %3 %4 rg")
        .arg(fontSize, 0, 'f', 1)
        .arg(textColor.redF(),   0, 'f', 3)
        .arg(textColor.greenF(), 0, 'f', 3)
        .arg(textColor.blueF(),  0, 'f', 3);
    FPDFAnnot_SetStringValue(annot, "DA",
        reinterpret_cast<FPDF_WIDESTRING>(daQ.utf16()));
    // 🔴 0927 BƯỜ 1 để 5: /Contents ghi xuống dòng bằng \r (quy uếc Acrobat
    // cho FreeText). Đọc ra đânh để đước chuẩn hoá \n (readAnnotString).
    FPDFAnnot_SetStringValue(annot, "Contents",
        reinterpret_cast<FPDF_WIDESTRING>(trContentsToCr(text).utf16()));
    if (!author.isEmpty())
        FPDFAnnot_SetStringValue(annot, "T",
            reinterpret_cast<FPDF_WIDESTRING>(author.utf16()));
    if (withBackground)
        FPDFAnnot_SetColor(annot, FPDFANNOT_COLORTYPE_Color, 255, 255, 150, 200);
    else
        FPDFAnnot_SetBorder(annot, 0.0f, 0.0f, 0.0f);

    QString trid = QString::number(noteId);
    FPDFAnnot_SetStringValue(annot, "TRID",
        reinterpret_cast<FPDF_WIDESTRING>(trid.utf16()));
    m_lastCreatedUid = generateUid();
    FPDFAnnot_SetStringValue(annot, "TRUID",
        reinterpret_cast<FPDF_WIDESTRING>(m_lastCreatedUid.utf16()));

    // ── /AP tu viet, phong /FXF1 Identity-H (tieng Việt chuan, da chung minh) ──
    float usable_w = w, usable_h = h;
    if (rot == 1 || rot == 3) { usable_w = h; usable_h = w; }
    // 🔴 FT-SIZE 0902: do bang advance THAT (cung font voi o va overlay), khong
    // dung uoc so length*0.6 nua. Uoc so ay LUON lon hon advance that (~0.55) nen
    // no thu fs xuong 8.8 trong khi /DA ghi 24 => MOI DANG MAU THUAN. O da duoc
    // nong vua chu nen day khong con trigger; /DA == /AP == fontSize.
    // 🔴 0927 BƯỚC 1: KHÔNG còn uoc 1-dong. Bề rộng dùng để ngắt mềm lấy
    // theo BỀ RỘNG Ô (w − 2·pad) — CÙNG hàm trWrapFreeText với trFreeTextFitRect
    // và overlay ⇒ số dòng ở /AP = số dòng trên màn hình.
    const double apPad = kTrFreeTextWrapPad;   // dung chung voi trFreeTextFitRect
    const double wrapW = qMax(0.0, static_cast<double>(usable_w) - 2.0 * apPad);
    QFont apFont = trDejaVuFontAtPixelSize(fontSize);
    QFontMetricsF apFm(apFont);
    // 1) Ngắt dòng TRƯỚC khi đo: realAdv = MAX advance của từng dòng (không
    // phải cả chuỗi như một dòng — lỗi 0902 chỉ ẩn vì thường chỉ có 1 dòng).
    QStringList apLines = trWrapFreeText(text, wrapW, apFm);
    double realAdv = 0.0;
    for (const QString& ln : apLines)
        realAdv = qMax(realAdv, apFm.horizontalAdvance(ln));
    float fs = fontSize;
    if (realAdv > usable_w - 8.0f) {
        fs = fontSize * (usable_w - 8.0f) / (realAdv > 0.0f ? realAdv : 1.0f);
        if (fs < 6.0f) fs = 6.0f;
        // Đã thu nhỏ cỡ chữ ⇒ bề rộng ngắt mềm PHẢI ngắt lại theo cỡ mới,
        // nếu không thì dòng vẫn tràn ra ngoài ô (đúng lỗi ta đang sửa).
        apFont = trDejaVuFontAtPixelSize(fs);
        apFm = QFontMetricsF(apFont);
        apLines = trWrapFreeText(text, wrapW, apFm);
    }
    const double lineAdv = apFm.lineSpacing();
    if (apLines.isEmpty()) apLines << QString();
    QStringList hexLines;
    hexLines.reserve(apLines.size());
    for (const QString& ln : apLines)
        hexLines << glyphHexIdentityH(m_unicodeFontData, ln);
    if (text.isEmpty()) hexLines.clear();
    if (hexLines.isEmpty())
        qWarning() << "[inote] GLYPH MAP FAILED — /AP khong co chu (Contents van du):" << text;

    QByteArray ap;
    ap.append("q\n");
    if (rot == 1)
        ap.append(QString("0 -1 1 0 0 %1 cm\n").arg(h, 0, 'f', 2).toUtf8());
    else if (rot == 2)
        ap.append(QString("-1 0 0 -1 %1 %2 cm\n").arg(w, 0, 'f', 2).arg(h, 0, 'f', 2).toUtf8());
    else if (rot == 3)
        ap.append(QString("0 1 -1 0 %1 0 cm\n").arg(w, 0, 'f', 2).toUtf8());
    if (withBackground) {
        ap.append("1 1 0.588 rg\n");
        ap.append(QString("0 0 %1 %2 re f\n").arg(w, 0, 'f', 2).arg(h, 0, 'f', 2).toUtf8());
    }
    ap.append("Q\n");
    ap.append(QString("q %1 %2 %3 RG 1 w 0.5 0.5 %4 %5 re S Q\n")
              .arg(textColor.redF(),   0, 'f', 3)
              .arg(textColor.greenF(), 0, 'f', 3)
              .arg(textColor.blueF(),  0, 'f', 3)
              .arg((double)w - 1.0, 0, 'f', 2)
              .arg((double)h - 1.0, 0, 'f', 2).toUtf8());
    // 🔴 0927 BƯỚC 1: mỗi dòng MỘT cặp Td + <hex> Tj, khoảng dòng =
    // fm.lineSpacing() — thay cho MỘT Tj cho toàn bộ chuỗi (file lưu ra
    // 1 dòng tràn khung dù màn hình có wrap). /FXF1 Identity-H giữ nguyên.
    if (!hexLines.isEmpty()) {
        ap.append(QString("BT /FXF1 %1 Tf %2 %3 %4 rg\n")
                  .arg(fs, 0, 'f', 1)
                  .arg(textColor.redF(),   0, 'f', 3)
                  .arg(textColor.greenF(), 0, 'f', 3)
                  .arg(textColor.blueF(),  0, 'f', 3).toUtf8());
        // Td đầu: x=apPad, y = cao ô − apPad − lineAdv (mốc nền dòng đầu nằm
        // trong ô khi chỉ 1 dòng — khớp đường cũ "4 %1 Td" với %1 = fs+2).
        for (int i = 0; i < apLines.size(); ++i) {
            const QString& hex = hexLines.at(i);
            if (hex.isEmpty()) continue;
            if (i == 0) {
                ap.append(QString("%1 %2 Td\n")
                          .arg(apPad, 0, 'f', 2)
                          .arg(usable_h - apPad - lineAdv, 0, 'f', 2).toUtf8());
            } else {
                ap.append(QString("0 %1 Td\n").arg(-lineAdv, 0, 'f', 2).toUtf8());
            }
            ap.append(QString("<%1> Tj\n").arg(hex).toUtf8());
        }
        ap.append("ET\n");
    }
    qDebug() << "[inote] apContent=" << ap.left(300);
    qDebug().noquote() << "[inote] AP lines=" << apLines.size()
                       << "fs=" << fs << "wrapW=" << wrapW
                       << "lineAdv=" << lineAdv << "box=" << w << "x" << h;
    QString apStr = QString::fromUtf8(ap);
    bool setOk = FPDFAnnot_SetAP(annot, FPDF_ANNOT_APPEARANCEMODE_NORMAL,
        reinterpret_cast<FPDF_WIDESTRING>(apStr.utf16()));
    unsigned long apLenAfter = FPDFAnnot_GetAP(annot, FPDF_ANNOT_APPEARANCEMODE_NORMAL, nullptr, 0);
    qDebug() << "[inote] SetAP returned=" << (setOk ? "true" : "false") << "apLenAfter=" << apLenAfter;
    // 🔴 0927 LƯỢT 2: SetAP trả false (PDFium bug #1381) ⇒ /AP không vào
    // tệp. Ta ghi nội dung /AP xuống sidecar + đánh dấu trang; patchOwnNoteAp
    // sẽ QPDF dựng /AP/N thật lúc save. KHÔNG giời suy — đã đo bằng chính
    // SetAP trong log trên.
    // Ghi sidecar KHÔNG PHẦI Ở RIÊNG NHÁNH !setOk: kể cả khi PDFium đã tự ghi
    // /AP được, ta vẫn phải đẩy dict chú thích thành INDIRECT (bug #2 — dict
    // trực tiếp trong /Annots thì không trình đọc PDF nào nhận). Còn nội dung
    // /AP thì luôn lấy từ ta tự dựng (ap) — đó là nguồn chân lý, không phải
    // bản PDFium trả về.
    if (!ap.isEmpty()) {
        QFile side(m_path + ".traownap");
        if (side.open(QIODevice::WriteOnly | QIODevice::Append)) {
            // Bản ghi: page \t TRID \t w \t h \t base64(/AP). w/h là kích thước
            // ô theo hệ toạ độ MÀ NỘI DUNG /AP VẼ (origin 0,0) — dùng làm /BBox.
            side.write(QByteArray::number(pageIndex) + '\t'
                       + trid.toLatin1() + '\t'
                       + QByteArray::number(w, 'f', 2) + '\t'
                       + QByteArray::number(h, 'f', 2) + '\t'
                       + ap.toBase64() + '\n');
            side.close();
            m_apOwnPending.insert(pageIndex);
            // 🔴 0927 LƯỢT 3: giữ /AP trong BỘ NHỚ làm nguồn sự thật lúc vá.
            // Sidecar vẫn ghi (khôi phục khi crash) nhưng không được phép là nguồn
            // duy nhất: m_path đổi giữa lúc tạo annot và lúc save (bản nhập trong
            // %TEMP%), nên đọc lại theo đường dẫn luôn trượt ⇒ /AP mất.
            const QString apKey = ownApKey(pageIndex, trid);
            m_ownApInMem.insert(apKey, ap);
            m_ownApBox.insert(apKey, QSizeF(w, h));
        } else {
            qWarning() << "[ownap] khong mo duoc sidecar" << m_path
                       << "— /AP co the mat";
            m_ownApFailed = true;
        }
    }
    FPDFPage_CloseAnnot(annot);

    if (outInfo) {
        outInfo->pageIndex = pageIndex;
        outInfo->type   = "FreeText";
        outInfo->text   = text;
        outInfo->author = author;
        outInfo->rect   = rectPdf;
        outInfo->color  = QColor(255, 255, 150, 200);
    }
    qDebug() << "[inote] done annotCount=" << FPDFPage_GetAnnotCount(page);
    return true;
}

bool AnnotationManager::createPopupNote_locked(FPDF_PAGE page, int pageIndex, QPointF pointDisp,
                                                const QString& text, const QString& author,
                                                AnnotInfo* outInfo) {
    if (!m_doc) return false;

    double pageH = FPDF_GetPageHeight(page);
    double pageW = FPDF_GetPageWidth(page);
    int rot = FPDFPage_GetRotation(page);
    const QPointF box = pdfBoxOrigin(page);

    QPointF pt = dispToPdf(pointDisp.x(), pointDisp.y(), pageW, pageH, rot, box.x(), box.y());
    float x0 = static_cast<float>(pt.x());
    float y0 = static_cast<float>(pt.y());
    float iconW = 40.0f, iconH = 40.0f;
    unsigned int noteId = m_nextNoteId++;

    double a, b, c, d;
    double tx, ty;
    switch (rot) {
        case 1: a = 0.0; b = 1.0; c = -1.0; d = 0.0;
                tx = x0 + iconW; ty = y0; break;
        case 2: a = -1.0; b = 0.0; c = 0.0; d = -1.0;
                tx = x0 + iconW; ty = y0 + iconH; break;
        case 3: a = 0.0; b = -1.0; c = 1.0; d = 0.0;
                tx = x0; ty = y0 + iconH; break;
        default: a = 1.0; b = 0.0; c = 0.0; d = 1.0;
                tx = x0; ty = y0; break;
    }

    FPDF_PAGEOBJECT body = FPDFPageObj_CreateNewPath(0, 0);
    if (body) {
        FPDFPath_LineTo(body, iconW, 0);
        FPDFPath_LineTo(body, iconW, iconH);
        FPDFPath_LineTo(body, 0, iconH);
        FPDFPath_Close(body);
        FPDFPageObj_SetFillColor(body, 255, 220, 0, 255);
        FPDFPath_SetDrawMode(body, FPDF_FILLMODE_ALTERNATE, false);
        FPDFPageObj_Transform(body, a, b, c, d, tx, ty);
        FPDF_PAGEOBJECTMARK bodyMk = FPDFPageObj_AddMark(body, "TRNote");
        if (bodyMk)
            FPDFPageObjMark_SetIntParam(m_doc, body, bodyMk, "id", static_cast<int>(noteId));
        FPDFPage_InsertObject(page, body);
    }

    FPDF_PAGEOBJECT border = FPDFPageObj_CreateNewPath(0, 0);
    if (border) {
        FPDFPath_LineTo(border, iconW, 0);
        FPDFPath_LineTo(border, iconW, iconH);
        FPDFPath_LineTo(border, 0, iconH);
        FPDFPath_Close(border);
        FPDFPageObj_SetStrokeColor(border, 180, 140, 0, 255);
        FPDFPageObj_SetStrokeWidth(border, 1.5f);
        FPDFPath_SetDrawMode(border, FPDF_FILLMODE_NONE, true);
        FPDFPageObj_Transform(border, a, b, c, d, tx, ty);
        FPDF_PAGEOBJECTMARK borderMk = FPDFPageObj_AddMark(border, "TRNote");
        if (borderMk)
            FPDFPageObjMark_SetIntParam(m_doc, border, borderMk, "id", static_cast<int>(noteId));
        FPDFPage_InsertObject(page, border);
    }

    auto makeLine = [&](float ly) {
        FPDF_PAGEOBJECT line = FPDFPageObj_CreateNewPath(8.0f, ly);
        if (line) {
            FPDFPath_LineTo(line, 32.0f, ly);
            FPDFPageObj_SetStrokeColor(line, 120, 90, 0, 255);
            FPDFPageObj_SetStrokeWidth(line, 1.5f);
            FPDFPath_SetDrawMode(line, FPDF_FILLMODE_NONE, true);
            FPDFPageObj_Transform(line, a, b, c, d, tx, ty);
            FPDF_PAGEOBJECTMARK mk = FPDFPageObj_AddMark(line, "TRNote");
            if (mk)
                FPDFPageObjMark_SetIntParam(m_doc, line, mk, "id", static_cast<int>(noteId));
            FPDFPage_InsertObject(page, line);
        }
    };
    makeLine(12.0f);
    makeLine(20.0f);
    makeLine(28.0f);

    FPDF_ANNOTATION annot = FPDFPage_CreateAnnot(page, FPDF_ANNOT_TEXT);
    if (!annot) { m_lastError = "Cannot create annotation"; return false; }

    FS_RECTF rect{ x0, y0, x0 + iconW, y0 + iconH };
    FPDFAnnot_SetRect(annot, &rect);
    FPDFAnnot_SetColor(annot, FPDFANNOT_COLORTYPE_Color, 255, 220, 0, 255);
    FPDFAnnot_SetStringValue(annot, "Contents",
        reinterpret_cast<FPDF_WIDESTRING>(text.utf16()));
    if (!author.isEmpty())
        FPDFAnnot_SetStringValue(annot, "T",
            reinterpret_cast<FPDF_WIDESTRING>(author.utf16()));

    QString trid = QString::number(noteId);
    FPDFAnnot_SetStringValue(annot, "TRID",
        reinterpret_cast<FPDF_WIDESTRING>(trid.utf16()));
    m_lastCreatedUid = generateUid();
    FPDFAnnot_SetStringValue(annot, "TRUID",
        reinterpret_cast<FPDF_WIDESTRING>(m_lastCreatedUid.utf16()));

    FPDFAnnot_SetFlags(annot, FPDFAnnot_GetFlags(annot) | FPDF_ANNOT_FLAG_HIDDEN);
    FPDFPage_CloseAnnot(annot);

    if (outInfo) {
        outInfo->pageIndex = pageIndex;
        outInfo->type   = "Note";
        outInfo->text   = text;
        outInfo->author = author;
        outInfo->rect   = QRectF(pointDisp, QSizeF(40, 40));
        outInfo->color  = QColor(255, 220, 0, 255);
    }
    return true;
}

// Kept for --foreignbench headless benchmark (main.cpp). Not used by GUI.
QImage AnnotationManager::buildForeignAnnotLayer(int pageIndex, int wPx, int hPx) {
    if (!m_doc || wPx <= 0 || hPx <= 0) return QImage();
    FPDF_PAGE page = nullptr;
    QVector<int> hiddenByUs;
    QImage plain, withA;
    {
        TimedPdfiumLock lock(__FILE__, __LINE__);
        page = PageCache::acquire(m_doc, pageIndex);
        if (!page) return QImage();
        PageCache::PageBorrow _b(m_doc, pageIndex);   // R1: cap doi acquire()
        int foreignCount = 0;
        const int n = FPDFPage_GetAnnotCount(page);
        for (int i = 0; i < n; ++i) {
            FPDF_ANNOTATION a = FPDFPage_GetAnnot(page, i);
            if (!a) continue;
            const int flags = FPDFAnnot_GetFlags(a);
            const bool alreadyHidden = (flags & FPDF_ANNOT_FLAG_HIDDEN) != 0;
            const bool isOurs = FPDFAnnot_HasKey(a, "TRUID") != 0;
            if (isOurs && !alreadyHidden) {
                FPDFAnnot_SetFlags(a, flags | FPDF_ANNOT_FLAG_HIDDEN);
                hiddenByUs.append(i);
            } else if (!isOurs && !alreadyHidden
                       && FPDFAnnot_GetSubtype(a) != FPDF_ANNOT_POPUP) {
                ++foreignCount;
            }
            FPDFPage_CloseAnnot(a);
        }
        if (foreignCount == 0) {
            for (int i : hiddenByUs) {
                FPDF_ANNOTATION a = FPDFPage_GetAnnot(page, i);
                if (!a) continue;
                FPDFAnnot_SetFlags(a, FPDFAnnot_GetFlags(a) & ~FPDF_ANNOT_FLAG_HIDDEN);
                FPDFPage_CloseAnnot(a);
            }
            return QImage();
        }
        auto renderTo = [&](int flags) -> QImage {
            QImage img(wPx, hPx, QImage::Format_ARGB32);
            img.fill(Qt::white);
            FPDF_BITMAP bmp = FPDFBitmap_CreateEx(wPx, hPx, FPDFBitmap_BGRA,
                                                  img.bits(), img.bytesPerLine());
            if (bmp) {
                FPDFBitmap_FillRect(bmp, 0, 0, wPx, hPx, 0xFFFFFFFF);
                FPDF_RenderPageBitmap(bmp, page, 0, 0, wPx, hPx, 0, flags);
                FPDFBitmap_Destroy(bmp);
            }
            return img;
        };
        plain = renderTo(0);
        withA = renderTo(FPDF_ANNOT);
        for (int i : hiddenByUs) {
            FPDF_ANNOTATION a = FPDFPage_GetAnnot(page, i);
            if (!a) continue;
            FPDFAnnot_SetFlags(a, FPDFAnnot_GetFlags(a) & ~FPDF_ANNOT_FLAG_HIDDEN);
            FPDFPage_CloseAnnot(a);
        }
    }
    QImage layer(wPx, hPx, QImage::Format_ARGB32);
    layer.fill(Qt::transparent);
    int diffCount = 0;
    for (int y = 0; y < hPx; ++y) {
        const QRgb* pa = reinterpret_cast<const QRgb*>(plain.constScanLine(y));
        const QRgb* pb = reinterpret_cast<const QRgb*>(withA.constScanLine(y));
        QRgb* po = reinterpret_cast<QRgb*>(layer.scanLine(y));
        for (int x = 0; x < wPx; ++x) {
            if ((pa[x] & 0x00FFFFFF) != (pb[x] & 0x00FFFFFF)) {
                po[x] = (pb[x] | 0xFF000000);
                ++diffCount;
            }
        }
    }
    if (diffCount == 0) return QImage();
    return layer;
}

int AnnotationManager::setOwnNoteObjectsActive(FPDF_PAGE page, bool active) {
    if (!page) return 0;
    int count = FPDFPage_CountObjects(page);
    int changed = 0;
    for (int i = 0; i < count; ++i) {
        FPDF_PAGEOBJECT obj = FPDFPage_GetObject(page, i);
        if (!obj) continue;
        int nmarks = FPDFPageObj_CountMarks(obj);
        if (nmarks <= 0) continue;
        bool isOwnNote = false;
        for (int m = 0; m < nmarks; ++m) {
            FPDF_PAGEOBJECTMARK mark = FPDFPageObj_GetMark(obj, static_cast<unsigned long>(m));
            if (!mark) continue;
            unsigned long nameLen = 0;
            if (!FPDFPageObjMark_GetName(mark, nullptr, 0, &nameLen)) continue;
            std::vector<unsigned short> nameBuf(nameLen / 2 + 1, 0);
            if (!FPDFPageObjMark_GetName(mark, reinterpret_cast<FPDF_WCHAR*>(nameBuf.data()),
                                          nameLen, &nameLen)) continue;
            QString markName = QString::fromUtf16(reinterpret_cast<const char16_t*>(nameBuf.data()));
            if (markName.startsWith(QLatin1String("TRNote"))) {
                isOwnNote = true;
                break;
            }
        }
        if (isOwnNote) {
            FPDFPageObj_SetIsActive(obj, active ? 1 : 0);
            ++changed;
        }
    }
    return changed;
}

// ── end _locked helpers ─────────────────────────────────────────────────

int AnnotationManager::removeNotePageObjects(int pageIndex, unsigned int noteId) {
    if (!m_doc) return 0;
    TimedPdfiumLock lock(__FILE__, __LINE__);
    FPDF_PAGE page = PageCache::acquire(m_doc, pageIndex);
    if (!page) return 0;
    PageCache::PageBorrow _b(m_doc, pageIndex);   // R1: cap doi acquire()
    int removed = removeNotePageObjects_locked(page, noteId);
    if (removed > 0) {
        setOwnNoteObjectsActive(page, true);
        QElapsedTimer _gt; _gt.start();
        FPDFPage_GenerateContent(page);
        qDebug().noquote() << "[perf] genContent page=" << pageIndex << "ms=" << _gt.elapsed();
        invalidateNoteObjCache_locked(pageIndex);
        PageCache::bumpAnnotGeneration(m_doc, pageIndex);
        
    }
    if (removed > 0) bumpPageRevision(pageIndex);
    return removed;
}

void AnnotationManager::generateContentForPage(int page) {
    if (!m_doc) return;
    TimedPdfiumLock lock(__FILE__, __LINE__);
    FPDF_PAGE fpage = PageCache::acquire(m_doc, page);
    if (fpage) {
        PageCache::PageBorrow _b(m_doc, page);   // R1: cap doi acquire()
        setOwnNoteObjectsActive(fpage, true);
        QElapsedTimer _gt; _gt.start();
        FPDFPage_GenerateContent(fpage);
        qDebug().noquote() << "[perf] genContent page=" << page << "ms=" << _gt.elapsed();
        PageCache::bumpAnnotGeneration(m_doc, page);

        bumpPageRevision(page);
    }
}

void AnnotationManager::flushPendingGenerate(int page) {
    if (!m_pendingGenerate.contains(page)) return;
    if (!m_doc) { m_pendingGenerate.remove(page); return; }
    QElapsedTimer _gt;
    _gt.start();
    TimedPdfiumLock lock(__FILE__, __LINE__);
    FPDF_PAGE fpage = PageCache::acquire(m_doc, page);
    if (fpage) {
        PageCache::PageBorrow _b(m_doc, page);   // R1: cap doi acquire()
        setOwnNoteObjectsActive(fpage, true);
        FPDFPage_GenerateContent(fpage);
        qDebug().noquote() << "[perf] genContent page=" << page << "ms=" << _gt.elapsed();
        PageCache::bumpAnnotGeneration(m_doc, page);

        bumpPageRevision(page);
    }
    m_pendingGenerate.remove(page);
}

void AnnotationManager::flushAllPendingGenerate() {
    if (m_pendingGenerate.isEmpty()) return;
    QList<int> pages = m_pendingGenerate.values();
    std::sort(pages.begin(), pages.end());
    for (int page : pages) {
        flushPendingGenerate(page);
    }
}

// ── 🔴 0927 LƯỢT 2 — vá /AP cho FreeText CỦA TA (giống NẤC 1, nhưng KHÔNG
// phải giữ nền/viền của app khác: ô này do TA dựng, /AP là nguồn chân lý).
//
// VÌ SAO CẦN: đo 27/09 trên chính máy này —
//   [inote] AP lines=5 fs=12 wrapW=192 …
//   [inote] SetAP returned= false  apLenAfter=2
// ⇒ PDFium KHÔNG ghi được /AP (bug #1381, đúng lý do NẤC 1 đã phải vá cho
// FreeText ngoài). Hệ quả: file lưu ra có /Annots nhưng KHÔNG có /AP ⇒ không
// renderer độc lập nào (Acrobat, máy in, PyMuPDF) vẽ được chữ; đồng thời
// đếm số cặp `Tj` trong /AP/N luôn ra 0. Đây là lỗi gốc của "lines=0".
//
// CÁCH LÀM: không đoán vòng PDF. Ta ghi nội dung /AP xuống sidecar .traownap
// (mỗi dòng base64, có khoá /TRID để khớp đúng annot), đánh dấu trang vào
// m_apOwnPending. Lúc save (sau FPDF_SaveAsCopy đã ghi /Annots) ta mở lại tệp
// bằng QPDF, dựng /AP/N thật cho từng annot CỦA TA, xoá khoá tạm, ghi lại.
// Sở dĩ phải để PDFium ghi /Annots trước là vì PDFium ghi dict trực tiếp
// (không phải indirect object) — QPDF/đọc PDF khác đều BỎ QUA dict trực tiếp
// trong /Annots (đã đo: cùng annot, đổi indirect⇄direct là đủ để PyMuPDF thấy
// /không thấy). Nên không thể vá bằng cách chỉ chạy QPDF một mình.
int AnnotationManager::patchOwnNoteAp(const QString& path) {
    if (m_apOwnPending.isEmpty()) return 0;
    const QString side = path + ".traownap";
    // sidecar: "page\ttrid\tw\th\tbase64(/AP)\n" cho mỗi annot CỦA TA.
    struct OwnApEntry { QByteArray ap; double w; double h; };
    QHash<QString, OwnApEntry> want;   // key "page:trid" -> /AP + kích thước ô
    // 🔴 0927 LƯỢT 3 — NGUỒN 1: BỘ NHỚ (đúng theo vòng đời, không phụ thuộc
    // đường dẫn). Đây là nguồn ta dùng. Nguồn 2 (sidecar) chỉ để phục hồi khi
    // tiến trình chết giữa lúc tạo annot và lúc bấm Save.
    for (auto it = m_ownApInMem.constBegin(); it != m_ownApInMem.constEnd(); ++it) {
        const QSizeF box = m_ownApBox.value(it.key());
        want.insert(it.key(), OwnApEntry{ it.value(), box.width(), box.height() });
    }
    {   // Nguồn 2: sidecar — chỉ bổ sung cho khoá bộ nhớ chưa có.
        QFile sf(side);
        if (sf.open(QIODevice::ReadOnly)) {
            const QList<QByteArray> ls = sf.readAll().split('\n');
            for (const QByteArray& raw : ls) {
                const QList<QByteArray> f = raw.split('\t');
                if (f.size() != 5) continue;
                const QString key = QString::fromLatin1(f[0]) + QLatin1Char(':')
                                    + QString::fromLatin1(f[1]);
                if (want.contains(key)) continue;
                const QVariant wv(f[2]), hv(f[3]);
                want.insert(key, OwnApEntry{ QByteArray::fromBase64(f[4]),
                                             wv.toDouble(), hv.toDouble() });
            }
            sf.close();
        }
    }
    if (want.isEmpty()) {
        // 🔴 0927 LƯỢT 3: KHÔNG còn im lặng trả về 0. Trước đây sidecar hỏng /
        // không tồn tại ⇒ hàm trả 0 = "vá xong" trong khi /AP chưa từng vào file
        // ⇒ tệp lưu ra KHÔNG có chữ mà không ai báo. Nay báo thẳng để save
        // thất bại đúng luật 21/09, thay vì tạo file hỏng.
        m_lastError = QStringLiteral(
            "Mất nội dung /AP của chú thích vừa tạo (không còn trong bộ nhớ và "
            "không đọc được sidecar) — tệp sẽ không có chữ hiển thị");
        return -1;
    }
    const QString tmp = path + ".ownaptmp";
    // 🔴 0927 LUOT 5 (LOI 4) - FILE TAM KHONG BAO GI DUOC SOT.
    // NGUYEN NHAN DO (khong doan): `replaceFileAtomically` KHONG xoa nguon
    // (no copy srcTmp -> staged, roi rename staged -> dest) - xem comment o
    // restoreAnnotFromBackup: "replaceFileAtomically khong xoa nguon tmp".
    // Duong THANH CONG cuoi ham CHI goi QFile::remove(side) ma quen tmp =>
    // moi lan luu FreeText cua ta bo lai 1 file <ten>.tortmp.ownaptmp canh
    // benh canh (CEO thay 5 file sot trong %TEMP%).
    // Giai phap: guard huy tmp o CUOI HAM bang BAT KY duong thoat (return
    // kiem tra / nem loi QPDF) => khong con nhanh nao quen xoa, ke ca nhanh
    // moi them sau nay. It dien hon go remove o 6 noi, va khong sot duong
    // nao. QFile::remove tren duong dan khong con la false, nen vo hai.
    // KHONG dung co `alive`: moi duong thoat deu phai don - het thoi la don.
    struct OwnTmpGuard {
        QString p;
        ~OwnTmpGuard() { if (!p.isEmpty()) QFile::remove(p); }
    } tmpGuard{ tmp };
    int patched = 0;
    try {
        QPDF pdf;
        pdf.processFile(path.toUtf8().constData());
        auto pages = QPDFPageDocumentHelper(pdf).getAllPages();
        for (int pi = 0; pi < static_cast<int>(pages.size()); ++pi) {
            if (!m_apOwnPending.contains(pi)) continue;
            QPDFObjectHandle page = pages[pi].getObjectHandle();
            QPDFObjectHandle annots = page.getKey("/Annots");
            if (!annots.isArray()) continue;
            const QPDFObjectHandle font = findDejaVuFont(page);
            for (QPDFObjectHandle a : annots.getArrayAsVector()) {
                if (!a.isDictionary()) continue;
                if (a.getKey("/Subtype").getName() != "/FreeText") continue;
                if (a.getKey("/Type").getName() != "/Annot") continue;
                // CHỈ annot CỦA TA: có /TRID (đánh dấu của ta) — FreeText ngoài
                // không có, nên không đụng nhầm (đó là việc của rebuildForeignFreeTextAp).
                if (!a.hasKey("/TRID")) continue;
                // 2.4.3: bo const - qpdf 10.6 khong khai isString()/getStringValue() la const
                QPDFObjectHandle trid = a.getKey("/TRID");
                if (!trid.isString()) continue;
                const QString key = ownApKey(pi, QString::fromStdString(trid.getStringValue()));
                const auto it = want.constFind(key);
                if (it == want.constEnd()) continue;   // annot cua ta nhung /AP rong
                const OwnApEntry ent = it.value();
                const QByteArray& apBytes = ent.ap;
                const double apW = ent.w, apH = ent.h;
                if (apBytes.isEmpty() || apW <= 0.0 || apH <= 0.0) { QFile::remove(tmp); QFile::remove(side);
                    m_lastError = QStringLiteral("Nội dung /AP rỗng cho chú thích trang %1").arg(pi);
                    return -1; }
                if (!font.isInitialized()) {
                    QFile::remove(tmp); QFile::remove(side);
                    m_lastError = QStringLiteral(
                        "Không tìm thấy font DejaVu trên trang %1 — không dựng được /AP").arg(pi);
                    return -1;
                }
                // 2.4.3: chay ca qpdf 10.6 (Ubuntu 22.04) lan 11+ - pdf.newStream()
                // (method cua QPDF) chi co tu 11.2, ban 10.6 phai goi ham static
                // QPDFObjectHandle::newStream(QPDF*, ...)
#if defined(QPDF_MAJOR_VERSION) && QPDF_MAJOR_VERSION >= 11
                QPDFObjectHandle ns = pdf.newStream(std::string(apBytes.constData(), apBytes.size()));
#else
                QPDFObjectHandle ns = QPDFObjectHandle::newStream(&pdf,
                    std::string(apBytes.constData(), apBytes.size()));
#endif
                QPDFObjectHandle nd = ns.getDict();
                nd.replaceKey("/Type",  QPDFObjectHandle::newName("/XObject"));
                nd.replaceKey("/Subtype", QPDFObjectHandle::newName("/Form"));
                nd.replaceKey("/FormType", QPDFObjectHandle::newInteger(1));
                // 🔴 0927 LƯỢT 2 — BUG #4 (đo 27/09, mất nhiều giờ nhất).
                // /BBox PHẢI bọc nội dung /AP, mà nội dung ta ghi BẮT ĐẦU TỪ
                // GỐC 0,0 (x = apPad, y = cao ô − apPad − lineAdv, border rect
                // tại 0.5 0.5 w−1 h−1). Trước đây ta đặt /BBox = /Rect (toạ độ
                // TRANG) ⇒ vùng /AP lệch hẳn so với nơi chữ thật nằm ⇒ KHÔNG
                // renderer nào vẽ được. Đo: cùng tệp, đổi /BBox từ [60 168 260
                // 242] sang [0 0 200 74] ⇒ pixel đen trong ô đi từ 0 lên 4339.
                // Đừng "sửa" lại thành /Rect.
                nd.replaceKey("/BBox", QPDFObjectHandle::newArray(
                    std::vector<QPDFObjectHandle>{QPDFObjectHandle::newInteger(0),
                        QPDFObjectHandle::newInteger(0),
                        QPDFObjectHandle::newReal(apW), QPDFObjectHandle::newReal(apH)}));
                QPDFObjectHandle res = QPDFObjectHandle::newDictionary();
                QPDFObjectHandle fonts = QPDFObjectHandle::newDictionary();
                fonts.replaceKey("/FXF1", font);
                res.replaceKey("/Font", fonts);
                nd.replaceKey("/Resources", res);
                // /Matrix đơn vị: /AP vẽ trong hệ toạ độ /Rect (đã có sẵn ở
                // nội dung ta ghi — xem createInlineNote_locked: 4.00 … Td).
                nd.replaceKey("/Matrix", QPDFObjectHandle::newArray(
                    std::vector<QPDFObjectHandle>{QPDFObjectHandle::newInteger(1),
                        QPDFObjectHandle::newInteger(0), QPDFObjectHandle::newInteger(0),
                        QPDFObjectHandle::newInteger(1), QPDFObjectHandle::newInteger(0),
                        QPDFObjectHandle::newInteger(0)}));
                QPDFObjectHandle apd = QPDFObjectHandle::newDictionary();
                apd.replaceKey("/N", ns);
                a.replaceKey("/AP", apd);
                a.removeKey("/TR_AP_OWN_PEND");
                // 🔴 0927 LƯỢT 3: bảo đảm /F trong file KHÔNG có bit Hidden cho
                // FreeText CỦA TA (xem chú thích ở createInlineNote_locked).
                // Vá ở đây nữa để tệp đã tạo từ bản cũ vẫn mở ra thấy chữ —
                // sửa ở nơi tạo chỉ có tác dụng với chú thích MỚI.
                {
                    // 2.4.3: bo const - qpdf 10.6 khong khai isInteger()/getIntValue() la const
                    QPDFObjectHandle fv = a.getKey("/F");
                    if (fv.isInteger()) {
                        const int fl = fv.getIntValue() & ~2;   // 2 = Hidden
                        if (fl == 0) a.removeKey("/F");
                        else a.replaceKey("/F", QPDFObjectHandle::newInteger(fl));
                    }
                }
                // 🔴 0927 LƯỢT 2 — BUG #2 (đo 27/09): PDFium ghi dict chú thích
                // TRỰC TIẾP vào /Annots, không phải indirect object. Đo kiểm:
                // cùng một annot FreeText, chỉ đổi indirect⇄direct, PyMuPDF
                // thấy 1 annot / thấy 0 annot. ⇒ file app xuất ra VÔ DÙNG với
                // mọi trình đọc PDF ngoài (Acrobat, máy in, PyMuPDF). Sửa ở đây
                // bằng cách đẩy dict thành indirect rồi thay trong /Annots.
                // (Không sửa được ở tầng PDFium: FPDFPage_GetAnnotObjectIndex +
                //  FPDFPage_InsertObject không đổi được cách lưu dict.)
                QPDFObjectHandle ind = pdf.makeIndirectObject(a);
                QPDFObjectHandle newAnnots = QPDFObjectHandle::newArray();
                // 2.4.3: chep theo GIA TRI (khong phai const&) - qpdf 10.6 khong khai
                // isDictionary()/getKey()/getName()/isString()/getStringValue() la const
                for (QPDFObjectHandle old : annots.getArrayAsVector()) {
                    bool isTarget = false;
                    if (old.isDictionary() && old.getKey("/Type").getName() == "/Annot"
                        && old.getKey("/Subtype").getName() == "/FreeText"
                        && old.hasKey("/TRID")) {
                        isTarget = (old.getKey("/TRID").isString()
                            && QString::fromStdString(old.getKey("/TRID").getStringValue())
                               == QString::fromStdString(trid.getStringValue()));
                    }
                    newAnnots.appendItem(isTarget ? ind : old);
                }
                page.replaceKey("/Annots", newAnnots);
                annots = newAnnots;
                ++patched;
            }
        }
        if (patched == 0) {
            // Đánh dấu trang nhưng không vá được annot nào ⇒ KHÔNG được khai lưu xong.
            QFile::remove(tmp);
            m_lastError = QStringLiteral(
                "Không vá được /AP cho chú thích CỦA TA (không tìm thấy annot /TRID khớp) "
                "— tệp sẽ không có chữ hiển thị");
            return -1;
        }
        QPDFWriter w(pdf, tmp.toUtf8().constData());
        w.setObjectStreamMode(qpdf_o_generate);
        w.setStreamDataMode(qpdf_s_compress);
        w.setCompressStreams(true);
        w.write();
    } catch (const std::exception& e) {
        QFile::remove(tmp);
        QFile::remove(side);
        qWarning() << "[ownap] QPDF loi:" << e.what();
        m_lastError = QStringLiteral("QPDF lỗi khi vá /AP: ") + QString::fromUtf8(e.what());
        return -1;
    }
    extern bool replaceFileAtomically(const QString& srcTmp, const QString& dest, QString* errOut);
    QString err;
    if (!replaceFileAtomically(tmp, path, &err)) {
        QFile::remove(tmp);
        m_lastError = QStringLiteral("Không thay được tệp sau khi vá /AP: ") + err;
        return -1;
    }
    QFile::remove(side);
    m_apOwnPending.clear();
    m_ownApFailed = false;
    // /AP đã vào file ⇒ xoá bản nhớ, tránh vá lại các annot đã xong nếu user
    // tạo thêm chú thích rồi bấm Save lần hai (annot cũ đã có /AP thật).
    m_ownApInMem.clear();
    m_ownApBox.clear();
    qDebug().noquote() << "[ownap] da va /AP cho" << patched << "FreeText cua ta";
    return patched;
}

int AnnotationManager::rebuildForeignFreeTextAp(const QString& path) {
    if (m_unicodeFontData.isEmpty()) {
        QFile f(":/fonts/DejaVuSans.ttf");
        if (f.open(QIODevice::ReadOnly)) m_unicodeFontData = f.readAll();
    }
    if (m_unicodeFontData.isEmpty()) return -1;   // khong co font ⇒ khong the giu loi hua

    const QString tmp = path + ".apfttmp";
    // 🔴 0927 LUOT 5 (LOI 4) - cung nguyen nhan sot tmp voi ownaptmp:
    // duong thanh cong duoi chi xoa... khong. Them guard cho ca nhanh nay.
    struct ForeignTmpGuard {
        QString p;
        ~ForeignTmpGuard() { if (!p.isEmpty()) QFile::remove(p); }
    } ftTmpGuard{ tmp };
    int patched = 0;
    int failed  = 0;   // so annot CO /TR_AP_REBUILD ma khong va duoc ⇒ save phai that bai
    try {
        QPDF pdf;
        pdf.processFile(path.toUtf8().constData());
        auto pages = QPDFPageDocumentHelper(pdf).getAllPages();
        for (auto& ph : pages) {
            QPDFObjectHandle page = ph.getObjectHandle();
            QPDFObjectHandle annots = page.getKey("/Annots");
            if (!annots.isArray()) continue;
            const QPDFObjectHandle font = findDejaVuFont(page);
            for (auto& aref : annots.getArrayAsVector()) {
                QPDFObjectHandle a = aref;
                if (!a.isDictionary()) continue;
                if (a.getKey("/Subtype").getName() != "/FreeText") continue;
                if (a.hasKey("/TRUID") || a.hasKey("/TRID")) continue;
                if (!a.hasKey("/TR_AP_REBUILD")) continue;
                QPDFObjectHandle ap = a.getKey("/AP");
                if (!ap.isDictionary()) { ++failed; continue; }
                QPDFObjectHandle oldN = ap.getKey("/N");
                if (!oldN.isStream()) { ++failed; continue; }
                QPDFObjectHandle contents = a.getKey("/Contents");
                if (!contents.isString()) { ++failed; continue; }
                if (!font.isInitialized()) {
                    qWarning() << "[ftap] khong tim thay font DejaVu tren trang — bo qua";
                    ++failed; continue;
                }
#if QPDF_MAJOR_VERSION >= 11
                std::shared_ptr<Buffer> sbuf = oldN.getStreamData(qpdf_dl_all);
#else
                PointerHolder<Buffer> sbuf = oldN.getStreamData(qpdf_dl_all);
#endif
                QByteArray oldRaw(reinterpret_cast<const char*>(sbuf->getBuffer()),
                                  static_cast<int>(sbuf->getSize()));
                // Trộn 2 font: lấy bảng /Widths của font WinAnsi gốc (ArialMT không nhúng).
                QString winName;
                const QPDFObjectHandle winFont = pickWinAnsiFont(oldN, winName);
                const WinAnsiMetrics wm = readWinAnsiMetrics(winFont);
                if (!wm.valid) winName.clear();   // không có font WinAnsi → tất cả qua DejaVu
                QByteArray newContent;
                const int align = a.hasKey("/Q") ? a.getKey("/Q").getIntValueAsInt() : 0;
                // 🔎 0927 LUOT 7: /Matrix cua /AP co DON VI thi moi GIAN KHUNG
                // xuong duoi. Xoay/scale thi chi sua be rong ngat dong (noi dung chu
                // van dung toa do cu cua content stream, khong duoc doi he toa do).
                bool apIdentityMtx = true;
                {
                    QPDFObjectHandle om = oldN.getDict().getKey("/Matrix");
                    if (om.isArray() && om.getArrayAsVector().size() == 6) {
                        auto mv = om.getArrayAsVector();
                        const double a1 = mv[0].getNumericValue();
                        const double b1 = mv[1].getNumericValue();
                        const double c1 = mv[2].getNumericValue();
                        const double d1 = mv[3].getNumericValue();
                        const double e1 = mv[4].getNumericValue();
                        const double f1 = mv[5].getNumericValue();
                        apIdentityMtx = (qFuzzyCompare(1.0 + a1, 1.0 + 1.0) &&
                                         qFuzzyIsNull(b1) && qFuzzyIsNull(c1) &&
                                         qFuzzyCompare(1.0 + d1, 1.0 + 1.0) &&
                                         qFuzzyIsNull(e1) && qFuzzyIsNull(f1));
                    }
                }
                ApFit fit;
                if (a.getKey("/Rect").isArray()) {
                    auto rv = a.getKey("/Rect").getArrayAsVector();
                    if (rv.size() == 4)
                        for (int q = 0; q < 4; ++q) fit.rectBefore[q] = rv[q].getNumericValue();
                }
                for (int q = 0; q < 4; ++q) fit.rectAfter[q] = fit.rectBefore[q];
                if (!buildForeignApContent(oldRaw, QString::fromStdString(contents.getUTF8Value()),
                                           m_unicodeFontData, align, wm, winName, newContent,
                                           &fit, apIdentityMtx)) {
                    qWarning() << "[ftap] khong phan tich duoc /AP cu — bo qua";
                    ++failed; continue;
                }
                // 🔴 P1a (0921): ghi DUNG MOT LAN. Lan sua 2 (sau khi Save da nap lai
                // /AP = ban CUA TA) KHONG duoc de /TR_AP_ORIG ⇒ ban Adobe goc khong mat.
                if (!a.hasKey("/TR_AP_ORIG"))
                    a.replaceKey("/TR_AP_ORIG", oldN);
#if QPDF_MAJOR_VERSION >= 11
                QPDFObjectHandle ns = pdf.newStream(
                    std::string(newContent.constData(), newContent.size()));
#else
                QPDFObjectHandle ns = QPDFObjectHandle::newStream(&pdf,
                    std::string(newContent.constData(), newContent.size()));
#endif
                QPDFObjectHandle nd = ns.getDict();   // sua dict, KHONG sua handle stream
                nd.replaceKey("/Type", QPDFObjectHandle::newName("/XObject"));
                nd.replaceKey("/Subtype", QPDFObjectHandle::newName("/Form"));
                nd.replaceKey("/FormType", QPDFObjectHandle::newInteger(1));
                QPDFObjectHandle oldDict = oldN.getDict();
                QPDFObjectHandle bbox = oldDict.getKey("/BBox");
                // 🔎 0927 LUOT 7: /BBox phai GIONG HOP NEN da gian trong
                // content stream. Ha can duoi giu mep tren + mep trai.
                if (bbox.isArray()) {
                    if (fit.expanded) {
                        auto bv = bbox.getArrayAsVector();
                        if (bv.size() == 4) {
                            const double bx0 = bv[0].getNumericValue();
                            const double by0 = bv[1].getNumericValue();
                            const double bx1 = bv[2].getNumericValue();
                            const double by1 = bv[3].getNumericValue();
                            // can duoi moi = can duoi cu - delta
                            nd.replaceKey("/BBox", QPDFObjectHandle::newArray(
                                std::vector<QPDFObjectHandle>{
                                    QPDFObjectHandle::newReal(qRound(bx0)),
                                    QPDFObjectHandle::newReal(qRound(by0 - fit.delta)),
                                    QPDFObjectHandle::newReal(qRound(bx1)),
                                    QPDFObjectHandle::newReal(qRound(by1))}));
                        }
                    } else {
                        nd.replaceKey("/BBox", bbox);
                    }
                }
                QPDFObjectHandle mtx = oldDict.getKey("/Matrix");
                if (mtx.isArray()) nd.replaceKey("/Matrix", mtx);
                // Giữ nguyên /Resources gốc (font WinAnsi /FXF1 + mọi thứ prefix cần),
                // chỉ thêm /FTR = DejaVu nhúng cho ký tự ngoài WinAnsi.
                QPDFObjectHandle res = oldDict.getKey("/Resources");
                if (!res.isDictionary()) res = QPDFObjectHandle::newDictionary();
                QPDFObjectHandle fonts = res.getKey("/Font");
                if (!fonts.isDictionary()) {
                    fonts = QPDFObjectHandle::newDictionary();
                    res.replaceKey("/Font", fonts);
                }
                fonts.replaceKey("/FTR", font);
                nd.replaceKey("/Resources", res);
                ap.replaceKey("/N", ns);
                // 📝 (0921) PHẠM VI restore: lưu /RC gốc (rich text) trước khi xoá để
                // restoreForeignAp trả lại được. Ghi đúng MỘT lần (lần sửa 2 /RC đã mất).
                // 🔎 0927 LUOT 7: ha /Rect cho khop /BBox da gian. Giu x0,y1
                // (mep tren + mep trai), chi ha y0. KHONG dung /CL (mui ten
                // FreeTextCallout giu nguyen). /RD (neu co) dich cung delta de
                // hop nen trung vung.
                if (fit.expanded && a.getKey("/Rect").isArray()) {
                    auto rv = a.getKey("/Rect").getArrayAsVector();
                    if (rv.size() == 4) {
                        const double rx0 = rv[0].getNumericValue();
                        const double ry0 = rv[1].getNumericValue();
                        const double rx1 = rv[2].getNumericValue();
                        const double ry1 = rv[3].getNumericValue();
                        fit.rectAfter[0] = rx0; fit.rectAfter[1] = ry0 - fit.delta;
                        fit.rectAfter[2] = rx1; fit.rectAfter[3] = ry1;
                        std::vector<QPDFObjectHandle> rnums;
                        for (int q = 0; q < 4; ++q)
                            rnums.push_back(QPDFObjectHandle::newReal(
                                qRound(fit.rectAfter[q])));
                        a.replaceKey("/Rect", QPDFObjectHandle::newArray(rnums));
                        // 🔎 0927 LUOT 7 (sau review): /RD la INSET tuong doi
                        // (khong phai toa do tuyet doi):
                        //     inner_y0 = Rect.y0 + RD[1]
                        //     inner_y1 = Rect.y1 - RD[3]
                        // /Rect.y0 da ha delta nen inner_y0 cung ha delta DUNG khi
                        // RD GIU NGUYEN. Ha them RD[1]/RD[3] se lam inner box tuot
                        // 2*delta ⇒ lech khung o annot co /RD (FreeText co vien).
                        // => KHONG sua /RD o day.
                    }
                }
                if (a.hasKey("/RC") && !a.hasKey("/TR_RC_ORIG"))
                    a.replaceKey("/TR_RC_ORIG", a.getKey("/RC"));
                a.removeKey("/RC");                              // 1 nguon su that
                a.removeKey("/TR_AP_REBUILD");
                ++patched;
            }
        }
        if (patched > 0) {
            QPDFWriter w(pdf, tmp.toUtf8().constData());
            w.setObjectStreamMode(qpdf_o_generate);
            w.setStreamDataMode(qpdf_s_compress);
            w.setCompressStreams(true);
            w.write();
        }
    } catch (const std::exception& e) {
        qWarning() << "[ftap] QPDF loi:" << e.what();
        QFile::remove(tmp);
        return -1;
    }
    if (failed > 0) {
        // Co annot can va ma khong va duoc ⇒ /Contents moi + /AP cu = hai ban su that.
        // Bo ban tmp de ben goi bao luu that bai, khong de GUI khai da luu.
        QFile::remove(tmp);
        qWarning() << "[ftap] that bai:" << failed << "annot khong va duoc /AP";
        return -1;
    }
    if (patched > 0) {
        // 🔴 P1c (0921): dung replaceFileAtomically (KHONG remove roi rename): rename
        // hong giua chung = mat ban tmp cua GUI ma save van tuong la xong.
        extern bool replaceFileAtomically(const QString& srcTmp, const QString& dest, QString* errOut);
        QString err;
        if (!replaceFileAtomically(tmp, path, &err)) {
            QFile::remove(tmp);
            qWarning() << "[ftap] khong thay duoc tep:" << err;
            return -1;
        }
        qDebug().noquote() << "[ftap] da vá /AP cho" << patched << "FreeText ngoai";
    }
    return patched;
}

// 🔴 P1 (0921): DUONG HOAN TAC — PHẠM VI ĐÚNG: trả lại /AP/N từ /TR_AP_ORIG,
// /Contents từ /TR_CONTENTS_ORIG và /RC từ /TR_RC_ORIG, rồi xoá các khoá. KHÔNG
// phải "hoàn tác thẳng": vật thể neo font DejaVu đã chèn vào nội dung trang vẫn còn
// (không sao lưu) — chỉ nội dung annot được trả về bản gốc. Không dùng GUI; harness
// --ftap-restore thử. Trả false + đặt m_lastError nếu thiếu khoá gốc hoặc ghi hỏng.
bool AnnotationManager::restoreForeignAp(int pageIndex, int index) {
    if (m_unicodeFontData.isEmpty()) {
        QFile f(":/fonts/DejaVuSans.ttf");
        if (f.open(QIODevice::ReadOnly)) m_unicodeFontData = f.readAll();
    }
    if (m_path.isEmpty()) { m_lastError = QStringLiteral("restoreForeignAp: chưa có đường tệp"); return false; }
    const QString tmp = m_path + ".apresttmp";
    // 🔴 0927 LUOT 5 (LOI 4) - cung nguyen nhan: replaceFileAtomically
    // khong xoa nguon tmp. Guard bao dam moi duong thoat deu don.
    struct RestoreTmpGuard {
        QString p;
        ~RestoreTmpGuard() { if (!p.isEmpty()) QFile::remove(p); }
    } rTmpGuard{ tmp };
    bool restored = false;
    try {
        QPDF pdf;
        pdf.processFile(m_path.toUtf8().constData());
        auto pages = QPDFPageDocumentHelper(pdf).getAllPages();
        if (pageIndex < 0 || pageIndex >= static_cast<int>(pages.size())) {
            m_lastError = QStringLiteral("restoreForeignAp: chỉ số trang ngoài phạm vi");
            return false;
        }
        QPDFObjectHandle page = pages[pageIndex].getObjectHandle();
        QPDFObjectHandle annots = page.getKey("/Annots");
        if (!annots.isArray()) {
            m_lastError = QStringLiteral("restoreForeignAp: trang không có /Annots");
            return false;
        }
        auto vec = annots.getArrayAsVector();
        if (index < 0 || index >= static_cast<int>(vec.size())) {
            m_lastError = QStringLiteral("restoreForeignAp: chỉ số annot ngoài phạm vi");
            return false;
        }
        QPDFObjectHandle a = vec[index];
        if (!a.isDictionary() || !a.hasKey("/TR_AP_ORIG")) {
            m_lastError = QStringLiteral("restoreForeignAp: annot không có /TR_AP_ORIG (không có gì để hoàn)");
            return false;
        }
        QPDFObjectHandle orig = a.getKey("/TR_AP_ORIG");
        QPDFObjectHandle ap = a.getKey("/AP");
        if (!ap.isDictionary()) {
            ap = QPDFObjectHandle::newDictionary();
            a.replaceKey("/AP", ap);
        }
        ap.replaceKey("/N", orig);
        a.removeKey("/TR_AP_ORIG");
        // (0921) trả luôn /Contents + /RC gốc nếu có sao lưu.
        if (a.hasKey("/TR_CONTENTS_ORIG")) {
            a.replaceKey("/Contents", a.getKey("/TR_CONTENTS_ORIG"));
            a.removeKey("/TR_CONTENTS_ORIG");
        }
        if (a.hasKey("/TR_RC_ORIG")) {
            a.replaceKey("/RC", a.getKey("/TR_RC_ORIG"));
            a.removeKey("/TR_RC_ORIG");
        }
        restored = true;
        QPDFWriter w(pdf, tmp.toUtf8().constData());
        w.setObjectStreamMode(qpdf_o_generate);
        w.setStreamDataMode(qpdf_s_compress);
        w.setCompressStreams(true);
        w.write();
    } catch (const std::exception& e) {
        qWarning() << "[ftap] restore QPDF loi:" << e.what();
        QFile::remove(tmp);
        m_lastError = QStringLiteral("restoreForeignAp: lỗi QPDF — ") + QString::fromUtf8(e.what());
        return false;
    }
    if (!restored) {
        QFile::remove(tmp);
        m_lastError = QStringLiteral("restoreForeignAp: không hoàn được");
        return false;
    }
    extern bool replaceFileAtomically(const QString& srcTmp, const QString& dest, QString* errOut);
    QString err;
    if (!replaceFileAtomically(tmp, m_path, &err)) {
        QFile::remove(tmp);
        m_lastError = QStringLiteral("restoreForeignAp: ") + err;
        qWarning() << "[ftap] restore khong thay duoc tep:" << err;
        return false;
    }
    return true;
}

// ═══ VIỆC 3-A3 (0921): "backup rồi xoá" — sao lưu NGUYÊN VẸN rồi mới xoá.
QString AnnotationManager::backupAnnotForDelete(int pageIndex, int index) {
    if (m_path.isEmpty()) { m_lastError = QStringLiteral("backupAnnotForDelete: chưa có đường tệp"); return {}; }
    // 1) Flush trạng thái in-memory (annot còn nguyên) xuống m_path để sidecar chụp đúng.
    if (!saveDocument()) {
        if (m_lastError.isEmpty()) m_lastError = QStringLiteral("backupAnnotForDelete: flush thất bại");
        return {};
    }
    const QString side = m_path + QStringLiteral(".trash-%1-%2-%3.pdf")
        .arg(pageIndex).arg(index).arg(QDateTime::currentMSecsSinceEpoch());
    QFile::remove(side);
    if (!QFile::copy(m_path, side)) {
        m_lastError = QStringLiteral("backupAnnotForDelete: không chép được sidecar");
        return {};
    }
    // 2) Xác nhận sidecar THẬT SỰ chứa annot đó (đọc bằng QPDF); sai thì bỏ, không xoá.
    try {
        QPDF pdf;
        pdf.processFile(side.toUtf8().constData());
        auto pages = QPDFPageDocumentHelper(pdf).getAllPages();
        if (pageIndex < 0 || pageIndex >= static_cast<int>(pages.size()))
            throw std::runtime_error("page out of range");
        QPDFObjectHandle annots = pages[pageIndex].getObjectHandle().getKey("/Annots");
        if (!annots.isArray() || index < 0 ||
            index >= static_cast<int>(annots.getArrayAsVector().size()))
            throw std::runtime_error("annot out of range");
    } catch (const std::exception& e) {
        QFile::remove(side);
        m_lastError = QStringLiteral("backupAnnotForDelete: sidecar không hợp lệ — ")
                      + QString::fromUtf8(e.what());
        return {};
    }
    return side;
}

bool AnnotationManager::restoreAnnotFromBackup(int pageIndex, int insertAt, const QString& backupPath) {
    if (m_path.isEmpty()) { m_lastError = QStringLiteral("restoreAnnotFromBackup: chưa có đường tệp"); return false; }
    if (!QFileInfo::exists(backupPath)) {
        m_lastError = QStringLiteral("restoreAnnotFromBackup: không thấy bản sao lưu");
        return false;
    }
    // Flush trạng thái HIỆN TẠI (annot đã bị xoá) xuống m_path trước khi chèn lại.
    if (!saveDocument()) {
        if (m_lastError.isEmpty()) m_lastError = QStringLiteral("restoreAnnotFromBackup: flush thất bại");
        return false;
    }
    const QString tmp = m_path + QStringLiteral(".aresttmp");
    // 🔴 0927 LUOT 5 (LOI 4) - cung nguyen nhan: guard don tmp moi duong thoat.
    struct RestoreAnnotTmpGuard {
        QString p;
        ~RestoreAnnotTmpGuard() { if (!p.isEmpty()) QFile::remove(p); }
    } raTmpGuard{ tmp };
    try {
        QPDF dest;
        dest.processFile(m_path.toUtf8().constData());
        QPDF src;
        src.processFile(backupPath.toUtf8().constData());
        auto dPages = QPDFPageDocumentHelper(dest).getAllPages();
        auto sPages = QPDFPageDocumentHelper(src).getAllPages();
        if (pageIndex < 0 || pageIndex >= static_cast<int>(dPages.size()) ||
            pageIndex >= static_cast<int>(sPages.size()))
            throw std::runtime_error("page out of range");
        QPDFObjectHandle sAnnots = sPages[pageIndex].getObjectHandle().getKey("/Annots");
        if (!sAnnots.isArray())
            throw std::runtime_error("backup has no /Annots");
        const int nSrc = static_cast<int>(sAnnots.getArrayAsVector().size());
        if (insertAt < 0 || insertAt >= nSrc)
            throw std::runtime_error("insert index out of range");
        QPDFObjectHandle pageDest = dPages[pageIndex].getObjectHandle();
        // Deep-copy giữ NGUYÊN mọi khoá + stream (copyForeignObject copy cả stream data).
        QPDFObjectHandle copy = dest.copyForeignObject(sAnnots.getArrayItem(insertAt));
        QPDFObjectHandle dAnnots = pageDest.getKey("/Annots");
        if (!dAnnots.isArray()) {
            dAnnots = QPDFObjectHandle::newArray();
            pageDest.replaceKey("/Annots", dAnnots);
        }
        const int nDst = static_cast<int>(dAnnots.getArrayAsVector().size());
        if (insertAt >= nDst) dAnnots.appendItem(copy);
        else                  dAnnots.insertItem(insertAt, copy);
        QPDFWriter w(dest, tmp.toUtf8().constData());
        w.setObjectStreamMode(qpdf_o_generate);
        w.setStreamDataMode(qpdf_s_preserve);   // giữ stream byte-bằng bản gốc
        w.write();
    } catch (const std::exception& e) {
        QFile::remove(tmp);
        m_lastError = QStringLiteral("restoreAnnotFromBackup: QPDF lỗi — ") + QString::fromUtf8(e.what());
        return false;
    }
    extern bool replaceFileAtomically(const QString& srcTmp, const QString& dest, QString* errOut);
    QString err;
    if (!replaceFileAtomically(tmp, m_path, &err)) {
        QFile::remove(tmp);
        m_lastError = QStringLiteral("restoreAnnotFromBackup: ") + err;
        return false;
    }
    return true;   // tmp da duoc don boi RestoreTmpGuard (khoi ra ham)
}

void AnnotationManager::discardAnnotBackup(const QString& backupPath) {
    if (!backupPath.isEmpty()) QFile::remove(backupPath);
}

bool AnnotationManager::saveDocument() {
    QFile file(m_path);
    if (!file.open(QIODevice::WriteOnly)) {
        m_lastError = "Cannot open file for writing: " + m_path;
        return false;
    }

    FileWriter fw;
    fw.base.version    = 1;
    fw.base.WriteBlock = FileWriter::WriteBlock;
    fw.file = &file;

    QElapsedTimer _w; _w.start();
    TimedPdfiumLock lock(__FILE__, __LINE__);
    if (_w.elapsed() > 300)
        qDebug().noquote() << "[lockwait] ms=" << _w.elapsed()
                           << "at" << __FILE__ << ":" << __LINE__ << "main="
                           << ((QThread::currentThread() == QCoreApplication::instance()->thread()) ? 1 : 0);
    {
        const auto pend = m_pendingGen;
        for (int p : pend) flushGenerate_locked(p);
    }
    // KHONG forgetDocument o day: giu dem am de thao tac sau save khong phai nap lai.
    // Doc se duoc forget khi dong/mo lai (PdfDocument::close / setDocument).
    bool ok = FPDF_SaveAsCopy(m_doc, &fw.base, FPDF_NO_INCREMENTAL) != 0;
    lock.unlock();

    file.close();
    if (!ok) { m_lastError = "FPDF_SaveAsCopy failed"; return false; }

    // 🔴 0927 LƯỢT 2: PDFium vừa ghi /Annots (dict TRỰC TIẾP — không đọc PDF nào
    // chấp nhận) nhưng KHÔNG ghi được /AP. Vá /AP cho FreeText CỦA TA ngay
    // bây giờ, TRƯỚC khi vá FreeText ngoài. Thất bại ⇒ save thất bại (tệp giữ
    // nguyên theo luật 21/09) chứ không khai lưu xong rồi mất chữ.
    if (!m_apOwnPending.isEmpty() || m_ownApFailed) {
        if (m_ownApFailed) {
            m_lastError = QStringLiteral(
                "Không ghi được /AP cho chú thích vừa tạo — tệp sẽ không có chữ hiển thị");
            return false;
        }
        if (patchOwnNoteAp(m_path) < 0) return false;
    }

    // 📐 NẤC 1: PDFium da ghi tep; QPDF vá /AP FreeText ngoai (nếu có đánh dấu).
    // 🔴 P1b (0921): neu co annot can va ma khong va duoc thi KHONG duoc khai da luu.
    // m_path la ban tmp cua GUI (chua ghi de tep goc) ⇒ tep nguoi dung van nguyen.
    if (m_hasForeignApEdits) {
        const int n = rebuildForeignFreeTextAp(m_path);
        if (n < 0) {
            m_lastError = QStringLiteral(
                "Không vá được /AP cho chú thích FreeText ngoài — đã HỦY lưu để tránh "
                "chữ mới đi kèm hình cũ; tệp gốc giữ nguyên");
            return false;
        }
        m_hasForeignApEdits = false;
    }
    return true;
}


