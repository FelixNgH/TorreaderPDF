#include "AnnotationManager.h"
#include <fpdf_edit.h>
#include <fpdf_save.h>
#include <QMutex>
#include <QFile>
#include <QStringList>
#include <vector>
#include <cstring>

extern QMutex s_pdfiumMutex;

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

// ── AnnotationManager ─────────────────────────────────────────────────────────

AnnotationManager::AnnotationManager(QObject* parent) : QObject(parent) {}

void AnnotationManager::setDocument(FPDF_DOCUMENT doc, const QString& filePath) {
    m_doc  = doc;
    m_path = filePath;
}

QList<AnnotInfo> AnnotationManager::loadPage(int pageIndex) {
    QList<AnnotInfo> result;
    if (!m_doc) return result;

    QMutexLocker lock(&s_pdfiumMutex);
    FPDF_PAGE page = FPDF_LoadPage(m_doc, pageIndex);
    if (!page) return result;

    double pageH = FPDF_GetPageHeight(page);
    int count = FPDFPage_GetAnnotCount(page);

    for (int i = 0; i < count; ++i) {
        FPDF_ANNOTATION annot = FPDFPage_GetAnnot(page, i);
        if (!annot) continue;

        AnnotInfo info;
        info.pageIndex = pageIndex;
        info.type   = subtypeName(FPDFAnnot_GetSubtype(annot));
        info.text   = readAnnotString(annot, "Contents");
        info.author = readAnnotString(annot, "T");

        FS_RECTF r{};
        if (FPDFAnnot_GetRect(annot, &r))
            // Flip Y back to Qt convention (Y down)
            info.rect = QRectF(r.left, pageH - r.top, r.right - r.left, r.top - r.bottom);

        unsigned int cr = 0, cg = 0, cb = 0, ca = 255;
        FPDFAnnot_GetColor(annot, FPDFANNOT_COLORTYPE_Color, &cr, &cg, &cb, &ca);
        info.color = QColor(cr, cg, cb, ca);
        info.isDraft = FPDFAnnot_HasKey(annot, "TRSD") != 0;

        result.append(info);
        FPDFPage_CloseAnnot(annot);
    }

    FPDF_ClosePage(page);
    return result;
}

QList<AnnotInfo> AnnotationManager::loadAll(int pageCount) {
    QList<AnnotInfo> all;
    for (int i = 0; i < pageCount; ++i)
        all.append(loadPage(i));
    return all;
}

bool AnnotationManager::createPopupNote(int pageIndex, QPointF pointPdf,
                                         const QString& text, const QString& author) {
    if (!m_doc) return false;

    QMutexLocker lock(&s_pdfiumMutex);
    FPDF_PAGE page = FPDF_LoadPage(m_doc, pageIndex);
    if (!page) { m_lastError = "Cannot load page"; return false; }

    double pageH = FPDF_GetPageHeight(page);

    FPDF_ANNOTATION annot = FPDFPage_CreateAnnot(page, FPDF_ANNOT_TEXT);
    if (!annot) { FPDF_ClosePage(page); m_lastError = "Cannot create annotation"; return false; }

    // Sticky note icon: 100x100 pt square at the drop point (5x larger for visibility)
    FS_RECTF rect{
        static_cast<float>(pointPdf.x()),
        static_cast<float>(pageH - pointPdf.y()),
        static_cast<float>(pointPdf.x() + 100),
        static_cast<float>(pageH - pointPdf.y() + 100)
    };
    FPDFAnnot_SetRect(annot, &rect);

    // Color: yellow sticky note
    FPDFAnnot_SetColor(annot, FPDFANNOT_COLORTYPE_Color, 255, 220, 0, 220);

    FPDFAnnot_SetStringValue(annot, "Contents",
        reinterpret_cast<FPDF_WIDESTRING>(text.utf16()));
    if (!author.isEmpty())
        FPDFAnnot_SetStringValue(annot, "T",
            reinterpret_cast<FPDF_WIDESTRING>(author.utf16()));

    FPDFPage_CloseAnnot(annot);
    FPDFPage_GenerateContent(page);
    FPDF_ClosePage(page);
    lock.unlock();

    AnnotInfo info;
    info.pageIndex = pageIndex;
    info.type   = "Note";
    info.text   = text;
    info.author = author;
    info.rect   = QRectF(pointPdf, QSizeF(100, 100));
    info.color  = QColor(255, 220, 0, 220);
    emit annotationAdded(pageIndex, info);
    return true;
}

bool AnnotationManager::createInlineNote(int pageIndex, QRectF rectPdf,
                                          const QString& text, const QString& author,
                                          bool withBackground, QColor textColor) {
    if (!m_doc) return false;

    QMutexLocker lock(&s_pdfiumMutex);
    FPDF_PAGE page = FPDF_LoadPage(m_doc, pageIndex);
    if (!page) { m_lastError = "Cannot load page"; return false; }

    double pageH = FPDF_GetPageHeight(page);

    FPDF_ANNOTATION annot = FPDFPage_CreateAnnot(page, FPDF_ANNOT_FREETEXT);
    if (!annot) { FPDF_ClosePage(page); m_lastError = "Cannot create annotation"; return false; }

    FS_RECTF rect{
        static_cast<float>(rectPdf.left()),
        static_cast<float>(pageH - rectPdf.bottom()),
        static_cast<float>(rectPdf.right()),
        static_cast<float>(pageH - rectPdf.top())
    };
    FPDFAnnot_SetRect(annot, &rect);

    // Default appearance: 11pt Helvetica, text color from style
    QString daQ = QString("/Helv 11 Tf %1 %2 %3 rg")
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

    FPDFPage_CloseAnnot(annot);
    FPDFPage_GenerateContent(page);
    FPDF_ClosePage(page);
    lock.unlock();

    AnnotInfo info;
    info.pageIndex = pageIndex;
    info.type   = "FreeText";
    info.text   = text;
    info.author = author;
    info.rect   = rectPdf;
    info.color  = QColor(255, 255, 150, 200);
    emit annotationAdded(pageIndex, info);
    return true;
}

bool AnnotationManager::removeAnnot(int pageIndex, int index) {
    if (!m_doc) return false;
    QMutexLocker lock(&s_pdfiumMutex);
    FPDF_PAGE page = FPDF_LoadPage(m_doc, pageIndex);
    if (!page) return false;
    bool ok = FPDFPage_RemoveAnnot(page, index);
    if (ok) FPDFPage_GenerateContent(page);
    FPDF_ClosePage(page);
    lock.unlock();
    return ok;
}

bool AnnotationManager::setAnnotStyle(int pageIndex, int index, QColor color, float width, bool fill) {
    AnnotSnapshot s = snapshotAnnot(pageIndex, index);
    if (!s.valid) return false;
    s.hasColor = true;
    s.r = static_cast<unsigned int>(color.red());
    s.g = static_cast<unsigned int>(color.green());
    s.b = static_cast<unsigned int>(color.blue());
    s.a = static_cast<unsigned int>(color.alpha());
    s.border = width;
    if (fill) {
        s.hasFill = true;
        s.fr = s.r; s.fg = s.g; s.fb = s.b; s.fa = s.a;
    } else {
        s.hasFill = false;
    }
    // FreeText text color lives in the DA string, so update it too.
    if (s.subtype == FPDF_ANNOT_FREETEXT) {
        s.da = "/Helv 11 Tf " + QString::number(color.redF(), 'f', 3) + " "
             + QString::number(color.greenF(), 'f', 3) + " "
             + QString::number(color.blueF(), 'f', 3) + " rg";
    }
    if (!removeAnnot(pageIndex, index)) return false;
    return addSnapshot(pageIndex, s);
}

AnnotSnapshot AnnotationManager::snapshotAnnot(int pageIndex, int index) {
    AnnotSnapshot s;
    if (!m_doc) return s;
    QMutexLocker lock(&s_pdfiumMutex);
    FPDF_PAGE page = FPDF_LoadPage(m_doc, pageIndex);
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
    FPDF_ClosePage(page);
    return s;
}

bool AnnotationManager::addSnapshot(int pageIndex, const AnnotSnapshot& s) {
    if (!m_doc || !s.valid) return false;
    QMutexLocker lock(&s_pdfiumMutex);
    FPDF_PAGE page = FPDF_LoadPage(m_doc, pageIndex);
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
        if (s.isDraft)
            FPDFAnnot_SetStringValue(annot, "TRSD", reinterpret_cast<FPDF_WIDESTRING>(QStringLiteral("1").utf16()));
        for (const auto& stroke : s.ink) {
            std::vector<FS_POINTF> pts;
            for (const auto& p : stroke) pts.push_back(FS_POINTF{ static_cast<float>(p.x()), static_cast<float>(p.y()) });
            if (pts.size() >= 2) FPDFAnnot_AddInkStroke(annot, pts.data(), pts.size());
        }
        FPDFPage_CloseAnnot(annot);
        FPDFPage_GenerateContent(page);
    }
    bool ok = (annot != nullptr);
    FPDF_ClosePage(page);
    return ok;
}

bool AnnotationManager::updateNote(int pageIndex, int annotIndex, const QString& newText) {
    if (!m_doc) { m_lastError = "No document"; return false; }

    QMutexLocker lock(&s_pdfiumMutex);
    FPDF_PAGE page = FPDF_LoadPage(m_doc, pageIndex);
    if (!page) { m_lastError = "Cannot load page"; return false; }

    FPDF_ANNOTATION annot = FPDFPage_GetAnnot(page, annotIndex);
    if (!annot) {
        FPDF_ClosePage(page);
        m_lastError = "Annotation not found";
        return false;
    }

    FPDFAnnot_SetStringValue(annot, "Contents",
        reinterpret_cast<FPDF_WIDESTRING>(newText.utf16()));

    FPDFPage_CloseAnnot(annot);
    FPDFPage_GenerateContent(page);
    FPDF_ClosePage(page);
    lock.unlock();

    bool ok = saveDocument();
    return ok;
}

bool AnnotationManager::createSignatureDraft(int pageIndex, QRectF rectPt, const QString& text) {
    if (!m_doc) return false;

    QMutexLocker lock(&s_pdfiumMutex);
    FPDF_PAGE page = FPDF_LoadPage(m_doc, pageIndex);
    if (!page) { m_lastError = "Cannot load page"; return false; }

    FPDF_ANNOTATION annot = FPDFPage_CreateAnnot(page, FPDF_ANNOT_FREETEXT);
    if (!annot) { FPDF_ClosePage(page); m_lastError = "Cannot create annotation"; return false; }

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

    FPDFPage_CloseAnnot(annot);
    FPDFPage_GenerateContent(page);
    FPDF_ClosePage(page);
    lock.unlock();

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
    bool ok = FPDF_SaveAsCopy(m_doc, &fw.base, FPDF_NO_INCREMENTAL) != 0;
    lock.unlock();

    file.close();
    if (!ok) m_lastError = "FPDF_SaveAsCopy failed";
    return ok;
}
