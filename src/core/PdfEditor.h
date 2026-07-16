#pragma once
#include <QObject>
#include <QString>
#include <QStringList>
#include <QList>
#include <QMap>

class QSaveFile;

struct fpdf_document_t__;
using FPDF_DOCUMENT = fpdf_document_t__*;
struct fpdf_bookmark_t__;
using FPDF_BOOKMARK = fpdf_bookmark_t__*;
struct FPDF_FILEWRITE_;

struct OutlineNode {
    QString title;
    int pageIndex = -1;
    QList<OutlineNode> children;
};

// PDFium-backed PDF manipulation. All ops synchronous; wrap in QThread for async UI.
class PdfEditor : public QObject {
    Q_OBJECT
public:
    explicit PdfEditor(QObject* parent = nullptr);
    ~PdfEditor() override;

    // Merge list of PDF files into outputPath
    bool merge(const QStringList& inputFiles, const QString& outputPath);

    // Extract pages [firstPage, lastPage] (0-indexed) from inputFile
    bool extractPages(const QString& inputFile, int firstPage, int lastPage,
                      const QString& outputPath);

    // Extract arbitrary pages (0-indexed) in given order; copies matching bookmarks.
    bool extractPageList(const QString& inputFile, const QList<int>& pageIndices,
                         const QString& outputPath);

    // Split inputFile into chunks of chunkSize pages
    bool splitBySize(const QString& inputFile, int chunkSize,
                     const QString& outputDir);

    // Split inputFile into one single-page PDF per page in outputDir.
    // baseNames[i] = sanitized filename stem (no .pdf) for page i.
    // Returns number of files written, or -1 on error (see lastError()).
    int extractAllPages(const QString& inputFile, const QStringList& baseNames,
                        const QString& outputDir);

    // Rotate pages (angle: 0, 90, 180, 270)
    bool rotatePages(const QString& inputFile, const QList<int>& pageIndices,
                     int angle, const QString& outputPath);

    // Delete pages from PDF (0-indexed)
    bool deletePages(const QString& inputFile, const QList<int>& pageIndices,
                     const QString& outputPath);

    // Reorder pages: newOrder[i] = original page index for result page i
    bool reorderPages(const QString& inputFile, const QList<int>& newOrder,
                      const QString& outputPath);

    // Reorder top-level bookmarks: newOrder[i] = original 0-based outline index
    // Preserves each item's subtree and destination intact.
    bool reorderBookmarks(const QString& inputFile, const QList<int>& newOrder,
                          const QString& outputPath);

    // Insert one page from sourceFile (sourcePageIndex) into targetFile,
    // before targetInsertBefore (use targetFile.pageCount() to append).
    // Writes result to outputPath (safe to equal targetFile if doc is closed first).
    bool insertPageFrom(const QString& targetFile, int targetInsertBefore,
                        const QString& sourceFile, int sourcePageIndex,
                        const QString& outputPath);

    // Insert ALL pages of sourceFile into targetFile before page `insertBefore`
    // (use targetFile.pageCount() to append). Adobe-style "Insert Pages from File".
    bool insertPdf(const QString& targetFile, int insertBefore,
                   const QString& sourceFile, const QString& outputPath);

    QString lastError() const { return m_lastError; }

signals:
    void progress(int percent);

private:
    QString m_lastError;
    bool saveDoc(FPDF_DOCUMENT doc, const QString& path);

    static OutlineNode readOutlineNode(FPDF_DOCUMENT doc, FPDF_BOOKMARK bm, int offset);
    static QList<OutlineNode> readOutlineTree(FPDF_DOCUMENT doc, int offset);
    static void writeOutlineTree(const QString& path, const QList<OutlineNode>& tree);
    static QList<OutlineNode> filterOutlineTree(const QList<OutlineNode>& tree,
                                                  const QMap<int,int>& oldToNew);
};
