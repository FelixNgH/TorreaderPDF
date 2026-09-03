#include "AnnotationManager.h"
#include "../core/PdfCoords.h"
#include "../core/PageCache.h"
#include "../core/PdfiumLock.h"
#include "../core/OwnAnnotHideGuard.h"
#include <fpdf_edit.h>
#include <fpdf_save.h>
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
    return QString::fromUtf16(buf.data());
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

// Nong o hien thi (Y-down, pt) cho DU CHO chu o fontSizePt: chieu ngang theo
// advance THAT (QFontMetricsF, cung font voi /AP), chieu cao theo so dong wrap
// trong chieu ngang do — khong dung he so co dinh (length*5.5, h=18) nua.
QRectF trFreeTextFitRect(const QRectF& dispRect, const QString& text, float fontSizePt) {
    if (text.isEmpty() || fontSizePt <= 0.0f) return dispRect;
    const double pad = 2.0;                       // le pt moi ben (khop pad overlay)
    QFont f = trDejaVuFontAtPixelSize(fontSizePt); // zoom=1 => 1pt=1px
    QFontMetricsF fm(f);
    double adv = fm.horizontalAdvance(text);
    double w = qMax(dispRect.width(),  adv + 2.0 * pad);
    double h = qMax(dispRect.height(), fm.boundingRect(QRectF(0, 0, w, 1e6),
                        Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, text).height()
                        + 2.0 * pad);
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
    m_doc  = doc;
    m_path = filePath;
    m_unicodeFont = nullptr;
    m_unicodeFontData.clear();
}

AnnotationManager::~AnnotationManager() {
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

    for (int i = 0; i < count; ++i) {
        if (m_stopScan.load()) break;
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
                                                      bool* hasForeign) {
    QList<AnnotVisual> result;
    if (!m_doc) { if (outOverlayCapable) *outOverlayCapable = false; return result; }
    QElapsedTimer _perf;
    _perf.start();

    // 🔴 VIỆC 1 (SPEC_SMOOTH_123 31/08): GUI chi duoc tryLock(0). Neu GUI khong lay duoc
    // khoa → tra rong + overlayCapable=false (ben goi GIU du lieu cu, khong xoa markup),
    // viec that se chay o QtConcurrent (ben goi da co refreshAnnotVisuals). Duong GUI
    // goi truc tiep nay la DEFENSE-IN-DEPTH — MainWindow da day cache-miss sang nen.
    TryPdfiumLock lock(__FILE__, __LINE__);
    if (!lock.held()) {
        qDebug().noquote() << "[annot] loadPageVisuals SKIP — khoa ban page=" << page
                           << "(day sang luong nen)";
        if (outOverlayCapable) *outOverlayCapable = false;
        if (hasForeign) *hasForeign = false;
        return result;
    }
    QElapsedTimer _w; _w.start();
    FPDF_PAGE fpage = PageCache::acquire(m_doc, page);
    if (!fpage) { if (outOverlayCapable) *outOverlayCapable = false; return result; }
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
    if (!FPDFAnnot_HasKey(annot, "TRUID") && !FPDFAnnot_HasKey(annot, "TRID")) {
        FPDFPage_CloseAnnot(annot); return false;
    }

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
    createInlineNote_locked(page, pageIndex, fitRect, contents, author, false, newColor, newFontSize, &info);

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
    QElapsedTimer _totalT; _totalT.start();
    TimedPdfiumLock lock(__FILE__, __LINE__);
    FPDF_PAGE page = PageCache::acquire(m_doc, pageIndex);
    if (!page) return false;
    PageCache::PageBorrow _b(m_doc, pageIndex);   // R1: cap doi acquire()
    FPDF_ANNOTATION a = FPDFPage_GetAnnot(page, index);
    if (!a) return false;

    FS_RECTF r{};
    bool ok = FPDFAnnot_GetRect(a, &r);
    if (ok) {
        r.left += (float)dxU; r.right  += (float)dxU;
        r.top  += (float)dyU; r.bottom += (float)dyU;
        ok = FPDFAnnot_SetRect(a, &r);
    }
    const int sub = FPDFAnnot_GetSubtype(a);

    if (sub == FPDF_ANNOT_INK) {
        // Ink cua phan mem khac co /AP rieng — remove+recreate se lam mat.
        // Khong co API sua InkList tai cho ⇒ TU CHOI di chuyen, khong pha du lieu nguoi khac.
        if (!FPDFAnnot_HasKey(a, "TRUID") && !FPDFAnnot_HasKey(a, "TRID")) {
            FPDFPage_CloseAnnot(a);
            return false;
        }
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

        if (!FPDFPage_RemoveAnnot(page, index)) return false;

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

bool AnnotationManager::retextNote(int pageIndex, int index, const QString& newText) {
    if (!m_doc) return false;
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
        FPDFPage_CloseAnnot(annot); return false;
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
    if (subtype == FPDF_ANNOT_FREETEXT)
        createInlineNote_locked(page, pageIndex, fitRect, newText, author, false, parsedColor, parsedFontSize, &info);
    else
        createPopupNote_locked(page, pageIndex, fitRect.topLeft(), newText, author, &info);

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
    AnnotSnapshot s = snapshotAnnot(pageIndex, index);
    if (!s.valid) return false;
    if (s.subtype == FPDF_ANNOT_FREETEXT) {
        QColor daColor; float daSize = 11.0f;
        if (!parseDA(s.da, daColor, daSize) || daSize <= 0) daSize = 11.0f;
        return rebuildTextNote(pageIndex, index, color, daSize);
    }
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

bool AnnotationManager::updateNote(int pageIndex, int annotIndex, const QString& newText) {
    if (!m_doc) { m_lastError = "No document"; return false; }

    QElapsedTimer _w; _w.start();
    TimedPdfiumLock lock(__FILE__, __LINE__);
    if (_w.elapsed() > 300)
        qDebug().noquote() << "[lockwait] ms=" << _w.elapsed()
                           << "at" << __FILE__ << ":" << __LINE__ << "main="
                           << ((QThread::currentThread() == QCoreApplication::instance()->thread()) ? 1 : 0);
    FPDF_PAGE page = PageCache::acquire(m_doc, pageIndex);
    if (!page) { m_lastError = "Cannot load page"; return false; }
    PageCache::PageBorrow _b(m_doc, pageIndex);   // R1: cap doi acquire()

    FPDF_ANNOTATION annot = FPDFPage_GetAnnot(page, annotIndex);
    if (!annot) {
        m_lastError = "Annotation not found";
        return false;
    }

    FPDFAnnot_SetStringValue(annot, "Contents",
        reinterpret_cast<FPDF_WIDESTRING>(newText.utf16()));

    FPDFPage_CloseAnnot(annot);
    setOwnNoteObjectsActive(page, true);
    QElapsedTimer _gt; _gt.start();
    FPDFPage_GenerateContent(page);
    qDebug().noquote() << "[perf] genContent page=" << pageIndex << "ms=" << _gt.elapsed();
    PageCache::bumpAnnotGeneration(m_doc, pageIndex);
    lock.unlock();

    bumpPageRevision(pageIndex);
    bool ok = saveDocument();
    return ok;
}



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

    FS_RECTF rect{ xu_min, yu_min, xu_max, yu_max };
    FPDFAnnot_SetRect(annot, &rect);
    qDebug() << "[inote] real FreeText rect=" << rect.left << rect.bottom << rect.right << rect.top;

    QString daQ = QString("/Helv %1 Tf %2 %3 %4 rg")
        .arg(fontSize, 0, 'f', 1)
        .arg(textColor.redF(),   0, 'f', 3)
        .arg(textColor.greenF(), 0, 'f', 3)
        .arg(textColor.blueF(),  0, 'f', 3);
    FPDFAnnot_SetStringValue(annot, "DA",
        reinterpret_cast<FPDF_WIDESTRING>(daQ.utf16()));
    FPDFAnnot_SetStringValue(annot, "Contents",
        reinterpret_cast<FPDF_WIDESTRING>(text.utf16()));
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
    float fs = fontSize;
    const QFont apFont = trDejaVuFontAtPixelSize(fontSize);
    const float realAdv = static_cast<float>(QFontMetricsF(apFont).horizontalAdvance(text));
    if (realAdv > usable_w - 8.0f) {
        fs = fontSize * (usable_w - 8.0f) / (realAdv > 0.0f ? realAdv : 1.0f);
        if (fs < 6.0f) fs = 6.0f;
    }
    const QString hex = glyphHexIdentityH(m_unicodeFontData, text);
    if (hex.isEmpty() && !text.isEmpty())
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
    if (!hex.isEmpty()) {
        ap.append(QString("BT /FXF1 %1 Tf %2 %3 %4 rg 4 %5 Td <%6> Tj ET\n")
                  .arg(fs, 0, 'f', 1)
                  .arg(textColor.redF(),   0, 'f', 3)
                  .arg(textColor.greenF(), 0, 'f', 3)
                  .arg(textColor.blueF(),  0, 'f', 3)
                  .arg(fs + 2.0, 0, 'f', 1)
                  .arg(hex).toUtf8());
    }
    qDebug() << "[inote] apContent=" << ap.left(300);
    QString apStr = QString::fromUtf8(ap);
    bool setOk = FPDFAnnot_SetAP(annot, FPDF_ANNOT_APPEARANCEMODE_NORMAL,
        reinterpret_cast<FPDF_WIDESTRING>(apStr.utf16()));
    unsigned long apLenAfter = FPDFAnnot_GetAP(annot, FPDF_ANNOT_APPEARANCEMODE_NORMAL, nullptr, 0);
    qDebug() << "[inote] SetAP returned=" << (setOk ? "true" : "false") << "apLenAfter=" << apLenAfter;
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
    if (!ok) m_lastError = "FPDF_SaveAsCopy failed";
    return ok;
}


