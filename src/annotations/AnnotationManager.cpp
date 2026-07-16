#include "AnnotationManager.h"
#include <fpdf_edit.h>
#include <fpdf_save.h>
#include <QMutex>
#include <QFile>
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

    if (!saveDocument()) return false;

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
                                          const QString& text, const QString& author) {
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

    // Default appearance: black text, 11pt Helvetica
    const char* da = "/Helv 11 Tf 0 g";
    QString daQ = QString::fromLatin1(da);
    FPDFAnnot_SetStringValue(annot, "DA",
        reinterpret_cast<FPDF_WIDESTRING>(daQ.utf16()));

    FPDFAnnot_SetStringValue(annot, "Contents",
        reinterpret_cast<FPDF_WIDESTRING>(text.utf16()));
    if (!author.isEmpty())
        FPDFAnnot_SetStringValue(annot, "T",
            reinterpret_cast<FPDF_WIDESTRING>(author.utf16()));

    FPDFAnnot_SetColor(annot, FPDFANNOT_COLORTYPE_Color, 255, 255, 150, 200);

    FPDFPage_CloseAnnot(annot);
    FPDFPage_GenerateContent(page);
    FPDF_ClosePage(page);
    lock.unlock();

    if (!saveDocument()) return false;

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
