#include "PdfEditor.h"
#include "PdfDocument.h"

#include <fpdfview.h>
#include <fpdf_ppo.h>
#include <fpdf_save.h>
#include <fpdf_edit.h>
#include <fpdf_doc.h>

#include <qpdf/QPDF.hh>
#include <qpdf/QPDFWriter.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFNameTreeObjectHelper.hh>

#include <QFile>
#include <QSaveFile>
#include <QFileInfo>
#include <QDir>
#include <QMutexLocker>
#include <QSet>
#include <QRegularExpression>
#include <numeric>
#include <map>
#include <functional>

// ══════════════════════════════════════════════════════════════════════════════
// PDFium helpers
// ══════════════════════════════════════════════════════════════════════════════

static int getBlockCallback(void* param, unsigned long position,
                            unsigned char* pBuf, unsigned long size) {
    auto* f = static_cast<QFile*>(param);
    f->seek(position);
    return f->read(reinterpret_cast<char*>(pBuf), size) == static_cast<qint64>(size) ? 1 : 0;
}

struct FileCtx {
    QFile file;
    FPDF_FILEACCESS fa = {};
    explicit FileCtx(const QString& p) : file(p) {}
};

struct LoadedDoc {
    FPDF_DOCUMENT doc = nullptr;
    FileCtx* ctx = nullptr;

    LoadedDoc() = default;
    ~LoadedDoc() { close(); }
    LoadedDoc(LoadedDoc&& o) noexcept
        : doc(o.doc), ctx(o.ctx) {
        o.doc = nullptr; o.ctx = nullptr;
    }
    LoadedDoc(const LoadedDoc&) = delete;
    LoadedDoc& operator=(const LoadedDoc&) = delete;

    void close() {
        if (doc) { FPDF_CloseDocument(doc); doc = nullptr; }
        delete ctx; ctx = nullptr;
    }
};

static LoadedDoc loadPdf(const QString& path, QString& errorOut) {
    LoadedDoc ld;
    ld.ctx = new FileCtx(path);
    if (!ld.ctx->file.open(QIODevice::ReadOnly)) {
        errorOut = QString("Cannot open file: %1").arg(path);
        delete ld.ctx; ld.ctx = nullptr;
        return ld;
    }
    ld.ctx->fa.m_FileLen = static_cast<unsigned long>(ld.ctx->file.size());
    ld.ctx->fa.m_GetBlock = getBlockCallback;
    ld.ctx->fa.m_Param = &ld.ctx->file;

    ld.doc = FPDF_LoadCustomDocument(&ld.ctx->fa, nullptr);
    if (!ld.doc) {
        unsigned long err = FPDF_GetLastError();
        if (err == FPDF_ERR_PASSWORD)
            errorOut = QString("File có mật khẩu, chưa hỗ trợ: %1").arg(QFileInfo(path).fileName());
        else
            errorOut = QString("Failed to load PDF: %1 (error %2)").arg(path).arg(err);
        delete ld.ctx; ld.ctx = nullptr;
    }
    return ld;
}

// FPDF_FILEWRITE wrapper with QSaveFile* embedded
struct SaveWriter {
    FPDF_FILEWRITE fileWrite;
    QSaveFile* saveFile;
    static int writeBlock(FPDF_FILEWRITE* self, const void* data, unsigned long size) {
        auto* w = reinterpret_cast<SaveWriter*>(self);
        return w->saveFile->write(reinterpret_cast<const char*>(data), size) == static_cast<qint64>(size) ? 1 : 0;
    }
};

bool PdfEditor::saveDoc(FPDF_DOCUMENT doc, const QString& path) {
    QSaveFile sf(path);
    if (!sf.open(QIODevice::WriteOnly)) {
        m_lastError = QString("Cannot write: %1").arg(path);
        return false;
    }
    SaveWriter sw;
    sw.fileWrite.version = 1;
    sw.fileWrite.WriteBlock = SaveWriter::writeBlock;
    sw.saveFile = &sf;

    if (!FPDF_SaveAsCopy(doc, &sw.fileWrite, FPDF_NO_INCREMENTAL)) {
        m_lastError = "Failed to save PDF";
        sf.cancelWriting();
        return false;
    }
    if (!sf.commit()) {
        m_lastError = "Failed to commit save file";
        sf.cancelWriting();
        return false;
    }
    return true;
}

// ══════════════════════════════════════════════════════════════════════════════
// Outline helpers
// ══════════════════════════════════════════════════════════════════════════════

static QString bookmarkTitle(FPDF_BOOKMARK bm) {
    unsigned long len = FPDFBookmark_GetTitle(bm, nullptr, 0);
    if (len == 0) return {};
    int wc = static_cast<int>(len / sizeof(unsigned short));
    QVector<unsigned short> buf(wc);
    FPDFBookmark_GetTitle(bm, buf.data(), buf.size() * sizeof(unsigned short));
    return QString::fromUtf16(reinterpret_cast<const char16_t*>(buf.data()));
}

static int bookmarkPage(FPDF_DOCUMENT doc, FPDF_BOOKMARK bm) {
    FPDF_DEST dest = FPDFBookmark_GetDest(doc, bm);
    if (!dest) {
        FPDF_ACTION action = FPDFBookmark_GetAction(bm);
        if (action && FPDFAction_GetType(action) == PDFACTION_GOTO)
            dest = FPDFAction_GetDest(doc, action);
    }
    return dest ? FPDFDest_GetDestPageIndex(doc, dest) : -1;
}

static OutlineNode readOutlineNodeImpl(FPDF_DOCUMENT doc, FPDF_BOOKMARK bm,
                                        int offset, QSet<quintptr>& seen, int depth) {
    if (!bm || depth >= 64) return {};
    quintptr ptr = reinterpret_cast<quintptr>(bm);
    if (seen.contains(ptr)) return {};
    seen.insert(ptr);

    OutlineNode node;
    node.title = bookmarkTitle(bm);
    int pg = bookmarkPage(doc, bm);
    node.pageIndex = (pg >= 0) ? pg + offset : -1;

    FPDF_BOOKMARK child = FPDFBookmark_GetFirstChild(doc, bm);
    while (child) {
        node.children.append(readOutlineNodeImpl(doc, child, offset, seen, depth + 1));
        child = FPDFBookmark_GetNextSibling(doc, child);
    }
    return node;
}

OutlineNode PdfEditor::readOutlineNode(FPDF_DOCUMENT doc, FPDF_BOOKMARK bm, int offset) {
    QSet<quintptr> seen;
    return readOutlineNodeImpl(doc, bm, offset, seen, 0);
}

QList<OutlineNode> PdfEditor::readOutlineTree(FPDF_DOCUMENT doc, int offset) {
    QList<OutlineNode> tree;
    QSet<quintptr> seen;
    FPDF_BOOKMARK bm = FPDFBookmark_GetFirstChild(doc, nullptr);
    while (bm) {
        tree.append(readOutlineNodeImpl(doc, bm, offset, seen, 0));
        bm = FPDFBookmark_GetNextSibling(doc, bm);
    }
    return tree;
}

QList<OutlineNode> PdfEditor::filterOutlineTree(const QList<OutlineNode>& tree,
                                                  const QMap<int,int>& oldToNew) {
    QList<OutlineNode> result;
    for (const auto& n : tree) {
        OutlineNode fn;
        fn.title = n.title;
        fn.pageIndex = (n.pageIndex >= 0) ? oldToNew.value(n.pageIndex, -1) : -1;
        fn.children = filterOutlineTree(n.children, oldToNew);
        if (fn.pageIndex >= 0 || !fn.children.isEmpty())
            result.append(fn);
    }
    return result;
}

static int totalVisibleCount(const QList<OutlineNode>& nodes) {
    int count = 0;
    for (const auto& node : nodes) {
        count += 1;
        if (!node.children.isEmpty())
            count += totalVisibleCount(node.children);
    }
    return count;
}

void PdfEditor::writeOutlineTree(const QString& path, const QList<OutlineNode>& tree) {
    QString tmpPath = path + ".mergetmp";
    try {
        {
            QPDF pdf;
            pdf.processFile(path.toUtf8().constData());
            auto pages = QPDFPageDocumentHelper(pdf).getAllPages();
            if (pages.empty()) return;

            if (!tree.isEmpty()) {
                QPDFObjectHandle outlines = pdf.makeIndirectObject(QPDFObjectHandle::newDictionary());
                outlines.replaceKey("/Type", QPDFObjectHandle::newName("/Outlines"));

                std::function<QPDFObjectHandle(const OutlineNode&, QPDFObjectHandle)> buildItem;
                buildItem = [&](const OutlineNode& node, QPDFObjectHandle parent) -> QPDFObjectHandle {
                    QPDFObjectHandle item = pdf.makeIndirectObject(QPDFObjectHandle::newDictionary());
                    item.replaceKey("/Title", QPDFObjectHandle::newUnicodeString(node.title.toStdString()));
                    item.replaceKey("/Parent", parent);

                    if (node.pageIndex >= 0 && node.pageIndex < static_cast<int>(pages.size())) {
                        QPDFObjectHandle dest = QPDFObjectHandle::newArray();
                        dest.appendItem(pages[node.pageIndex].getObjectHandle());
                        dest.appendItem(QPDFObjectHandle::newName("/XYZ"));
                        dest.appendItem(QPDFObjectHandle::newNull());
                        dest.appendItem(QPDFObjectHandle::newNull());
                        dest.appendItem(QPDFObjectHandle::newNull());
                        item.replaceKey("/Dest", dest);
                    }

                    if (!node.children.isEmpty()) {
                        QPDFObjectHandle first, last;
                        for (int i = 0; i < node.children.size(); ++i) {
                            QPDFObjectHandle child = buildItem(node.children[i], item);
                            if (i == 0) first = child;
                            else last.replaceKey("/Next", child);
                            if (i > 0) child.replaceKey("/Prev", last);
                            last = child;
                        }
                        item.replaceKey("/First", first);
                        item.replaceKey("/Last", last);
                        item.replaceKey("/Count", QPDFObjectHandle::newInteger(totalVisibleCount(node.children)));
                    }
                    return item;
                };

                QPDFObjectHandle firstTop, lastTop;
                for (int i = 0; i < tree.size(); ++i) {
                    QPDFObjectHandle item = buildItem(tree[i], outlines);
                    if (i == 0) firstTop = item;
                    else lastTop.replaceKey("/Next", item);
                    if (i > 0) item.replaceKey("/Prev", lastTop);
                    lastTop = item;
                }

                if (firstTop.isInitialized()) {
                    outlines.replaceKey("/First", firstTop);
                    outlines.replaceKey("/Last", lastTop);
                    outlines.replaceKey("/Count", QPDFObjectHandle::newInteger(totalVisibleCount(tree)));
                    pdf.getRoot().replaceKey("/Outlines", outlines);
                }
            }

            QPDFWriter writer(pdf, tmpPath.toUtf8().constData());
            writer.setObjectStreamMode(qpdf_o_generate);
            writer.setStreamDataMode(qpdf_s_compress);
            writer.setCompressStreams(true);
            writer.write();
        }

        QFile::remove(path);
        if (!QFile::rename(tmpPath, path)) {
            QFile::remove(tmpPath);
            qWarning() << "[PdfEditor] Failed to replace" << path;
        }
    } catch (const std::exception& e) {
        QFile::remove(tmpPath);
        qWarning() << "[PdfEditor] Outline post-pass failed:" << e.what();
    }
}

// Xoá tiêu đề của các bookmark "Page N" tự-sinh (đệ quy) để buildPagedOutline
// đánh số lại từ đầu; bookmark có tên THẬT (chương sách…) được giữ nguyên.
static void stripAutoPageTitles(QList<OutlineNode>& nodes) {
    static const QRegularExpression re(QStringLiteral("^\\s*Page\\s+\\d+\\s*$"),
                                       QRegularExpression::CaseInsensitiveOption);
    for (auto& n : nodes) {
        if (re.match(n.title).hasMatch())
            n.title.clear();
        stripAutoPageTitles(n.children);
    }
}

// ══════════════════════════════════════════════════════════════════════════════
// Merged outline builder (quy tắc user): giữ nguyên bookmark gốc của mỗi PDF con
// theo thứ tự merge (đã remap trang); trang nào KHÔNG có bookmark thì thêm mục
// "Trang N" (N = số thứ tự trang 1-based trong file merge). Danh sách phẳng, theo trang.
// ══════════════════════════════════════════════════════════════════════════════

static void flattenOutline(const QList<OutlineNode>& tree,
                           QList<QPair<int, QString>>& out) {
    for (const auto& n : tree) {
        if (n.pageIndex >= 0 && !n.title.isEmpty())
            out.append(qMakePair(n.pageIndex, n.title));
        flattenOutline(n.children, out);
    }
}

static QList<OutlineNode> buildPagedOutline(const QList<OutlineNode>& mergedTree,
                                            int totalPages) {
    QList<QPair<int, QString>> flat;
    flattenOutline(mergedTree, flat);
    QMap<int, QStringList> byPage;
    for (const auto& pr : flat) byPage[pr.first].append(pr.second);

    QList<OutlineNode> result;
    for (int pg = 0; pg < totalPages; ++pg) {
        if (byPage.contains(pg)) {
            for (const QString& title : byPage.value(pg)) {
                OutlineNode n; n.title = title; n.pageIndex = pg;
                result.append(n);
            }
        } else {
            OutlineNode n;
            n.title = QString("Page %1").arg(pg + 1);
            n.pageIndex = pg;
            result.append(n);
        }
    }
    return result;
}

// ══════════════════════════════════════════════════════════════════════════════
// PdfEditor
// ══════════════════════════════════════════════════════════════════════════════

PdfEditor::PdfEditor(QObject* parent) : QObject(parent) {
    PdfDocument::libAddRef();
}

// Destructor
PdfEditor::~PdfEditor() {
    PdfDocument::libRelease();
}

// ── merge ──────────────────────────────────────────────────────────────────────
bool PdfEditor::merge(const QStringList& inputFiles, const QString& outputPath) {
    QMutexLocker lock(&PdfDocument::pdfiumGlobalMutex());

    FPDF_DOCUMENT output = FPDF_CreateNewDocument();
    if (!output) { m_lastError = "Failed to create output document"; return false; }

    int totalPages = 0;
    bool firstFile = true;
    QList<OutlineNode> mergedTree;

    for (int fi = 0; fi < inputFiles.size(); ++fi) {
        const QString& f = inputFiles[fi];
        LoadedDoc ld = loadPdf(f, m_lastError);
        if (!ld.doc) { FPDF_CloseDocument(output); return false; }

        if (firstFile) {
            FPDF_CopyViewerPreferences(output, ld.doc);
            firstFile = false;
        }

        int n = FPDF_GetPageCount(ld.doc);
        if (!FPDF_ImportPagesByIndex(output, ld.doc, nullptr, 0, totalPages)) {
            m_lastError = QString("Failed to import pages from: %1").arg(QFileInfo(f).fileName());
            ld.close();
            FPDF_CloseDocument(output);
            return false;
        }

        // Giữ nguyên bookmark gốc của file này, remap trang theo offset merge.
        mergedTree.append(readOutlineTree(ld.doc, totalPages));
        totalPages += n;
        ld.close();
        emit progress((fi + 1) * 100 / inputFiles.size());
    }

    bool ok = saveDoc(output, outputPath);
    FPDF_CloseDocument(output);
    if (ok)
        writeOutlineTree(outputPath, buildPagedOutline(mergedTree, totalPages));
    return ok;
}

// ── extractPages ───────────────────────────────────────────────────────────────
bool PdfEditor::extractPages(const QString& inputFile, int firstPage, int lastPage,
                              const QString& outputPath) {
    QMutexLocker lock(&PdfDocument::pdfiumGlobalMutex());
    LoadedDoc ld = loadPdf(inputFile, m_lastError);
    if (!ld.doc) return false;

    int total = FPDF_GetPageCount(ld.doc);
    if (firstPage < 0) firstPage = 0;
    if (lastPage >= total) lastPage = total - 1;
    if (firstPage > lastPage) { m_lastError = "Invalid page range"; ld.close(); return false; }

    FPDF_DOCUMENT output = FPDF_CreateNewDocument();
    if (!output) { m_lastError = "Failed to create output document"; ld.close(); return false; }

    QVector<int> indices(lastPage - firstPage + 1);
    std::iota(indices.begin(), indices.end(), firstPage);
    if (!FPDF_ImportPagesByIndex(output, ld.doc, indices.data(), indices.size(), 0)) {
        m_lastError = "Failed to import pages";
        FPDF_CloseDocument(output); ld.close();
        return false;
    }

    bool ok = saveDoc(output, outputPath);
    FPDF_CloseDocument(output);
    ld.close();
    return ok;
}

// ── extractPageList ────────────────────────────────────────────────────────────
bool PdfEditor::extractPageList(const QString& inputFile, const QList<int>& pageIndices,
                                 const QString& outputPath) {
    if (pageIndices.isEmpty()) { m_lastError = "No pages selected"; return false; }
    QMutexLocker lock(&PdfDocument::pdfiumGlobalMutex());
    LoadedDoc ld = loadPdf(inputFile, m_lastError);
    if (!ld.doc) return false;

    int total = FPDF_GetPageCount(ld.doc);
    QVector<int> valid;
    QMap<int,int> oldToNew;
    for (int i = 0; i < pageIndices.size(); ++i) {
        int idx = pageIndices[i];
        if (idx >= 0 && idx < total) {
            valid.append(idx);
            oldToNew[idx] = valid.size() - 1;
        }
    }
    if (valid.isEmpty()) { m_lastError = "No valid page indices"; ld.close(); return false; }

    FPDF_DOCUMENT output = FPDF_CreateNewDocument();
    if (!output) { m_lastError = "Failed to create output document"; ld.close(); return false; }

    if (!FPDF_ImportPagesByIndex(output, ld.doc, valid.data(), valid.size(), 0)) {
        m_lastError = "Failed to import pages";
        FPDF_CloseDocument(output); ld.close();
        return false;
    }

    bool ok = saveDoc(output, outputPath);
    FPDF_CloseDocument(output);

    if (ok) {
        auto tree = readOutlineTree(ld.doc, 0);
        auto filtered = filterOutlineTree(tree, oldToNew);
        lock.unlock();
        writeOutlineTree(outputPath, filtered);
        lock.relock();
    }

    ld.close();
    return ok;
}

// ── splitBySize ────────────────────────────────────────────────────────────────
bool PdfEditor::splitBySize(const QString& inputFile, int chunkSize,
                             const QString& outputDir) {
    QMutexLocker lock(&PdfDocument::pdfiumGlobalMutex());
    LoadedDoc ld = loadPdf(inputFile, m_lastError);
    if (!ld.doc) return false;

    int total = FPDF_GetPageCount(ld.doc);
    if (chunkSize <= 0) chunkSize = 1;
    int part = 0;

    for (int start = 0; start < total; start += chunkSize) {
        int end = qMin(start + chunkSize, total);
        FPDF_DOCUMENT out = FPDF_CreateNewDocument();
        if (!out) { m_lastError = "Failed to create output document"; ld.close(); return false; }

        QVector<int> indices(end - start);
        std::iota(indices.begin(), indices.end(), start);
        if (!FPDF_ImportPagesByIndex(out, ld.doc, indices.data(), indices.size(), 0)) {
            m_lastError = "Failed to import chunk pages";
            FPDF_CloseDocument(out); ld.close();
            return false;
        }

        QString outPath = QDir(outputDir).filePath(
            QFileInfo(inputFile).baseName() + QString("_part%1.pdf").arg(++part));
        if (!saveDoc(out, outPath)) { FPDF_CloseDocument(out); ld.close(); return false; }
        FPDF_CloseDocument(out);
        emit progress(end * 100 / total);
    }

    ld.close();
    return true;
}

// ── extractAllPages ────────────────────────────────────────────────────────────
int PdfEditor::extractAllPages(const QString& inputFile, const QStringList& baseNames,
                                const QString& outputDir) {
    QMutexLocker lock(&PdfDocument::pdfiumGlobalMutex());
    LoadedDoc ld = loadPdf(inputFile, m_lastError);
    if (!ld.doc) return -1;

    int total = FPDF_GetPageCount(ld.doc);
    QDir dir(outputDir);
    QSet<QString> used;

    for (int i = 0; i < total; ++i) {
        QString stem = (i < baseNames.size() && !baseNames[i].trimmed().isEmpty())
                           ? baseNames[i].trimmed()
                           : QString("page_%1").arg(i + 1);
        QString unique = stem;
        int dup = 2;
        while (used.contains(unique.toLower()))
            unique = stem + QString(" (%1)").arg(dup++);
        used.insert(unique.toLower());

        FPDF_DOCUMENT out = FPDF_CreateNewDocument();
        if (!out) { m_lastError = "Failed to create output document"; ld.close(); return -1; }

        int idx = i;
        if (!FPDF_ImportPagesByIndex(out, ld.doc, &idx, 1, 0)) {
            m_lastError = QString("Failed to extract page %1").arg(i);
            FPDF_CloseDocument(out); ld.close();
            return -1;
        }

        QString outPath = dir.filePath(unique + ".pdf");
        if (!saveDoc(out, outPath)) { FPDF_CloseDocument(out); ld.close(); return -1; }
        FPDF_CloseDocument(out);
        emit progress((i + 1) * 100 / total);
    }

    ld.close();
    return total;
}

// ── rotatePages ────────────────────────────────────────────────────────────────
bool PdfEditor::rotatePages(const QString& inputFile, const QList<int>& pageIndices,
                             int angle, const QString& outputPath) {
    QMutexLocker lock(&PdfDocument::pdfiumGlobalMutex());
    LoadedDoc ld = loadPdf(inputFile, m_lastError);
    if (!ld.doc) return false;

    int add = angle / 90; // 0, 1, 2, 3 or -1, -2, -3
    for (int idx : pageIndices) {
        if (idx < 0) continue;
        FPDF_PAGE page = FPDF_LoadPage(ld.doc, idx);
        if (!page) continue;
        int rot = FPDFPage_GetRotation(page);
        rot = ((rot + add) % 4 + 4) % 4;
        FPDFPage_SetRotation(page, rot);
        FPDF_ClosePage(page);
    }

    bool ok = saveDoc(ld.doc, outputPath);
    ld.close();
    return ok;
}

// ── deletePages ────────────────────────────────────────────────────────────────
bool PdfEditor::deletePages(const QString& inputFile, const QList<int>& pageIndices,
                             const QString& outputPath) {
    QMutexLocker lock(&PdfDocument::pdfiumGlobalMutex());
    LoadedDoc ld = loadPdf(inputFile, m_lastError);
    if (!ld.doc) return false;

    QList<int> sorted = pageIndices;
    std::sort(sorted.begin(), sorted.end(), std::greater<int>());
    for (int idx : sorted) {
        if (idx >= 0 && idx < FPDF_GetPageCount(ld.doc))
            FPDFPage_Delete(ld.doc, idx);
    }

    bool ok = saveDoc(ld.doc, outputPath);
    ld.close();
    return ok;
}

// ── reorderPages ───────────────────────────────────────────────────────────────
bool PdfEditor::reorderPages(const QString& inputFile, const QList<int>& newOrder,
                              const QString& outputPath) {
    QMutexLocker lock(&PdfDocument::pdfiumGlobalMutex());
    LoadedDoc ld = loadPdf(inputFile, m_lastError);
    if (!ld.doc) return false;

    int total = FPDF_GetPageCount(ld.doc);
    QVector<int> valid;
    for (int idx : newOrder)
        if (idx >= 0 && idx < total) valid.append(idx);

    if (valid.isEmpty()) { m_lastError = "No valid page indices in newOrder"; ld.close(); return false; }

    FPDF_DOCUMENT output = FPDF_CreateNewDocument();
    if (!output) { m_lastError = "Failed to create output document"; ld.close(); return false; }

    if (!FPDF_ImportPagesByIndex(output, ld.doc, valid.data(), valid.size(), 0)) {
        m_lastError = "Failed to reorder pages";
        FPDF_CloseDocument(output); ld.close();
        return false;
    }

    bool ok = saveDoc(output, outputPath);
    FPDF_CloseDocument(output);
    ld.close();
    return ok;
}

// ── reorderBookmarks ───────────────────────────────────────────────────────────
bool PdfEditor::reorderBookmarks(const QString& inputFile, const QList<int>& newOrder,
                                  const QString& outputPath) {
    // PDFium has no bookmark-manipulation API. Keep QPDF for this outline-only op.
    QMutexLocker lock(&PdfDocument::pdfiumGlobalMutex());
    // Release mutex before QPDF access, re-acquire after.
    lock.unlock();

    try {
        QPDF pdf;
        pdf.processFile(inputFile.toUtf8().constData());

        QPDFObjectHandle root = pdf.getRoot();
        QPDFObjectHandle outlines = root.hasKey("/Outlines")
                                        ? root.getKey("/Outlines")
                                        : QPDFObjectHandle();

        if (!outlines.isDictionary() || !outlines.hasKey("/First")) {
            QPDFWriter writer(pdf, outputPath.toUtf8().constData());
            writer.setStreamDataMode(qpdf_s_preserve);
            writer.write();
            return true;
        }

        QList<QPDFObjectHandle> items;
        QPDFObjectHandle cur = outlines.getKey("/First");
        while (cur.isDictionary()) {
            items.append(cur);
            if (!cur.hasKey("/Next") || cur.getKey("/Next").isNull()) break;
            cur = cur.getKey("/Next");
        }

        QList<QPDFObjectHandle> ordered;
        ordered.reserve(newOrder.size());
        for (int idx : newOrder)
            if (idx >= 0 && idx < items.size())
                ordered.append(items[idx]);
        for (int i = 0; i < items.size(); ++i) {
            if (!newOrder.contains(i))
                ordered.append(items[i]);
        }

        if (ordered.isEmpty()) {
            QPDFWriter writer(pdf, outputPath.toUtf8().constData());
            writer.setStreamDataMode(qpdf_s_preserve);
            writer.write();
            return true;
        }

        for (int i = 0; i < ordered.size(); ++i) {
            ordered[i].replaceKey("/Parent", outlines);
            if (i == 0) ordered[i].removeKey("/Prev");
            else        ordered[i].replaceKey("/Prev", ordered[i - 1]);
            if (i == ordered.size() - 1) ordered[i].removeKey("/Next");
            else                          ordered[i].replaceKey("/Next", ordered[i + 1]);
        }
        outlines.replaceKey("/First", ordered.first());
        outlines.replaceKey("/Last",  ordered.last());
        outlines.replaceKey("/Count", QPDFObjectHandle::newInteger(ordered.size()));

        QPDFWriter writer(pdf, outputPath.toUtf8().constData());
        writer.setStreamDataMode(qpdf_s_preserve);
        writer.write();
        return true;
    } catch (const std::exception& e) {
        m_lastError = QString::fromUtf8(e.what());
        return false;
    }
}

// ── insertPageFrom ─────────────────────────────────────────────────────────────
bool PdfEditor::insertPageFrom(const QString& targetFile, int targetInsertBefore,
                                const QString& sourceFile, int sourcePageIndex,
                                const QString& outputPath) {
    QMutexLocker lock(&PdfDocument::pdfiumGlobalMutex());

    LoadedDoc ldSrc = loadPdf(sourceFile, m_lastError);
    if (!ldSrc.doc) return false;

    LoadedDoc ldTgt = loadPdf(targetFile, m_lastError);
    if (!ldTgt.doc) { ldSrc.close(); return false; }

    int srcCount = FPDF_GetPageCount(ldSrc.doc);
    int tgtCount = FPDF_GetPageCount(ldTgt.doc);
    if (sourcePageIndex < 0 || sourcePageIndex >= srcCount) {
        m_lastError = "Source page index out of range"; ldSrc.close(); ldTgt.close(); return false;
    }

    int insertAt = qBound(0, targetInsertBefore, tgtCount);

    // Đọc outline gốc TRƯỚC khi đóng doc (page index ở không gian file gốc).
    QList<OutlineNode> tgtTree = readOutlineTree(ldTgt.doc, 0);
    QList<OutlineNode> srcTree = readOutlineTree(ldSrc.doc, 0);

    FPDF_DOCUMENT output = FPDF_CreateNewDocument();
    if (!output) { m_lastError = "Failed to create output document"; ldSrc.close(); ldTgt.close(); return false; }

    // Import target pages before insert point
    if (insertAt > 0) {
        QVector<int> before(insertAt);
        std::iota(before.begin(), before.end(), 0);
        if (!FPDF_ImportPagesByIndex(output, ldTgt.doc, before.data(), before.size(), 0)) {
            m_lastError = "Failed to import leading target pages";
            FPDF_CloseDocument(output); ldSrc.close(); ldTgt.close();
            return false;
        }
    }

    // Import source page
    int srcIdx = sourcePageIndex;
    if (!FPDF_ImportPagesByIndex(output, ldSrc.doc, &srcIdx, 1, insertAt)) {
        m_lastError = "Failed to import source page";
        FPDF_CloseDocument(output); ldSrc.close(); ldTgt.close();
        return false;
    }

    // Import remaining target pages
    int remaining = tgtCount - insertAt;
    if (remaining > 0) {
        QVector<int> after(remaining);
        std::iota(after.begin(), after.end(), insertAt);
        if (!FPDF_ImportPagesByIndex(output, ldTgt.doc, after.data(), after.size(), insertAt + 1)) {
            m_lastError = "Failed to import trailing target pages";
            FPDF_CloseDocument(output); ldSrc.close(); ldTgt.close();
            return false;
        }
    }

    bool ok = saveDoc(output, outputPath);
    FPDF_CloseDocument(output);
    ldSrc.close();
    ldTgt.close();

    if (ok) {
        // Remap trang về layout sau khi chèn 1 trang nguồn tại insertAt, rồi
        // dựng lại outline theo trang (giống merge) — count == số trang mới.
        QMap<int,int> tgtMap;
        for (int p = 0; p < tgtCount; ++p)
            tgtMap[p] = (p < insertAt) ? p : p + 1;
        QMap<int,int> srcMap;
        srcMap[sourcePageIndex] = insertAt;

        QList<OutlineNode> combined = filterOutlineTree(tgtTree, tgtMap);
        combined.append(filterOutlineTree(srcTree, srcMap));
        stripAutoPageTitles(combined);
        writeOutlineTree(outputPath, buildPagedOutline(combined, tgtCount + 1));
    }
    return ok;
}

// ── insertPdf ──────────────────────────────────────────────────────────────────
// Insert ALL pages of sourceFile into targetFile before page index `insertBefore`
// (use targetFile.pageCount() to append). Adobe-style "Insert Pages from File".
bool PdfEditor::insertPdf(const QString& targetFile, int insertBefore,
                          const QString& sourceFile, const QString& outputPath) {
    QMutexLocker lock(&PdfDocument::pdfiumGlobalMutex());

    LoadedDoc ldSrc = loadPdf(sourceFile, m_lastError);
    if (!ldSrc.doc) return false;
    LoadedDoc ldTgt = loadPdf(targetFile, m_lastError);
    if (!ldTgt.doc) { ldSrc.close(); return false; }

    int srcCount = FPDF_GetPageCount(ldSrc.doc);
    int tgtCount = FPDF_GetPageCount(ldTgt.doc);
    if (srcCount <= 0) { m_lastError = "Source PDF has no pages"; ldSrc.close(); ldTgt.close(); return false; }
    int insertAt = qBound(0, insertBefore, tgtCount);

    // Đọc outline gốc TRƯỚC khi đóng doc (page index ở không gian file gốc).
    QList<OutlineNode> tgtTree = readOutlineTree(ldTgt.doc, 0);
    QList<OutlineNode> srcTree = readOutlineTree(ldSrc.doc, 0);

    FPDF_DOCUMENT output = FPDF_CreateNewDocument();
    if (!output) { m_lastError = "Failed to create output document"; ldSrc.close(); ldTgt.close(); return false; }

    // Leading target pages [0, insertAt)
    if (insertAt > 0) {
        QVector<int> before(insertAt);
        std::iota(before.begin(), before.end(), 0);
        if (!FPDF_ImportPagesByIndex(output, ldTgt.doc, before.data(), before.size(), 0)) {
            m_lastError = "Failed to import leading target pages";
            FPDF_CloseDocument(output); ldSrc.close(); ldTgt.close(); return false;
        }
    }

    // All source pages (nullptr = every page), inserted at insertAt
    if (!FPDF_ImportPagesByIndex(output, ldSrc.doc, nullptr, 0, insertAt)) {
        m_lastError = "Failed to import source pages";
        FPDF_CloseDocument(output); ldSrc.close(); ldTgt.close(); return false;
    }

    // Trailing target pages [insertAt, tgtCount)
    int remaining = tgtCount - insertAt;
    if (remaining > 0) {
        QVector<int> after(remaining);
        std::iota(after.begin(), after.end(), insertAt);
        if (!FPDF_ImportPagesByIndex(output, ldTgt.doc, after.data(), after.size(), insertAt + srcCount)) {
            m_lastError = "Failed to import trailing target pages";
            FPDF_CloseDocument(output); ldSrc.close(); ldTgt.close(); return false;
        }
    }

    bool ok = saveDoc(output, outputPath);
    FPDF_CloseDocument(output);
    ldSrc.close();
    ldTgt.close();

    if (ok) {
        // Remap trang về layout sau khi chèn srcCount trang tại insertAt:
        //   target p < insertAt  → p              (giữ nguyên)
        //   target p >= insertAt → p + srcCount   (dời xuống)
        //   source p             → p + insertAt   (nằm trong đoạn chèn)
        // Dựng lại outline theo trang (giống merge) — count == số trang mới.
        QMap<int,int> tgtMap;
        for (int p = 0; p < tgtCount; ++p)
            tgtMap[p] = (p < insertAt) ? p : p + srcCount;
        QMap<int,int> srcMap;
        for (int p = 0; p < srcCount; ++p)
            srcMap[p] = p + insertAt;

        QList<OutlineNode> combined = filterOutlineTree(tgtTree, tgtMap);
        combined.append(filterOutlineTree(srcTree, srcMap));
        stripAutoPageTitles(combined);
        writeOutlineTree(outputPath, buildPagedOutline(combined, tgtCount + srcCount));
    }
    return ok;
}
