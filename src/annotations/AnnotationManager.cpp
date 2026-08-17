#include "AnnotationManager.h"
#include "../core/PdfCoords.h"
#include "../core/PageCache.h"
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
#include <QDebug>
#include <QUuid>

// ── PDF escape helper (shared by both AP generators) ───────────────────────
static QByteArray pdfEscape(const QString& text) {
    QByteArray out;
    for (const QChar& ch : text) {
        if (ch == '\\') out.append("\\\\");
        else if (ch == '(') out.append("\\(");
        else if (ch == ')') out.append("\\)");
        else if (ch.unicode() < 32 || ch.unicode() > 126) out.append('?');
        else out.append(ch.toLatin1());
    }
    return out;
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
        case FPDF_ANNOT_WIDGET:      return "Widget";
        default:                     return "Annotation";
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
        QMutexLocker lk(&s_pdfiumMutex);
        const auto pend = m_pendingGen;
        for (int p : pend) flushGenerate_locked(p);
        if (m_doc && m_doc != doc) {
            // Tai lieu cu sap bi dong — xoa trang cua no khoi PageCache (con tro chet).
            PageCache::forgetDocument(m_doc);
        }
    }
    m_doc  = doc;
    m_path = filePath;
    m_unicodeFont = nullptr;
    m_unicodeFontData.clear();
}

AnnotationManager::~AnnotationManager() {
    {
        QMutexLocker lk(&s_pdfiumMutex);
        const auto pend = m_pendingGen;
        for (int p : pend) flushGenerate_locked(p);
        if (m_doc) PageCache::forgetDocument(m_doc);
    }
}

FPDF_PAGE AnnotationManager::acquireSharedPage(int pageIndex) {
    return PageCache::acquire(m_doc, pageIndex);
}

void AnnotationManager::pinPage_locked(int pageIndex) {
    PageCache::acquire(m_doc, pageIndex);
}

void AnnotationManager::pinPage(int pageIndex) { QMutexLocker lk(&s_pdfiumMutex); pinPage_locked(pageIndex); }

bool AnnotationManager::isSharedPage(int pageIndex) const {
    return PageCache::tryAcquire(m_doc, pageIndex) != nullptr;
}

void AnnotationManager::releaseSharedPage() {
    QMutexLocker lk(&s_pdfiumMutex);
    releaseSharedPage_locked();
}

void AnnotationManager::releaseSharedPage_locked() {
    if (m_doc) PageCache::forgetDocument(m_doc);
}

void AnnotationManager::flushGenerate_locked(int pageIndex) {
    if (!m_pendingGen.contains(pageIndex) || !m_doc) return;
    FPDF_PAGE p = PageCache::acquire(m_doc, pageIndex);
    if (p) {
        QElapsedTimer _gt; _gt.start();
        setOwnNoteObjectsActive(p, true);
        FPDFPage_GenerateContent(p);
        qDebug().noquote() << "[perf] genContent page=" << pageIndex << "ms=" << _gt.elapsed();
    }
    m_pendingGen.remove(pageIndex);
}

QList<AnnotInfo> AnnotationManager::loadPage(int pageIndex) {
    QList<AnnotInfo> result;
    if (!m_doc) return result;

    QMutexLocker lock(&s_pdfiumMutex);
    FPDF_PAGE page = PageCache::acquire(m_doc, pageIndex);
    if (!page) return result;

    double pageH = FPDF_GetPageHeight(page);
    double pageW = FPDF_GetPageWidth(page);
    int rot = FPDFPage_GetRotation(page);
    const QPointF box = pdfBoxOrigin(page);
    int count = FPDFPage_GetAnnotCount(page);

    QElapsedTimer _parseT;
    _parseT.start();

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
        if (m_stopScan.load()) return;
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

        QThread::yieldCurrentThread();
        QThread::msleep(1);
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
                     sub == FPDF_ANNOT_FREETEXT || sub == FPDF_ANNOT_TEXT);
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

    if (sub == FPDF_ANNOT_FREETEXT || sub == FPDF_ANNOT_TEXT)
        out.paintByOverlay = false;

    // AutoCAD sinh /Square annot VO HINH (SHX text): /Border=[0,0,0], khong /C,
    // /IC, /AP. Theo chuan khong duoc ve gi len trang (Adobe cung khong ve).
    // Bo qua overlay de khoi bi QPen ep thanh vien 1px do. Phai du CA BON dieu,
    // thieu mot lai co the giau nham annot that. Annot tu app luon co /C (duoc
    // FPDFAnnot_SetColor khi tao) nen khong bao gio dinh luat nay.
    if (!out.hasAP && !out.hasColor && !out.hasFill && out.border == 0.0f)
        out.paintByOverlay = false;

    return true;
}

QList<AnnotVisual> AnnotationManager::loadPageVisuals(int page, bool* outOverlayCapable,
                                                      bool* hasForeign) {
    QList<AnnotVisual> result;
    if (!m_doc) { if (outOverlayCapable) *outOverlayCapable = false; return result; }
    QElapsedTimer _perf;
    _perf.start();

    QMutexLocker lock(&s_pdfiumMutex);
    FPDF_PAGE fpage = PageCache::acquire(m_doc, page);
    if (!fpage) { if (outOverlayCapable) *outOverlayCapable = false; return result; }

    int count = FPDFPage_GetAnnotCount(fpage);
    bool capable = true;

    for (int i = 0; i < count; ++i) {
        FPDF_ANNOTATION annot = FPDFPage_GetAnnot(fpage, i);
        if (!annot) continue;

        int sub = FPDFAnnot_GetSubtype(annot);
        bool drawable = (sub == FPDF_ANNOT_INK || sub == FPDF_ANNOT_SQUARE ||
                         sub == FPDF_ANNOT_CIRCLE || sub == FPDF_ANNOT_HIGHLIGHT ||
                         sub == FPDF_ANNOT_LINE || sub == FPDF_ANNOT_POLYGON ||
                         sub == FPDF_ANNOT_FREETEXT || sub == FPDF_ANNOT_TEXT);
        if (!drawable && !(FPDFAnnot_GetFlags(annot) & FPDF_ANNOT_FLAG_HIDDEN) && sub != FPDF_ANNOT_POPUP) {
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
    QMutexLocker lock(&s_pdfiumMutex);
    FPDF_PAGE page = PageCache::acquire(m_doc, pageIndex);
    if (!page) { m_lastError = "Cannot load page"; return false; }
    AnnotInfo info;
    bool ok = createPopupNote_locked(page, pageIndex, pointDisp, text, author, &info);
    if (ok) {
        setOwnNoteObjectsActive(page, true);
        QElapsedTimer _gt; _gt.start();
        FPDFPage_GenerateContent(page);
        qDebug().noquote() << "[perf] genContent page=" << pageIndex << "ms=" << _gt.elapsed();
        // Trang da bi sua: bo entry cu (cac he khac khong dung handle cu) roi nap lai cho am.
        PageCache::invalidate(m_doc, pageIndex);
        PageCache::acquire(m_doc, pageIndex);
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
    QMutexLocker lock(&s_pdfiumMutex);
    FPDF_PAGE page = PageCache::acquire(m_doc, pageIndex);
    if (!page) { m_lastError = "Cannot load page"; return false; }
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
        PageCache::invalidate(m_doc, pageIndex);
        PageCache::acquire(m_doc, pageIndex);
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

    QMutexLocker lock(&s_pdfiumMutex);
    FPDF_PAGE page = PageCache::acquire(m_doc, pageIndex);
    if (!page) return false;

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
    PageCache::invalidate(m_doc, pageIndex);
    PageCache::acquire(m_doc, pageIndex);
    lock.unlock();
    bumpPageRevision(pageIndex);
    emit annotationAdded(pageIndex, info);
    qDebug().noquote() << "[perf] note rebuildTextNote total ms=" << _totalT.elapsed();
    return true;
}

bool AnnotationManager::moveAnnot(int pageIndex, int index, double dxU, double dyU) {
    if (!m_doc) return false;
    QElapsedTimer _totalT; _totalT.start();
    QMutexLocker lock(&s_pdfiumMutex);
    FPDF_PAGE page = PageCache::acquire(m_doc, pageIndex);
    if (!page) return false;
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
        PageCache::invalidate(m_doc, pageIndex);
        PageCache::acquire(m_doc, pageIndex);
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
        PageCache::invalidate(m_doc, pageIndex);
        PageCache::acquire(m_doc, pageIndex);
    }
    if (ok) bumpPageRevision(pageIndex);
    qDebug().noquote() << "[perf] note moveAnnot total ms=" << _totalT.elapsed();
    return ok;
}

int AnnotationManager::annotCount(int pageIndex) {
    if (!m_doc) return 0;
    QMutexLocker lock(&s_pdfiumMutex);
    FPDF_PAGE page = PageCache::acquire(m_doc, pageIndex);
    if (!page) return 0;
    int count = FPDFPage_GetAnnotCount(page);
    return count;
}

bool AnnotationManager::isOwnAnnot(int pageIndex, int index) {
    if (!m_doc) return false;
    QMutexLocker lock(&s_pdfiumMutex);
    FPDF_PAGE page = PageCache::acquire(m_doc, pageIndex);
    if (!page) return false;
    FPDF_ANNOTATION annot = FPDFPage_GetAnnot(page, index);
    if (!annot) return false;
    bool own = FPDFAnnot_HasKey(annot, "TRUID") || FPDFAnnot_HasKey(annot, "TRID");
    FPDFPage_CloseAnnot(annot);
    return own;
}

bool AnnotationManager::retextNote(int pageIndex, int index, const QString& newText) {
    if (!m_doc) return false;
    QElapsedTimer _totalT; _totalT.start();

    QMutexLocker lock(&s_pdfiumMutex);
    FPDF_PAGE page = PageCache::acquire(m_doc, pageIndex);
    if (!page) return false;

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
    PageCache::invalidate(m_doc, pageIndex);
    PageCache::acquire(m_doc, pageIndex);
    lock.unlock();
    bumpPageRevision(pageIndex);
    emit annotationAdded(pageIndex, info);
    qDebug().noquote() << "[perf] note retextNote total ms=" << _totalT.elapsed();
    return true;
}

bool AnnotationManager::removeAnnot(int pageIndex, int index) {
    if (!m_doc) return false;
    QMutexLocker lock(&s_pdfiumMutex);
    FPDF_PAGE page = PageCache::acquire(m_doc, pageIndex);
    if (!page) return false;
    bool needsGen = false;
    bool ok = removeAnnot_locked(page, index, &needsGen);
    if (ok && needsGen) {
        setOwnNoteObjectsActive(page, true);
        QElapsedTimer _gt; _gt.start();
        FPDFPage_GenerateContent(page);
        qDebug().noquote() << "[perf] genContent page=" << pageIndex << "ms=" << _gt.elapsed();
        invalidateNoteObjCache_locked(pageIndex);
    }
    if (ok) {
        PageCache::invalidate(m_doc, pageIndex);
        PageCache::acquire(m_doc, pageIndex);
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
    QMutexLocker lock(&s_pdfiumMutex);
    FPDF_PAGE page = PageCache::acquire(m_doc, pageIndex);
    if (!page) return s;
    FPDF_ANNOTATION annot = FPDFPage_GetAnnot(page, index);
    if (annot) {
        s.subtype = FPDFAnnot_GetSubtype(annot);
        FS_RECTF r{};
        if (FPDFAnnot_GetRect(annot, &r)) { s.rl = r.left; s.rt = r.top; s.rr = r.right; s.rb = r.bottom; }
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
    QMutexLocker lock(&s_pdfiumMutex);
    FPDF_PAGE page = PageCache::acquire(m_doc, pageIndex);
    if (!page) return false;
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
        FPDFPage_CloseAnnot(annot);
        // GenerateContent deferred to save time — annotations in /Annots are
        // preserved by FPDF_SaveAsCopy without explicit GenerateContent.
    }
    bool ok = (annot != nullptr);
    if (ok) {
        PageCache::invalidate(m_doc, pageIndex);
        PageCache::acquire(m_doc, pageIndex);
    }
    if (ok) bumpPageRevision(pageIndex);
    return ok;
}

bool AnnotationManager::getAnnotEditState(int pageIndex, int index,
                                          QString& outType, QColor& outColor,
                                          float& outWidth, float& outFontSize,
                                          bool* outHasFill, int* outFillAlpha) {
    if (!m_doc) return false;
    QMutexLocker lock(&s_pdfiumMutex);
    FPDF_PAGE page = PageCache::acquire(m_doc, pageIndex);
    if (!page) return false;
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

    QMutexLocker lock(&s_pdfiumMutex);
    FPDF_PAGE page = PageCache::acquire(m_doc, pageIndex);
    if (!page) { m_lastError = "Cannot load page"; return false; }

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
    PageCache::invalidate(m_doc, pageIndex);
    PageCache::acquire(m_doc, pageIndex);
    lock.unlock();

    bumpPageRevision(pageIndex);
    bool ok = saveDocument();
    return ok;
}



int AnnotationManager::findAnnotIndexByUid(int pageIndex, const QString& uid) {
    if (!m_doc || uid.isEmpty()) return -1;
    QMutexLocker lock(&s_pdfiumMutex);
    FPDF_PAGE page = PageCache::acquire(m_doc, pageIndex);
    if (!page) return -1;
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
    QMutexLocker lock(&s_pdfiumMutex);
    FPDF_PAGE page = PageCache::acquire(m_doc, pageIndex);
    if (!page) return -1;
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
    QMutexLocker lock(&s_pdfiumMutex);
    FPDF_PAGE page = PageCache::acquire(m_doc, pageIndex);
    if (!page) return {};
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
    PageCache::invalidate(m_doc, pageIndex);
    PageCache::acquire(m_doc, pageIndex);
    bumpPageRevision(pageIndex);
    return newUid;
}

bool AnnotationManager::setAnnotUid(int pageIndex, int index, const QString& uid) {
    if (!m_doc || uid.isEmpty()) return false;
    QMutexLocker lock(&s_pdfiumMutex);
    FPDF_PAGE page = PageCache::acquire(m_doc, pageIndex);
    if (!page) return false;
    FPDF_ANNOTATION annot = FPDFPage_GetAnnot(page, index);
    if (!annot) return false;
    FPDFAnnot_SetStringValue(annot, "TRUID", reinterpret_cast<FPDF_WIDESTRING>(uid.utf16()));
    FPDFPage_CloseAnnot(annot);
    PageCache::invalidate(m_doc, pageIndex);
    PageCache::acquire(m_doc, pageIndex);
    bumpPageRevision(pageIndex);
    return true;
}

bool AnnotationManager::setAnnotContents(int pageIndex, int index, const QString& text) {
    if (!m_doc) return false;
    QMutexLocker lock(&s_pdfiumMutex);
    FPDF_PAGE page = PageCache::acquire(m_doc, pageIndex);
    if (!page) return false;
    FPDF_ANNOTATION annot = FPDFPage_GetAnnot(page, index);
    if (!annot) return false;
    FPDFAnnot_SetStringValue(annot, "Contents", reinterpret_cast<FPDF_WIDESTRING>(text.utf16()));
    FPDFPage_CloseAnnot(annot);
    PageCache::invalidate(m_doc, pageIndex);
    PageCache::acquire(m_doc, pageIndex);
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

    QMutexLocker lock(&s_pdfiumMutex);
    FPDF_PAGE page = PageCache::acquire(m_doc, pageIndex);
    if (!page) { m_lastError = "Cannot load page"; return false; }

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
    PageCache::invalidate(m_doc, pageIndex);
    PageCache::acquire(m_doc, pageIndex);
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
    if (!font) font = FPDFText_LoadStandardFont(m_doc, "Helvetica");
    qDebug() << "[inote] font loaded=" << (font != nullptr);
    unsigned int noteId = m_nextNoteId++;

    if (font) {
        FPDF_PAGEOBJECT to = FPDFPageObj_CreateTextObj(m_doc, font, fontSize);
        qDebug() << "[inote] textObj created=" << (to != nullptr);
        if (to) {
            FPDFText_SetText(to, reinterpret_cast<FPDF_WIDESTRING>(text.utf16()));
            double a, b, c, d;
            float tx, ty;
            switch (rot) {
                case 1:
                    a = 0.0; b = 1.0; c = -1.0; d = 0.0;
                    tx = xu_min + fontSize + 2.0f; ty = yu_min + 4.0f;
                    break;
                case 2:
                    a = -1.0; b = 0.0; c = 0.0; d = -1.0;
                    tx = xu_max - 4.0f; ty = yu_max - 2.0f;
                    break;
                case 3:
                    a = 0.0; b = -1.0; c = 1.0; d = 0.0;
                    tx = xu_max - fontSize - 2.0f; ty = yu_max - 4.0f;
                    break;
                default:
                    a = 1.0; b = 0.0; c = 0.0; d = 1.0;
                    tx = xu_min + 4.0f; ty = yu_min + 2.0f;
                    break;
            }
            FPDFPageObj_Transform(to, a, b, c, d,
                                  static_cast<double>(tx),
                                  static_cast<double>(ty));
            qDebug() << "[inote] transform a=" << a << "b=" << b << "c=" << c << "d=" << d << "tx=" << tx << "ty=" << ty;
            unsigned int tr = textColor.red();
            unsigned int tg = textColor.green();
            unsigned int tb = textColor.blue();
            FPDFPageObj_SetFillColor(to, tr, tg, tb, 255);
            FPDF_PAGEOBJECTMARK mk = FPDFPageObj_AddMark(to, "TRNote");
            if (mk)
                FPDFPageObjMark_SetIntParam(m_doc, to, mk, "id", static_cast<int>(noteId));
            FPDFPage_InsertObject(page, to);
            qDebug() << "[inote] text page object inserted";
        }

        if (withBackground) {
            FPDF_PAGEOBJECT bg = FPDFPageObj_CreateNewRect(xu_min, yu_min, w, h);
            if (bg) {
                FPDFPageObj_SetFillColor(bg, 255, 255, 150, 200);
                FPDF_PAGEOBJECTMARK bgMk = FPDFPageObj_AddMark(bg, "TRNote");
                if (bgMk)
                    FPDFPageObjMark_SetIntParam(m_doc, bg, bgMk, "id", static_cast<int>(noteId));
                FPDFPage_InsertObject(page, bg);
            }
        }
    }

    {
        FPDF_ANNOTATION annot = FPDFPage_CreateAnnot(page, FPDF_ANNOT_FREETEXT);
        if (!annot) { m_lastError = "Cannot create annotation"; return false; }

        FS_RECTF rect{ xu_min, yu_min, xu_max, yu_max };
        FPDFAnnot_SetRect(annot, &rect);
        qDebug() << "[inote] annot created rect=" << rect.left << rect.bottom << rect.right << rect.top;

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

        FPDFAnnot_SetFlags(annot, FPDFAnnot_GetFlags(annot) | FPDF_ANNOT_FLAG_HIDDEN);
        FPDFPage_CloseAnnot(annot);

        int nAnnots = FPDFPage_GetAnnotCount(page);
        if (nAnnots > 0) {
            FPDF_ANNOTATION a2 = FPDFPage_GetAnnot(page, nAnnots - 1);
            if (a2) {
                float usable_w = w, usable_h = h;
                if (rot == 1 || rot == 3) { usable_w = h; usable_h = w; }
                float fs = fontSize;
                float textEst = text.length() * fs * 0.6f;
                if (textEst > usable_w - 8.0f) {
                    int textLen = text.length() < 1 ? 1 : static_cast<int>(text.length());
                    fs = (usable_w - 8.0f) / (textLen * 0.6f);
                    if (fs < 6.0f) fs = 6.0f;
                }
                QByteArray ap;
                ap.append("q\n");
                if (rot == 1)
                    ap.append(QString("0 -1 1 0 0 %1 cm\n").arg(h, 0, 'f', 2).toUtf8());
                else if (rot == 2)
                    ap.append(QString("-1 0 0 -1 %1 %2 cm\n").arg(w, 0, 'f', 2).arg(h, 0, 'f', 2).toUtf8());
                else if (rot == 3)
                    ap.append(QString("0 1 -1 0 %1 0 cm\n").arg(w, 0, 'f', 2).toUtf8());
                ap.append("BT\n");
                ap.append("/Helv " + QByteArray::number(fs, 'f', 1) + " Tf\n");
                ap.append(QByteArray::number(textColor.redF(), 'f', 3) + " ");
                ap.append(QByteArray::number(textColor.greenF(), 'f', 3) + " ");
                ap.append(QByteArray::number(textColor.blueF(), 'f', 3) + " rg\n");
                ap.append(QByteArray::number(4.0f, 'f', 1) + " ");
                ap.append(QByteArray::number(fs + 2.0f, 'f', 1) + " Td\n");
                ap.append("(" + pdfEscape(text) + ") Tj\n");
                ap.append("ET\n");
                ap.append("Q");

                QString apStr = QString::fromUtf8(ap);
                qDebug() << "[inote] apContent=" << ap.left(300);
                bool setOk = FPDFAnnot_SetAP(a2, FPDF_ANNOT_APPEARANCEMODE_NORMAL,
                    reinterpret_cast<FPDF_WIDESTRING>(apStr.utf16()));
                unsigned long apLenAfter = FPDFAnnot_GetAP(a2, FPDF_ANNOT_APPEARANCEMODE_NORMAL, nullptr, 0);
                qDebug() << "[inote] SetAP returned=" << (setOk ? "true" : "false") << "apLenAfter=" << apLenAfter;
                FPDFPage_CloseAnnot(a2);
            }
        }
    }

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
        QMutexLocker lock(&s_pdfiumMutex);
        page = PageCache::acquire(m_doc, pageIndex);
        if (!page) return QImage();
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
    QMutexLocker lock(&s_pdfiumMutex);
    FPDF_PAGE page = PageCache::acquire(m_doc, pageIndex);
    if (!page) return 0;
    int removed = removeNotePageObjects_locked(page, noteId);
    if (removed > 0) {
        setOwnNoteObjectsActive(page, true);
        QElapsedTimer _gt; _gt.start();
        FPDFPage_GenerateContent(page);
        qDebug().noquote() << "[perf] genContent page=" << pageIndex << "ms=" << _gt.elapsed();
        invalidateNoteObjCache_locked(pageIndex);
        PageCache::invalidate(m_doc, pageIndex);
        PageCache::acquire(m_doc, pageIndex);
    }
    if (removed > 0) bumpPageRevision(pageIndex);
    return removed;
}

void AnnotationManager::generateContentForPage(int page) {
    if (!m_doc) return;
    QMutexLocker lock(&s_pdfiumMutex);
    FPDF_PAGE fpage = PageCache::acquire(m_doc, page);
    if (fpage) {
        setOwnNoteObjectsActive(fpage, true);
        QElapsedTimer _gt; _gt.start();
        FPDFPage_GenerateContent(fpage);
        qDebug().noquote() << "[perf] genContent page=" << page << "ms=" << _gt.elapsed();
        PageCache::invalidate(m_doc, page);
        PageCache::acquire(m_doc, page);
        bumpPageRevision(page);
    }
}

void AnnotationManager::flushPendingGenerate(int page) {
    if (!m_pendingGenerate.contains(page)) return;
    if (!m_doc) { m_pendingGenerate.remove(page); return; }
    QElapsedTimer _gt;
    _gt.start();
    QMutexLocker lock(&s_pdfiumMutex);
    FPDF_PAGE fpage = PageCache::acquire(m_doc, page);
    if (fpage) {
        setOwnNoteObjectsActive(fpage, true);
        FPDFPage_GenerateContent(fpage);
        qDebug().noquote() << "[perf] genContent page=" << page << "ms=" << _gt.elapsed();
        PageCache::invalidate(m_doc, page);
        PageCache::acquire(m_doc, page);
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

    QMutexLocker lock(&s_pdfiumMutex);
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


