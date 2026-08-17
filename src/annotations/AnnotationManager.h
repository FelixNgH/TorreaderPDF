#pragma once
#include <QObject>
#include <QString>
#include <QByteArray>
#include <QList>
#include <QRectF>
#include <QColor>
#include <QVector>
#include <QImage>
#include <QHash>
#include <QSet>
#include <atomic>
#include <fpdfview.h>
#include <fpdf_annot.h>

// Flat record for one annotation read from a PDF page.
struct AnnotInfo {
    int     pageIndex = 0;
    int     indexInPage = -1;
    QString type;      // "Note", "FreeText", "Highlight", "Underline", etc.
    QString text;      // /Contents
    QString author;    // /T
    QRectF  rect;      // in PDF points, Y upward
    QColor  color;
    bool isDraft = false;
    QString uid;
};

Q_DECLARE_METATYPE(AnnotInfo)

struct AnnotSnapshot {
    bool  valid = false;
    int   subtype = 0;
    float rl = 0, rt = 0, rr = 0, rb = 0;
    unsigned int r = 255, g = 0, b = 0, a = 255;
    bool hasColor = false;
    bool hasFill = false;
    unsigned int fr = 255, fg = 255, fb = 255, fa = 0;
    float border = 2.0f;
    bool isDraft = false;
    QString da;
    QString contents;
    QString uid;
    QVector<QVector<QPointF>> ink;
};

// Overlay annotation data — coordinates in display space (Y-down, rotation applied).
struct AnnotVisual {
    int      page = 0;
    QString  uid;
    int      subtype = 0;      // FPDF_ANNOT_*
    QRectF   rect;             // display coords (via pdfToDisp)
    QColor   stroke = QColor(Qt::red);
    QColor   fill = QColor(Qt::transparent);
    float    border = 2.0f;
    QVector<QVector<QPointF>> ink;    // INK: strokes, display coords
    QVector<QRectF>           quads;  // HIGHLIGHT: quad points, display coords
    QString  text;
    float    fontSize = 11.0f;
    bool     isNote = false;
    bool     hasColor = false;  // /C co trong annot dict
    bool     hasFill  = false;  // /IC co trong annot dict
    bool     hasAP    = false;  // /AP co trong annot dict
    // ponytail: FreeText/Note are drawn as page objects in renderer, not by overlay
    bool     paintByOverlay = true;
};

// Reads and creates annotations via PDFium.
// All operations are main-thread only (PDFium is single-threaded for writes).
class AnnotationManager : public QObject {
    Q_OBJECT
public:
    explicit AnnotationManager(QObject* parent = nullptr);
    ~AnnotationManager() override;

    void setDocument(FPDF_DOCUMENT doc, const QString& filePath);

    // Read all annotations from one page (fast, called per-page).
    QList<AnnotInfo> loadPage(int pageIndex);

    // Read all annotations across the whole document.
    QList<AnnotInfo> loadAll(int pageCount);

    // Stream annotations page-by-page, emitting pageAnnotsLoaded per non-empty page.
    void loadAllStreaming(int pageCount, int startPage = 0);

    // Read annotations as overlay visuals for one page.
    QList<AnnotVisual> loadPageVisuals(int page, bool* outOverlayCapable,
                                       bool* hasForeign = nullptr);

    // Build one AnnotVisual from an already-open annot. Returns false if not overlay-drawable.
    // Caller must hold s_pdfiumMutex and have `page` open.
    bool buildVisual(FPDF_PAGE page, FPDF_ANNOTATION annot, int pageIndex, AnnotVisual& out);

    // Create a sticky-note annotation (FPDF_ANNOT_TEXT) at a point on the page.
    // Saves the document to disk.
    bool createPopupNote(int pageIndex, QPointF pointPdf,
                         const QString& text, const QString& author);

    // Create a free-text annotation (FPDF_ANNOT_FREETEXT) over a rect on the page.
    // Saves the document to disk.
    bool createInlineNote(int pageIndex, QRectF rectPdf,
                          const QString& textIn, const QString& author,
                          bool withBackground = true,
                          QColor textColor = Qt::black,
                          float fontSize = 11.0f);

    // Update the Contents string of an existing annotation in place.
    // Saves the document to disk.
    bool updateNote(int pageIndex, int annotIndex, const QString& newText);

    bool removeAnnot(int pageIndex, int index);
    int removeNotePageObjects(int pageIndex, unsigned int noteId);
    bool setAnnotStyle(int pageIndex, int index, QColor color, float width, bool fill, int fillAlpha = 255);
    bool rebuildTextNote(int pageIndex, int index, QColor newColor, float newFontSize);
    int findAnnotIndexByUid(int pageIndex, const QString& uid);
    int findAnnotIndexByAnyUid(int pageIndex, const QString& uid);
    QString ensureExternalUid(int pageIndex, int index);
    bool setAnnotUid(int pageIndex, int index, const QString& uid);
    bool setAnnotContents(int pageIndex, int index, const QString& text);
    QString generateUid();
    bool retextNote(int pageIndex, int index, const QString& newText);

    // Di chuyen annot BAT KY loai nao: chi tinh tien, KHONG dung lai gi.
    // dxU/dyU la delta trong he PDF CHUA xoay (goi ben tu doi theo /Rotate).
    // Note cua ta (co TRID): dich luon page object cua no.
    // Annot ngoai / hinh khoi: chi dich /Rect, /AP giu nguyen.
    // INK (Freehand): snapshot + remove + add (vi PDFium khong cho sua InkList tai cho).
    bool moveAnnot(int pageIndex, int index, double dxU, double dyU);

    // Annot cua TorReader co TRUID (moi) hoac TRID (note cu). Khong co ca hai = cua phan mem khac.
    bool isOwnAnnot(int pageIndex, int index);

    // Dem so annot tren mot trang (nhanh hon loadPage vi khong parse tung cai).
    int annotCount(int pageIndex);

    AnnotSnapshot snapshotAnnot(int pageIndex, int index);
    bool addSnapshot(int pageIndex, const AnnotSnapshot& s);

    // Read-back for Properties dialog. Returns false if annot does not exist.
    bool getAnnotEditState(int pageIndex, int index, QString& outType,
                           QColor& outColor, float& outWidth, float& outFontSize,
                           bool* outHasFill = nullptr, int* outFillAlpha = nullptr);

    bool createSignatureDraft(int pageIndex, QRectF rectPt, const QString& text);
    QRectF findSignatureDraftRect(int pageIndex, int* outIndex);

    quint32 pageRevision(int page) const { return m_pageRev.value(page, 0); }
    void    bumpPageRevision(int page);

    void stopScan()  { m_stopScan.store(true); }
    void resetScan() { m_stopScan.store(false); }

    // Generate content for a single page (called just before save from deferred set).
    static int setOwnNoteObjectsActive(FPDF_PAGE page, bool active);

    void generateContentForPage(int page);

    void flushPendingGenerate(int page);
    void flushAllPendingGenerate();

    bool saveDocument();
    QString lastError() const { return m_lastError; }
    QString lastCreatedUid() const { return m_lastCreatedUid; }

    // Kept for --foreignbench headless benchmark (main.cpp). Not used by GUI.
    QImage buildForeignAnnotLayer(int pageIndex, int wPx, int hPx);

signals:
    void annotationAdded(int pageIndex, AnnotInfo info);
    void pageAnnotsLoaded(int pageIndex, QList<AnnotInfo> annots);
    void scanProgress(int pagesScanned, int totalPages);
    void pageContentChanged(int page);

private:

    FPDF_FONT     m_unicodeFont = nullptr;
    QByteArray    m_unicodeFontData;
    FPDF_FONT unicodeFont();

    // ⚠️ _locked: caller must hold s_pdfiumMutex and have `page` open.
    // No lock, no LoadPage/ClosePage, no GenerateContent, no emit.
    int  removeNotePageObjects_locked(FPDF_PAGE page, unsigned int noteId);
    int  translateNotePageObjects_locked(FPDF_PAGE page, int pageIndex,
                                         unsigned int noteId, double dx, double dy);
    bool objectHasNoteId(FPDF_PAGEOBJECT obj, unsigned int noteId);
    bool removeAnnot_locked(FPDF_PAGE page, int index, bool* outNeedsGen);
    bool createInlineNote_locked(FPDF_PAGE page, int pageIndex, QRectF rectPdf,
                                 const QString& textIn, const QString& author,
                                 bool withBackground, QColor textColor, float fontSize,
                                 AnnotInfo* outInfo);
    bool createPopupNote_locked(FPDF_PAGE page, int pageIndex, QPointF pointDisp,
                                const QString& text, const QString& author,
                                AnnotInfo* outInfo);

    // GIA DINH: ben goi DA giu s_pdfiumMutex. Khong duoc tu khoa.
    void flushGenerate_locked(int pageIndex);
    // GIA DINH: ben goi DA giu s_pdfiumMutex.
    void releaseSharedPage_locked();
    void invalidateNoteObjCache_locked(int pageIndex);

    std::atomic<bool> m_stopScan{false};
    QHash<int, quint32> m_pageRev;
    QSet<int> m_pendingGenerate;
    QSet<int> m_pendingGen;                  // trang co page object doi, chua sinh noi dung
    QHash<QPair<int,quint32>, QVector<int>> m_noteObjIdxCache;

    FPDF_DOCUMENT m_doc     = nullptr;
    QString       m_path;
    QString       m_lastError;
    unsigned int  m_nextNoteId = 1;
    QString m_lastCreatedUid;

public:
    // BO LOP DEM CŨ (m_pinLru + m_scratchPage) — thay bang PageCache chung
    // (SPEC_PAGECACHE_CORE_2026-08-16). Nhung phuong thuc nay giu nguyen ten de
    // MainWindow / harness cũ dung duoc; than noi duoi la PageCache::acquire /
    // forgetDocument. PageCache la chu so huu duy nhat cua FPDF_PAGE.
    bool isSharedPage(int pageIndex) const;
    FPDF_PAGE acquireSharedPage(int pageIndex);
    void pinPage(int pageIndex);
    void pinPage_locked(int pageIndex);
    // TU giu khoa. Chi goi tu noi KHONG giu khoa.
    void      releaseSharedPage();
};
