#pragma once
#include <QMainWindow>
#include <QTabWidget>
#include <QAction>
#include <QLineEdit>
#include <QList>
#include <memory>
#include "core/PdfDocument.h"
#include "core/PdfRenderer.h"
#include "core/TileCacheFile.h"
#include "core/ThumbnailRenderPool.h"
#include "core/TextSearch.h"
#include "PdfView.h"
#include "PdfGpuView.h"
#include "annotations/AnnotationManager.h"

class QMimeData;
class PdfEditor;
class ThumbnailPanel;
class ContinuousView;
class QSplitter;
class Translator;
class TranslationPopup;
class GoogleAuth;
class NotificationBar;

struct DocTab {
    std::unique_ptr<PdfDocument>       doc;
    std::unique_ptr<PdfRenderer>       renderer;
    std::unique_ptr<AnnotationManager> annotMgr;
    std::unique_ptr<TileCacheFile>        tileCache;
    std::unique_ptr<ThumbnailRenderPool>  thumbPool;
    PdfGpuView* view        = nullptr;
    int      currentPage    = 0;
    double   zoom           = 1.0;
    QMetaObject::Connection pageReadyConn;
    QMetaObject::Connection scrollConn;
    QList<AnnotInfo> annotCache;
    bool     annotCacheValid = false;
    QString  originalPath;        // real on-disk file — Save target & tab name source
    bool     dirty = false;       // has unsaved in-memory edits (working copy != original)
};

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    void openFile(const QString& path);

protected:
    void dragEnterEvent(QDragEnterEvent* e) override;
    void dropEvent(QDropEvent* e) override;
    void closeEvent(QCloseEvent* e) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void resizeEvent(QResizeEvent* e) override;

private slots:
    void onOpenFile();
    void onSaveFile();
    void onSaveAsFile();
    void onMergeFiles();
#ifdef TORREADER_ENABLE_SIGNER
    void onSignPdf();
#endif
    void onExtractAll();
    void onPrintFile();
    void onTabChanged(int idx);
    void onTabClose(int idx);
    void onPageChanged(int pageIndex);
    void onZoomChanged(double scale);
    void onTextRegionSelected(int pageIdx, QRectF rectPts, QPoint globalPos);

private:
    void setupActionBar();
    void applyTheme(bool dark);
    void syncSidebarToTab(int idx, bool forceRebuild = false);
    DocTab* currentTab() const;
    void showThumbnailContextMenu(int pageIndex, QPoint globalPos);
    void reloadTab(DocTab* t, const QString& filePath, const QString& tmpPath);
    void loadTabFile(DocTab* t, const QString& path);
    void updateTabDirty(DocTab* t);

    QTabWidget*      m_docTabs       = nullptr;
    QList<DocTab*>   m_openDocs;
    ThumbnailPanel*  m_thumbPanel    = nullptr;
    ContinuousView*  m_continuousView = nullptr;
    QSplitter*       m_splitter      = nullptr;

    QAction*   m_continuousAct = nullptr;
    QAction*   m_translateAct  = nullptr;
    QLineEdit* m_zoomEdit      = nullptr;
    bool       m_darkMode      = false;
    bool       m_continuousMode = false;

    std::unique_ptr<PdfEditor>  m_editor;
    TextSearch*                 m_textSearch    = nullptr;

    // Translation feature
    Translator*        m_translator  = nullptr;
    TranslationPopup*  m_transPopup  = nullptr;
    GoogleAuth*        m_googleAuth  = nullptr;
    NotificationBar*   m_notifBar    = nullptr;
    QPoint             m_lastTransPos;

    void repositionNotifBar();
};
