#include "ThumbnailPanel.h"
#include "SearchPanel.h"
#include <QDebug>
#include <QTimer>
#include <QVBoxLayout>
#include <QGridLayout>
#include <QScrollBar>
#include <QMutex>
#include <QFutureWatcher>
#include <QtConcurrent>
#include <QMenu>
#include <QPushButton>
#include <QFileInfo>
#include <QHeaderView>
#include <QDragMoveEvent>
#include <fpdf_doc.h>
#include <fpdf_text.h>
#include <QKeySequence>
#include <QShortcut>
#include <QClipboard>
#include <QApplication>
#include <algorithm>
#include <functional>
#include <vector>

extern QMutex s_pdfiumMutex;

// Qt6 QListWidget with InternalMove does NOT reliably emit rowsMoved
// (it does rowsRemoved+rowsInserted).  Use onDropped callback instead.
class DropListWidget : public QListWidget {
public:
    using QListWidget::QListWidget;
    std::function<void()> onDropped;
protected:
    void dropEvent(QDropEvent* e) override {
        QListWidget::dropEvent(e);
        if (onDropped)
            QTimer::singleShot(0, this, [this]{ if (onDropped) onDropped(); });
    }
};

class BookmarkTreeWidget : public QTreeWidget {
public:
    using QTreeWidget::QTreeWidget;
    // Called after a successful drop so caller can collect the new order.
    // Qt6 QTreeWidget with InternalMove emits rowsRemoved+rowsInserted,
    // NOT rowsMoved, so we can't rely on the rowsMoved signal.
    std::function<void()> onDropped;

protected:
    // Only top-level items can be dragged
    void startDrag(Qt::DropActions actions) override {
        for (auto* item : selectedItems())
            if (item->parent()) return;
        QTreeWidget::startDrag(actions);
    }
    // Prevent "drop onto item" (reparenting); allow above/below only
    void dragMoveEvent(QDragMoveEvent* e) override {
        QPoint pos = e->position().toPoint();
        QModelIndex idx = indexAt(pos);
        if (idx.isValid()) {
            if (idx.parent().isValid()) { e->ignore(); return; }
            QRect r = visualRect(idx);
            int y = pos.y() - r.top(), h = r.height();
            if (y > h / 4 && y < h * 3 / 4) { e->ignore(); return; }
        }
        QTreeWidget::dragMoveEvent(e);
    }
    void dropEvent(QDropEvent* e) override {
        QModelIndex idx = indexAt(e->position().toPoint());
        if (idx.isValid() && idx.parent().isValid()) { e->ignore(); return; }
        QTreeWidget::dropEvent(e);
        // Fire after the event loop settles so the model reflects the new order.
        if (onDropped) QTimer::singleShot(0, this, [this]{ if (onDropped) onDropped(); });
    }
};

// Filter raw PDFium text: PDFs without ToUnicode tables produce garbage glyph-IDs.
static QString cleanPdfText(const QString& raw) {
    if (raw.isEmpty()) return {};
    QString result;
    result.reserve(raw.size());
    int bad = 0;
    for (QChar c : raw) {
        ushort u = c.unicode();
        bool ok = (u >= 0x0020 && u <= 0x007E)
               || (u >= 0x00A0 && u <= 0x024F)
               || (u >= 0x1E00 && u <= 0x1EFF)
               || (u >= 0x2000 && u <= 0x206F)
               || (u >= 0x4E00 && u <= 0x9FFF)
               || (u >= 0xAC00 && u <= 0xD7A3)
               || (u >= 0x3000 && u <= 0x303F)
               || (u == 0x0009 || u == 0x000A || u == 0x000D);
        if (ok) result += c;
        else    ++bad;
    }
    if (bad * 100 / raw.size() > 40) return {};
    return result.simplified();
}

// ── Constructor ───────────────────────────────────────────────────────────────
ThumbnailPanel::ThumbnailPanel(QWidget* parent) : QWidget(parent) {

    // ── Thumbnails tab ────────────────────────────────────────────────────
    m_list = new QListWidget(this);
    m_list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    QFont f = m_list->font();
    f.setPointSize(8);
    m_list->setFont(f);
    m_list->setViewMode(QListWidget::IconMode);
    m_list->setIconSize({110, 150});
    m_list->setResizeMode(QListWidget::Adjust);
    m_list->setSpacing(2);
    m_list->setDragEnabled(true);
    m_list->setAcceptDrops(true);
    m_list->setDropIndicatorShown(true);
    m_list->setDragDropMode(QAbstractItemView::InternalMove);
    m_list->setDefaultDropAction(Qt::MoveAction);

    connect(m_list, &QListWidget::customContextMenuRequested, this,
            [this](const QPoint& pos) {
        auto sel = m_list->selectedItems();
        if (sel.size() > 1) {
            QList<int> pages;
            for (auto* it : sel) pages.append(m_list->row(it));
            std::sort(pages.begin(), pages.end());
            QMenu menu;
            menu.addAction(
                QString("Extract %1 Pages to New File…").arg(pages.size()),
                [this, pages]{ emit extractPagesRequested(pages); });
            menu.exec(m_list->mapToGlobal(pos));
        } else if (auto* item = m_list->itemAt(pos)) {
            emit pageContextMenu(m_list->row(item), m_list->mapToGlobal(pos));
        }
    });
    connect(m_list, &QListWidget::itemClicked, this, [this](QListWidgetItem* item){
        emit pageClicked(m_list->row(item));
    });

    // ── Bookmarks tab ─────────────────────────────────────────────────────
    m_outline = new BookmarkTreeWidget(this);
    m_outline->setHeaderHidden(true);
    m_outline->setRootIsDecorated(true);
    m_outline->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_outline->setDragEnabled(true);
    m_outline->setAcceptDrops(true);
    m_outline->setDropIndicatorShown(true);
    m_outline->setDragDropMode(QAbstractItemView::InternalMove);

    connect(m_outline, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* item, int){
        // During Ctrl/Shift multi-select, don't navigate — preserve the selection.
        if (m_outline->selectedItems().size() > 1) return;
        int page = item->data(0, Qt::UserRole).toInt();
        if (page >= 0) emit pageClicked(page);
    });
    m_outline->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_outline, &QTreeWidget::customContextMenuRequested, this,
            [this](const QPoint& pos) {
        auto sel = m_outline->selectedItems();
        if (sel.size() > 1) {
            QList<int> pages;
            for (auto* it : sel) {
                int pg = it->data(0, Qt::UserRole).toInt();
                if (pg >= 0 && !pages.contains(pg)) pages.append(pg);
            }
            std::sort(pages.begin(), pages.end());
            if (pages.isEmpty()) return;
            QMenu menu;
            menu.addAction(
                QString("Extract %1 Pages to New File…").arg(pages.size()),
                [this, pages]{ emit extractPagesRequested(pages); });
            menu.exec(m_outline->mapToGlobal(pos));
        } else if (auto* item = m_outline->itemAt(pos)) {
            int page = item->data(0, Qt::UserRole).toInt();
            if (page >= 0)
                emit pageContextMenu(page, m_outline->mapToGlobal(pos));
        }
    });

    // ── Content tab ───────────────────────────────────────────────────────
    // Re-use BookmarkTreeWidget so we get the same onDropped mechanism.
    m_contentTree = new BookmarkTreeWidget(this);
    m_contentTree->setHeaderHidden(true);
    m_contentTree->setRootIsDecorated(false);
    m_contentTree->setDragEnabled(true);
    m_contentTree->setAcceptDrops(true);
    m_contentTree->setDropIndicatorShown(true);
    m_contentTree->setDragDropMode(QAbstractItemView::InternalMove);
    m_contentTree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    connect(m_contentTree, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* item, int){
        int page = item->data(0, Qt::UserRole).toInt();
        qDebug() << "[Content] itemClicked page=" << page;
        if (page >= 0) emit pageClicked(page);
    });
    // Ctrl+A / Ctrl+C: select all / copy text from content tab
    auto* copyShortcut = new QShortcut(QKeySequence::Copy, m_contentTree);
    copyShortcut->setContext(Qt::WidgetShortcut);
    connect(copyShortcut, &QShortcut::activated, this, [this]() {
        QStringList lines;
        for (auto* item : m_contentTree->selectedItems())
            lines.append(item->text(0));
        if (!lines.isEmpty())
            QApplication::clipboard()->setText(lines.join("\n"));
    });
    auto* selAllShortcut = new QShortcut(QKeySequence::SelectAll, m_contentTree);
    selAllShortcut->setContext(Qt::WidgetShortcut);
    connect(selAllShortcut, &QShortcut::activated, m_contentTree, &QTreeWidget::selectAll);

    // ── Properties tab ────────────────────────────────────────────────────
    m_propertiesTree = new QTreeWidget(this);
    m_propertiesTree->setHeaderLabels({"Property", "Value"});
    m_propertiesTree->setRootIsDecorated(false);
    m_propertiesTree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_propertiesTree->header()->setStretchLastSection(true);
    m_propertiesTree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);

    // ── Search panel (hidden, kept for future version) ────────────────────
    m_searchPanel = new SearchPanel(this);

    // ── 2×2 tab-button grid + stacked content ────────────────────────────
    auto makeTabBtn = [](const QString& text) {
        auto* btn = new QPushButton(text);
        btn->setCheckable(true);
        btn->setObjectName("sidebarTab");
        btn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        btn->setFixedHeight(26);
        return btn;
    };

    auto* thumbBtn = makeTabBtn("Thumbnails");
    auto* booksBtn = makeTabBtn("Bookmarks");
    auto* contBtn  = makeTabBtn("Content");
    auto* propBtn  = makeTabBtn("Properties");
    thumbBtn->setChecked(true);

    m_tabGroup = new QButtonGroup(this);
    m_tabGroup->setExclusive(true);
    m_tabGroup->addButton(thumbBtn, 0);
    m_tabGroup->addButton(booksBtn, 1);
    m_tabGroup->addButton(contBtn,  2);
    m_tabGroup->addButton(propBtn,  3);

    auto* tabGrid = new QWidget;
    auto* gl      = new QGridLayout(tabGrid);
    gl->setContentsMargins(0, 0, 0, 0);
    gl->setSpacing(1);
    gl->setColumnStretch(0, 1);
    gl->setColumnStretch(1, 1);
    gl->addWidget(thumbBtn, 0, 0);
    gl->addWidget(booksBtn, 0, 1);
    gl->addWidget(contBtn,  1, 0);
    gl->addWidget(propBtn,  1, 1);

    m_stack = new QStackedWidget;
    m_stack->addWidget(m_list);           // idx 0 — Thumbnails
    m_stack->addWidget(m_outline);        // idx 1 — Bookmarks
    m_stack->addWidget(m_contentTree);    // idx 2 — Content
    m_stack->addWidget(m_propertiesTree); // idx 3 — Properties

    connect(m_tabGroup, &QButtonGroup::idClicked, m_stack, &QStackedWidget::setCurrentIndex);
    connect(m_tabGroup, &QButtonGroup::idClicked, this, [this](int id) {
        if (id == 2 && m_contentTree->topLevelItemCount() == 0
                && m_doc && m_doc->isOpen())
            buildContentTree();
        if (id == 3 && m_propertiesTree->topLevelItemCount() == 0
                && m_doc && m_doc->isOpen())
            buildProperties();
    });

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(tabGrid, 0);
    layout->addWidget(m_stack, 1);

    // Wire drag-reorder: pages
    connect(m_list->model(), &QAbstractItemModel::rowsMoved, this,
            [this](const QModelIndex&, int, int, const QModelIndex&, int) {
        QTimer::singleShot(0, this, [this]() {
            QList<int> newOrder;
            for (int i = 0; i < m_list->count(); ++i)
                newOrder.append(m_list->item(i)->data(Qt::UserRole).toInt());
            emit pagesReordered(newOrder);
        });
    });

    // Wire drag-reorder: bookmarks (top-level only).
    // Qt6 QTreeWidget with InternalMove does NOT reliably emit rowsMoved —
    // use the onDropped callback in BookmarkTreeWidget instead.
    static_cast<BookmarkTreeWidget*>(m_outline)->onDropped = [this]() {
        QList<int> newOrder;
        for (int i = 0; i < m_outline->topLevelItemCount(); ++i)
            newOrder.append(m_outline->topLevelItem(i)->data(0, Qt::UserRole + 1).toInt());
        emit bookmarksReordered(newOrder);
    };

    // Wire drag-reorder: content tab (same page-reorder as thumbnails).
    static_cast<BookmarkTreeWidget*>(m_contentTree)->onDropped = [this]() {
        QList<int> newOrder;
        for (int i = 0; i < m_contentTree->topLevelItemCount(); ++i)
            newOrder.append(m_contentTree->topLevelItem(i)->data(0, Qt::UserRole).toInt());
        emit pagesReordered(newOrder);
    };
}

ThumbnailPanel::~ThumbnailPanel() {
    if (m_renderer) disconnect(m_rendererConn);
}

// ── setDocument ───────────────────────────────────────────────────────────────
void ThumbnailPanel::setDocument(PdfDocument* doc, PdfRenderer* renderer,
                                  ThumbnailRenderPool* pool, bool forceRebuild) {
    // forceRebuild: sau khi sửa file (insert/delete/reorder…) doc được mở lại
    // TRÊN CÙNG con trỏ — phải dựng lại thumbnail + bookmark, không được early-return.
    if (!forceRebuild && doc == m_doc && renderer == m_renderer && m_list->count() > 0)
        return;

    m_doc = doc;
    if (m_renderer) disconnect(m_rendererConn);
    if (m_thumbPool) disconnect(m_thumbPoolConn);
    m_renderer  = renderer;
    m_thumbPool = pool;
    if (m_thumbPool)
        m_thumbPoolConn = connect(m_thumbPool, &ThumbnailRenderPool::thumbnailReady,
                                  this, &ThumbnailPanel::onPageReady);
    m_list->clear();
    m_currentPage = -1;
    m_contentGen.fetchAndAddOrdered(1);
    m_bookmarkGen.fetchAndAddOrdered(1);
    m_contentTree->clear();
    m_propertiesTree->clear();

    if (!m_doc || !m_renderer) return;

    int n = m_doc->pageCount();

    for (int i = 0; i < n; ++i) {
        auto* item = new QListWidgetItem(QString::number(i + 1), m_list);
        item->setSizeHint({125, 170});
        item->setData(Qt::UserRole, i);
    }

    // Connect BEFORE issuing requests so tile-cache hits (synchronous emit) are not lost
    m_rendererConn = connect(m_renderer, &PdfRenderer::pageReady,
                             this, &ThumbnailPanel::onPageReady);

    // Use ThumbnailRenderPool (parallel) if available, else fallback to PdfRenderer
    if (m_thumbPool && m_thumbPool->isOpen()) {
        // Request first 12 visible pages at high priority
        int initial = qMin(n, 12);
        for (int i = 0; i < initial; ++i)
            m_thumbPool->requestThumbnail(i, 0);
        // Prefetch rest [12..99] in background after 200ms
        if (n > initial)
            QTimer::singleShot(200, this, [this, initial, n]() {
                if (m_thumbPool) m_thumbPool->prefetchRange(initial, qMin(n - 1, 99));
            });
    } else if (m_renderer) {
        static constexpr double kThumbScale = 0.15;
        int limit = qMin(n, 8);
        for (int i = 0; i < limit; ++i)
            m_renderer->requestPage(i, kThumbScale);
    }

    if (m_scrollConn) disconnect(m_scrollConn);
    m_scrollConn = connect(m_list->verticalScrollBar(), &QScrollBar::valueChanged,
            this, [this, n]() {
        int topRow = m_list->row(m_list->itemAt(QPoint(0, 1)));
        if (topRow < 0) topRow = 0;
        int iconH = m_list->iconSize().height() + m_list->spacing() * 2 + 22;
        int colW  = m_list->iconSize().width()  + m_list->spacing() * 2 + 4;
        int cols  = qMax(1, m_list->viewport()->width()  / qMax(1, colW));
        int rows  = qMax(1, m_list->viewport()->height() / qMax(1, iconH));
        int ahead = (rows + 2) * cols;
        for (int i = topRow; i < qMin(topRow + ahead, n); ++i) {
            auto* item = m_list->item(i);
            if (!item || !item->icon().isNull()) continue;
            if (m_thumbPool && m_thumbPool->isOpen())
                m_thumbPool->requestThumbnail(i, 1); // adjacent priority
            else if (m_renderer)
                m_renderer->requestPage(i, 0.15);
        }
    });

    buildBookmarks();
    // Properties built lazily on tab click; pre-populate now only if already visible
    if (m_stack->currentIndex() == 3)
        buildProperties();
}

// ── setCurrentPage ────────────────────────────────────────────────────────────
void ThumbnailPanel::setCurrentPage(int pageIndex) {
    if (m_currentPage >= 0 && m_currentPage < m_list->count())
        m_list->item(m_currentPage)->setBackground(Qt::transparent);
    m_currentPage = pageIndex;
    if (pageIndex >= 0 && pageIndex < m_list->count()) {
        auto* item = m_list->item(pageIndex);
        item->setBackground(QColor(37, 99, 235, 90));
        m_list->scrollToItem(item, QAbstractItemView::PositionAtCenter);
    }
    syncBookmarkToPage(pageIndex);
}

// ── clearThumbnails ───────────────────────────────────────────────────────────
void ThumbnailPanel::clearThumbnails() {
    m_doc = nullptr;
    if (m_renderer) disconnect(m_rendererConn);
    if (m_scrollConn) disconnect(m_scrollConn);
    m_renderer = nullptr;
    m_list->clear();
    m_outline->clear();
    m_contentGen.fetchAndAddOrdered(1);
    m_bookmarkGen.fetchAndAddOrdered(1);
    m_contentTree->clear();
    m_propertiesTree->clear();
    m_currentPage = -1;
}

// ── Search helpers (kept as no-ops; panel hidden) ─────────────────────────────
void ThumbnailPanel::addSearchResult(const SearchResult& result) {
    m_searchPanel->addResult(result);
}
void ThumbnailPanel::clearSearchResults() { m_searchPanel->clearResults(); }
void ThumbnailPanel::activateSearch() {}

// ── setDarkMode ───────────────────────────────────────────────────────────────
void ThumbnailPanel::setDarkMode(bool dark) { Q_UNUSED(dark) }

// ── onPageReady ───────────────────────────────────────────────────────────────
void ThumbnailPanel::onPageReady(int pageIndex, const QImage& image) {
    if (!m_doc || pageIndex < 0 || pageIndex >= m_list->count()) return;
    double expectedW = m_doc->pageSize(pageIndex).width() * 0.15;
    if (image.width() > expectedW * 3.0) return;
    if (auto* item = m_list->item(pageIndex))
        item->setIcon(QIcon(QPixmap::fromImage(image)));
}

// ── buildBookmarks (async) ────────────────────────────────────────────────────
struct BmEntry { int depth; QString title; int page; };

void ThumbnailPanel::buildBookmarks() {
    m_outline->clear();
    if (!m_doc || !m_doc->isOpen()) return;

    FPDF_DOCUMENT doc   = m_doc->raw();
    int           myGen = m_bookmarkGen.fetchAndAddOrdered(1) + 1;

    auto* watcher = new QFutureWatcher<QVector<BmEntry>>(this);

    auto future = QtConcurrent::run([doc, myGen, this]() -> QVector<BmEntry> {
        QVector<BmEntry> entries;
        QMutexLocker lock(&s_pdfiumMutex);
        if (m_bookmarkGen.loadAcquire() != myGen) return entries;

        std::function<void(FPDF_BOOKMARK, int)> walk;
        walk = [&](FPDF_BOOKMARK bm, int depth) {
            while (bm) {
                if (m_bookmarkGen.loadAcquire() != myGen) return;

                unsigned long len = FPDFBookmark_GetTitle(bm, nullptr, 0);
                std::vector<char> buf(len + 2, 0);
                FPDFBookmark_GetTitle(bm, buf.data(), len);
                QString title = QString::fromUtf16(
                    reinterpret_cast<const char16_t*>(buf.data()));
                if (title.isEmpty()) title = "(untitled)";

                FPDF_DEST dest = FPDFBookmark_GetDest(doc, bm);
                // Fallback: bookmark uses /A GoTo action instead of /Dest directly
                if (!dest) {
                    FPDF_ACTION action = FPDFBookmark_GetAction(bm);
                    if (action && FPDFAction_GetType(action) == PDFACTION_GOTO)
                        dest = FPDFAction_GetDest(doc, action);
                }
                int page = dest ? FPDFDest_GetDestPageIndex(doc, dest) : -1;

                entries.append({depth, title, page});

                FPDF_BOOKMARK child = FPDFBookmark_GetFirstChild(doc, bm);
                if (child) walk(child, depth + 1);
                bm = FPDFBookmark_GetNextSibling(doc, bm);
            }
        };
        walk(FPDFBookmark_GetFirstChild(doc, nullptr), 0);
        return entries;
    });

    connect(watcher, &QFutureWatcher<QVector<BmEntry>>::finished, this,
            [this, watcher, myGen]() {
        watcher->deleteLater();
        if (m_bookmarkGen.loadAcquire() != myGen) return;

        auto entries = watcher->result();
        m_outline->clear();

        if (entries.isEmpty()) {
            auto* none = new QTreeWidgetItem(m_outline, {"(No bookmarks)"});
            none->setDisabled(true);
            return;
        }

        // Rebuild tree from flat list with depth info using a parent stack.
        QVector<QTreeWidgetItem*> stack;
        int topIdx = 0;
        for (const auto& e : entries) {
            QTreeWidgetItem* item;
            if (e.depth == 0 || stack.isEmpty()) {
                item = new QTreeWidgetItem(m_outline, {e.title});
                item->setData(0, Qt::UserRole + 1, topIdx++);
                item->setFlags(item->flags() | Qt::ItemIsDropEnabled);
                stack = {item};
            } else {
                while (stack.size() > e.depth) stack.pop_back();
                item = new QTreeWidgetItem(stack.last(), {e.title});
                if ((int)stack.size() <= e.depth) stack.append(item);
                else stack[e.depth] = item;
            }
            item->setData(0, Qt::UserRole, e.page);
        }
        m_outline->expandAll();

        // Sync to current page after bookmarks are loaded
        if (m_currentPage >= 0) syncBookmarkToPage(m_currentPage);
    });
    watcher->setFuture(future);
}

// ── syncBookmarkToPage ────────────────────────────────────────────────────────
void ThumbnailPanel::syncBookmarkToPage(int pageIndex) {
    // When the Bookmarks tab is active, user may be making a multi-selection — don't interfere.
    if (m_stack->currentIndex() == 1) return;
    if (pageIndex < 0 || m_outline->topLevelItemCount() == 0) return;

    QTreeWidgetItem* best     = nullptr;
    int              bestPage = -1;

    std::function<void(QTreeWidgetItem*)> search = [&](QTreeWidgetItem* item) {
        int pg = item->data(0, Qt::UserRole).toInt();
        if (pg >= 0 && pg <= pageIndex && pg > bestPage) {
            bestPage = pg;
            best     = item;
        }
        for (int i = 0; i < item->childCount(); ++i)
            search(item->child(i));
    };
    for (int i = 0; i < m_outline->topLevelItemCount(); ++i)
        search(m_outline->topLevelItem(i));

    if (best) {
        QSignalBlocker b(m_outline);
        m_outline->clearSelection();
        best->setSelected(true);
        m_outline->scrollToItem(best, QAbstractItemView::EnsureVisible);
    }
}

// ── buildContentTree ──────────────────────────────────────────────────────────
void ThumbnailPanel::buildContentTree() {
    m_contentTree->clear();
    if (!m_doc || !m_doc->isOpen()) return;

    FPDF_DOCUMENT doc  = m_doc->raw();
    int           n    = m_doc->pageCount();
    int           myGen = m_contentGen.fetchAndAddOrdered(1) + 1;

    auto* watcher = new QFutureWatcher<QList<QPair<int,QString>>>(this);

    auto future = QtConcurrent::run([doc, n, myGen, this]() {
        QList<QPair<int,QString>> results;
        for (int i = 0; i < n; ++i) {
            if (m_contentGen.loadAcquire() != myGen) break;
            QString preview;
            {
                QMutexLocker lock(&s_pdfiumMutex);
                if (m_contentGen.loadAcquire() != myGen) break;
                FPDF_PAGE page = FPDF_LoadPage(doc, i);
                if (page) {
                    FPDF_TEXTPAGE tp = FPDFText_LoadPage(page);
                    if (tp) {
                        int cnt = qMin(FPDFText_CountChars(tp), 120);
                        std::vector<unsigned short> buf(static_cast<size_t>(cnt) + 1, 0);
                        FPDFText_GetText(tp, 0, cnt, buf.data());
                        preview = cleanPdfText(
                            QString::fromUtf16(
                                reinterpret_cast<const char16_t*>(buf.data()))
                        ).left(100);
                        FPDFText_ClosePage(tp);
                    }
                    FPDF_ClosePage(page);
                }
            }
            if (preview.isEmpty()) preview = "(font encoding not supported)";
            results.append({i, preview});
        }
        return results;
    });

    connect(watcher, &QFutureWatcher<QList<QPair<int,QString>>>::finished,
            this, [this, watcher, myGen] {
        watcher->deleteLater();
        if (m_contentGen.loadAcquire() != myGen) return;
        auto results = watcher->result();
        m_contentTree->clear();
        for (const auto& [idx, text] : results) {
            auto* item = new QTreeWidgetItem(m_contentTree,
                {QString("p.%1  %2").arg(idx + 1).arg(text)});
            item->setData(0, Qt::UserRole, idx);
            item->setFlags(item->flags() | Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled);
        }
    });
    watcher->setFuture(future);
}

// ── buildProperties (async — avoids blocking main thread on s_pdfiumMutex) ────
void ThumbnailPanel::buildProperties() {
    m_propertiesTree->clear();
    if (!m_doc || !m_doc->isOpen()) return;

    FPDF_DOCUMENT doc      = m_doc->raw();
    QString       filePath = m_doc->filePath();
    int           pages    = m_doc->pageCount();
    int           myGen    = m_propsGen.fetchAndAddOrdered(1) + 1;

    struct Props {
        int     fileVersion = 0;
        QString title, author, subject, creator, producer, created, modified;
    };

    auto* watcher = new QFutureWatcher<Props>(this);

    auto future = QtConcurrent::run([doc, myGen, this]() -> Props {
        Props p;
        auto getMeta = [&](const char* tag) -> QString {
            QMutexLocker lk(&s_pdfiumMutex);
            if (m_propsGen.loadAcquire() != myGen) return {};
            unsigned long len = FPDF_GetMetaText(doc, tag, nullptr, 0);
            if (!len) return {};
            std::vector<char> buf(len + 2, 0);
            FPDF_GetMetaText(doc, tag, buf.data(), len);
            return QString::fromUtf16(
                reinterpret_cast<const char16_t*>(buf.data())).trimmed();
        };
        { QMutexLocker lk(&s_pdfiumMutex); FPDF_GetFileVersion(doc, &p.fileVersion); }
        p.title    = getMeta("Title");
        p.author   = getMeta("Author");
        p.subject  = getMeta("Subject");
        p.creator  = getMeta("Creator");
        p.producer = getMeta("Producer");
        p.created  = getMeta("CreationDate");
        p.modified = getMeta("ModDate");
        return p;
    });

    connect(watcher, &QFutureWatcher<Props>::finished, this,
            [this, watcher, myGen, filePath, pages]() {
        watcher->deleteLater();
        if (m_propsGen.loadAcquire() != myGen) return;
        Props p = watcher->result();

        auto add = [&](const QString& k, const QString& v) {
            if (v.isEmpty()) return;
            new QTreeWidgetItem(m_propertiesTree, {k, v});
        };
        auto fmtDate = [](const QString& d) -> QString {
            if (d.startsWith("D:") && d.length() >= 10)
                return d.mid(2, 4) + "-" + d.mid(6, 2) + "-" + d.mid(8, 2);
            return d;
        };

        add("Pages", QString::number(pages));
        if (p.fileVersion > 0)
            add("PDF Version", QString("PDF %1.%2")
                .arg(p.fileVersion / 10).arg(p.fileVersion % 10));
        QFileInfo fi(filePath);
        if (fi.exists()) {
            qint64 b = fi.size();
            add("File Size", b < 1024*1024
                ? QString("%1 KB").arg(b / 1024)
                : QString("%1 MB").arg(b / (1024*1024)));
            add("File Path", fi.absoluteFilePath());
        }
        add("Title",    p.title);
        add("Author",   p.author);
        add("Subject",  p.subject);
        add("Creator",  p.creator);
        add("Producer", p.producer);
        add("Created",  fmtDate(p.created));
        add("Modified", fmtDate(p.modified));
        m_propertiesTree->resizeColumnToContents(0);
    });
    watcher->setFuture(future);
}
