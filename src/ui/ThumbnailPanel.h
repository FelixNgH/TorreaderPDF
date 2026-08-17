#pragma once
#include <QWidget>
#include <QListWidget>
#include <QTreeWidget>
#include <QStackedWidget>
#include <QButtonGroup>
#include <QAtomicInt>
#include <QList>
#include <QSet>
#include <QHash>
#include <QRectF>
#include <QString>
#include "annotations/AnnotationManager.h"
#include "core/PdfDocument.h"
#include "core/PdfRenderer.h"
#include "core/ThumbnailRenderPool.h"
#include "core/TextSearch.h"
#include "SearchPanel.h"
#include "OcrPanel.h"
#include <fpdfview.h>

class QPushButton;
class QComboBox;
class QLabel;
class QFrame;

class ThumbnailPanel : public QWidget {
    Q_OBJECT
public:
    explicit ThumbnailPanel(QWidget* parent = nullptr);
    ~ThumbnailPanel() override;

    void setDocument(PdfDocument* doc, PdfRenderer* renderer,
                     ThumbnailRenderPool* pool = nullptr,
                     bool forceRebuild = false);
    void setComments(const QList<AnnotInfo>& comments);
    void setCommentsLoading(bool loading);
    void setCommentsProgress(int scanned, int total);
    void setAnnotMgr(AnnotationManager* mgr, int pageCount);
    void setCurrentPage(int pageIndex);
    void setActiveToolButton(int id);
    int  activeTool() const { return m_activeTool; }
    // Goi dung luong dien tu khi bam nut cong cu trong luoi (probe + dieu khien
    // tu ma, SPEC_FIX_PICK_TOOL muc NGHIEM THU).
    void activateToolFromGrid(int id);
    bool isCommentsTabVisible() const { return m_stack && m_stack->currentIndex() == 2; }
    QImage thumbnailForPage(int pageIndex) const;
    void clearThumbnails();
    void setDarkMode(bool dark);
    void selectCommentFor(int pageIndex, int annotIndex);

    // Search
    void addSearchResult(const SearchResult& result);
    void clearSearchResults();
    void activateSearch();
    void setSearchProgress(int pagesScanned, int totalPages);
    // Xoa CA o nhap + danh sach + nhan dem cua SearchPanel (doi tab).
    void resetSearch();
    // Nap lai trang thai tim kiem cua tab: truy van + danh sach ket qua.
    void setSearchResults(const QString& query, const QList<SearchResult>& results);
    // Probe (nghiem thu bang so): so ket qua + truy van dang hien thi.
    int     probeSearchCount() const { return m_searchPanel ? m_searchPanel->probeCount() : -1; }
    QString probeSearchQuery() const { return m_searchPanel ? m_searchPanel->probeQuery() : QString(); }

    // Debug counters for --thumbepoch-test harness.
    int  debugEarlyReturnCount() const { return m_dbgEarlyReturn; }
    int  debugAcceptedCount() const { return m_dbgAccepted; }
    int  debugDroppedCount()  const { return m_dbgDropped; }
    int  debugPendingCount()  const { return m_dbgPending; }
    int  debugRejectedCount() const { return m_dbgRejected; }
    void debugResetCounters() { m_dbgAccepted = 0; m_dbgDropped = 0; m_dbgPending = 0; m_dbgRejected = 0; m_dbgEarlyReturn = 0; }

    // Panel OCR cua sidebar (tab id 5, SPEC_OCR_TAB_AND_SELECT phan 1).
    OcrPanel* ocrPanel() const { return m_ocrPanel; }
    // Chuyen tab sidebar theo id (probe + dieu khien tu ma).
    void selectTab(int id);
    int  currentTabIndex() const { return m_stack ? m_stack->currentIndex() : 0; }

public slots:
    void onPageReady(int pageIndex, const QImage& image, quint64 epoch);

signals:
    void pageClicked(int pageIndex);
    void pageContextMenu(int pageIndex, QPoint globalPos);
    void extractPagesRequested(QList<int> pageIndices);
    void searchRequested(const QString& query, bool matchDiacritics);
    void searchResultSelected(int pageIndex, QList<QRectF> rects);
    void searchCleared();
    void pagesReordered(QList<int> newOrder);
    void bookmarksReordered(QList<int> newOrder);
    void annotToolSelected(int toolId);
    void commentActivated(int pageIndex, int annotIndex);
    void commentTextEdited(int page, int indexInPage, const QString& text);
    void annotStyleChanged(QColor color, double width, bool fill, int fillOpacityPct, double fontSize);
    void requestComments();

private:
    bool eventFilter(QObject* o, QEvent* e) override;
    void requestVisibleThumbnails();
    void resizeEvent(QResizeEvent* event) override;
    void buildBookmarks();
    void buildContentTree();
    void buildProperties();
    void syncBookmarkToPage(int pageIndex);
    void flushPendingThumbs();
    void updateSizeComboForTool(int toolId);
    void applyToolButtonStyles();
    void updateColorBtnStyle();
    QColor currentPageHighlight() const;

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
    OcrPanel*     m_ocrPanel        = nullptr;
    PdfDocument*         m_doc       = nullptr;
    PdfRenderer*         m_renderer  = nullptr;
    ThumbnailRenderPool* m_thumbPool = nullptr;
    QMetaObject::Connection m_thumbPoolConn;
    QMetaObject::Connection m_scrollConn;
    int           m_currentPage = -1;
    QAtomicInt    m_contentGen{0};
    QAtomicInt    m_bookmarkGen{0};
    QAtomicInt    m_propsGen{0};
    AnnotationManager* m_annotMgr   = nullptr;
    int                m_annotPages = 0;
    QColor       m_annColor = Qt::red;
    double       m_annWidth = 2.0;
    bool         m_annFill  = false;
    int          m_annFillOpacity = 50;
    QPushButton* m_colorBtn = nullptr;
    QComboBox*   m_sizeCombo   = nullptr;
    QLabel*      m_commentsHint = nullptr;
    QFrame*      m_commentsSep  = nullptr;
    double       m_annFontSize = 24.0;
    bool         m_sizeIsFont  = false;
    bool         m_dark        = false;
    int          m_activeTool  = 0;
    QHash<int, QPushButton*> m_toolButtons;
    QHash<int, QPair<quint64, QImage>> m_pendingThumbs;
    quint64 m_acceptEpoch = 0;
    int m_dbgAccepted = 0;
    int m_dbgDropped  = 0;
    int m_dbgPending  = 0;
    int m_dbgRejected     = 0;
    int m_dbgEarlyReturn  = 0;
};
