#pragma once
#include <QWidget>
#include <QListWidget>
#include <QTreeWidget>
#include <QStackedWidget>
#include <QButtonGroup>
#include <QAtomicInt>
#include <QList>
#include <QSet>
#include <QRectF>
#include <QString>
#include "annotations/AnnotationManager.h"
#include "core/PdfDocument.h"
#include "core/PdfRenderer.h"
#include "core/ThumbnailRenderPool.h"
#include "core/TextSearch.h"
#include <fpdfview.h>

class SearchPanel;
class QPushButton;

class ThumbnailPanel : public QWidget {
    Q_OBJECT
public:
    explicit ThumbnailPanel(QWidget* parent = nullptr);
    ~ThumbnailPanel() override;

    void setDocument(PdfDocument* doc, PdfRenderer* renderer,
                     ThumbnailRenderPool* pool = nullptr,
                     bool forceRebuild = false);
    void setComments(const QList<AnnotInfo>& comments);
    void setCurrentPage(int pageIndex);
    void clearThumbnails();
    void setDarkMode(bool dark);

    // Search (panel is hidden; kept for future version)
    void addSearchResult(const SearchResult& result);
    void clearSearchResults();
    void activateSearch();

signals:
    void pageClicked(int pageIndex);
    void pageContextMenu(int pageIndex, QPoint globalPos);
    void extractPagesRequested(QList<int> pageIndices);
    void searchRequested(const QString& query);
    void searchResultSelected(int pageIndex, QRectF boundingBox);
    void pagesReordered(QList<int> newOrder);
    void bookmarksReordered(QList<int> newOrder);
    void annotToolSelected(int toolId);
    void commentActivated(int pageIndex);
    void annotStyleChanged(QColor color, double width, bool fill);

private slots:
    void onPageReady(int pageIndex, const QImage& image);

private:
    void buildBookmarks();
    void buildContentTree();
    void buildProperties();
    void syncBookmarkToPage(int pageIndex);

    // Tab navigation: 2×2 button grid + stacked content widget
    QStackedWidget* m_stack          = nullptr;
    QButtonGroup*   m_tabGroup       = nullptr;

    QWidget*      m_commentsPanel   = nullptr;
    QListWidget*  m_commentsList    = nullptr;
    QListWidget*  m_list            = nullptr;
    QTreeWidget*  m_outline         = nullptr;
    QTreeWidget*  m_contentTree     = nullptr;
    QTreeWidget*  m_propertiesTree  = nullptr;
    SearchPanel*  m_searchPanel     = nullptr;
    PdfDocument*         m_doc       = nullptr;
    PdfRenderer*         m_renderer  = nullptr;
    ThumbnailRenderPool* m_thumbPool = nullptr;
    QMetaObject::Connection m_rendererConn;
    QMetaObject::Connection m_thumbPoolConn;
    QMetaObject::Connection m_scrollConn;
    int           m_currentPage = -1;
    QAtomicInt    m_contentGen{0};
    QAtomicInt    m_bookmarkGen{0};
    QAtomicInt    m_propsGen{0};
    QColor       m_annColor = Qt::red;
    double       m_annWidth = 2.0;
    bool         m_annFill  = false;
    QPushButton* m_colorBtn = nullptr;
};
