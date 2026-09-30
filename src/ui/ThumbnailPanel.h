#pragma once
#include <QWidget>
#include <QListWidget>
#include <QTreeWidget>
#include <QStackedWidget>
#include <QButtonGroup>
#include <QAtomicInt>
#include <QTimer>
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
class QDialog;
class QPlainTextEdit;

class ThumbCurrentPageDelegate;

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

    // 0927 LUOT 4 (VIEC A/C) - probe doc o sua chu thich trong sidebar.
    // CHI cho harness --sidebar-edit-probe; khong dung giong duong dung.
    struct CommentRowProbe {
        int     page = -1;
        int     indexInPage = -1;
        bool    isOwn = false;
        bool    exists = false;            // hang co that su trong list
        bool    hasEdit = false;           // co o nhap
        bool    editableInPlace = false;   // setReadOnly(false) = sua tai cho
        bool    displayOnly = false;       // true = che do CHI-HIEN-THI
        bool    hasExpandBtn = false;      // co nut expand mo popup
        QString text;                      // nguyen van ban hien ra (da them dau ...)
        bool    tooltipHasAllText = false;
        int     popupLines = -1;           // so dong trong QPlainTextEdit cua popup
        bool    popupOkEnabled = true;     // nut OK trong popup co bat hay khong
    };
    int  probeCommentRowCount() const;
    // row = -1 = de doi chon row dau tien. idxInPage dung de doi chieu voi
    // chi so doc duoc trong list (thuong khop nhau, nhung khong tu dong).
    CommentRowProbe probeCommentRow(int page, int indexInPage, int row = -1) const;
    // Mo popup theo DUNG DUONG GIONG NGUOI DUNG: goi dung nghia Qt
    // (QPushButton::click) tren nut expand, hoac keyPress Return/F2 len o.
    // false neu khong co gi de bam.
    bool probeOpenCommentPopup(int page, int indexInPage, int row = -1,
                               const QString& byButton = QString());
    // Dat chu MOI (nhieu dong, giu nguyen newline) vao QPlainTextEdit dang mo
    // roi accept() dung duong OK. false neu chua mo popup.
    bool probeSetPopupTextAndAccept(const QString& text);
    bool probeCommentPopupOpen() const { return m_cmtPopup != nullptr; }
    // 🔴 0927 LUOT 5 (LOI 3) - --sidebar-popup-hold: toa do popup DANG
    // mo (global) + toa do hang da bam (global), de harness in ra de CEO canh
    // vung chup man hinh. rGeometry() = trong mau = khi popup chua mo.
    bool probePopupGeometry(QRect* popupGlobal, QRect* rowGlobal) const;
    // Dong popup (khong ghi gi) - dung khi ket thuc gioi han giu popup.
    void closeCommentPopupIfOpen();

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
    // 0928 LƯỢT 15: `draft` = ảnh CHƯA render xong (quá trần thời gian worker).
    // Mặc định false để các lời gọi thử nghiệm trong main.cpp không phải sửa.
    void onPageReady(int pageIndex, const QImage& image, quint64 epoch, bool draft = false);
    // 🔴 2026-08-31: nhan anh trang DAY DU da render san, thu nho lam thumbnail.
    // Do duoc: thumbnail cua trang quai vat ton 11.942 ms — gan bang mot luot render day du,
    // vi PDFium phai DOC LAI toan bo 2,54 trieu doi tuong du anh ra chi 300px.
    // Anh day du DA co san => chi can scale, khoi doc lai.
    void acceptFromFullRender(int pageIndex, const QImage& fullImg);

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
    // Insert Image (SPEC_INSERT_IMAGE_2026-08-30): nguoi dung bam nut "Insert" trong
    // tab Comments — MainWindow mo hộp chon anh roi chen vao PDF thanh Stamp.
    void insertImageRequested();
    void commentActivated(int pageIndex, int annotIndex);
    void commentTextEdited(int page, int indexInPage, const QString& text);
    void annotStyleChanged(QColor color, double width, bool fill, int fillOpacityPct, double fontSize);
    void requestComments();
    void sidebarTabChanged(int newIndex);
    void lowResPageAvailable(int pageIndex, const QImage& img);

private:
    friend class ThumbCurrentPageDelegate;
    bool eventFilter(QObject* o, QEvent* e) override;
    // 0927 LUOT 4 - mo popup sua chu thich (nhieu dong). DUNG LAI
    // NoteInputDialog (QPlainTextEdit) - khong viet hop thu hai.
    void  openCommentPopup(QWidget* row, int page, int indexInPage, bool editable);
    void  closeCommentPopup();
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
    int currentPageIndex() const { return m_currentPage; }

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
    // 0928 LƯỢT 15b: gom 60 lần valueChanged của một cú cuộn bánh xe thành MỘT lần
    // đo, sau khi layout đã ngồi xuống. Không có debounce thì đo giữa lúc
    // `doItemsLayout` đang chạy ⇒ đọc vùng cũ.
    QTimer        m_scrollDebounce;
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
    // Popup sua chu thich: thuoc tinh rieng de giu con trong khi popup
    // dong. parent = chinh panel (khong phai hang) - QWidget cha khong giu
    // doi tuong con nen khong lo me tham chieu.
    QDialog*     m_cmtPopup     = nullptr;
    QPlainTextEdit* m_cmtPopupEdit = nullptr;
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
