#pragma once
#include <QOpenGLWidget>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLVertexArrayObject>
#include <QOpenGLBuffer>
#include <QImage>
#include <QPointF>
#include <QList>
#include <QRectF>
#include <QSizeF>
#include <QString>
#include <QTimer>
#include <QElapsedTimer>
#include <QMatrix4x4>
#include <QHash>
#include <QSet>
#include <QPair>
#include <QVector>
#include <QKeyEvent>

#include "annotations/AnnotationTypes.h"
#include "annotations/AnnotationManager.h"
#include "core/VectorLayer.h"
#include "core/ForeignAnnotLayer.h"
#include "core/PdfLinks.h"
#include "core/TextSelection.h"

class PdfDocument;
class QMouseEvent;

// GPU-accelerated PDF page viewer with viewport tiling.
// Renders the page as a low-res full-page background texture plus a grid of
// sharp 512×512 tile textures for the visible viewport region.
class PdfGpuView : public QOpenGLWidget, protected QOpenGLFunctions {
    Q_OBJECT
public:
    enum class ViewTool { Pan, PlaceNote, Line, Arrow, Rectangle, Ellipse, Cloud, FreeText, Freehand, Highlight, SelectText };
    enum class ViewMode { Single, Double };

    struct AnnotOverlay {
        int     pageIndex;
        QRectF  pdfRect;
        QString snippet;
        QString uid;
    };

    struct PendingMarkup {
        AnnotTool tool;
        QColor    color;
        float     width;
        QColor    fill;
        QPointF   a, b;
        QVector<QPointF> freehand;
    };

    explicit PdfGpuView(QWidget* parent = nullptr);
    ~PdfGpuView() override;

    void setPage(int pageIndex, const QImage& pageImage, QSizeF pageSizePt);
    void setSecondPage(int pageIndex, const QImage& pageImage, QSizeF pageSizePt);
    void updatePageImage(const QImage& pageImage);

    // Accept a partial (in-progress) render from ProgressiveRenderTask
    void showPartial(int page, double scale, QImage img);

    void setZoom(double scale);
    void centerPage();
    void setViewMode(ViewMode mode);
    void setDarkMode(bool dark);
    void beginLoading();
    void setPendingPage(int pageIndex, QSizeF pageSizePt);
    // Goc hop trang cua trang dang hien (CropBox.left, CropBox.bottom). Mac dinh (0,0).
    void setPageBoxOrigin(const QPointF& o) { m_pageBoxOrigin = o; }
    void setTool(ViewTool tool);
    void beginSignaturePick();
    void setHighlights(const QList<QRectF>& rects);
    void setHighlights(const QList<QRectF>& all, int currentIdx);
    void clearHighlights();

    void setAnnotVisuals(const QList<AnnotVisual>& visuals);
    void clearAnnotVisuals();
    void addPendingMarkup(AnnotTool tool, const AnnotStyle& style, QPointF a, QPointF b, const QVector<QPointF>& freehand = {});
    void clearPendingMarkups();
    void setSelectedAnnot(const QRectF& rectPdf);
    void clearSelectedAnnot();
    void setDragTarget(const QString& uid, const QString& ghostText,
                       float fontSizePt, const QColor& ghostColor);
    void clearDragTarget();
    void setVectorLayer(std::shared_ptr<VectorLayer> layer);
    void setForeignAnnotLayer(std::shared_ptr<ForeignAnnotLayer> layer);
    void setForeignAnnotRegion(int page, double scale, QRect regionPx, const QImage& img);
    void setDragNote(const QRectF& rPt);
    void clearDragState();

    // Insert or update a tile in the current view.
    void setTile(int page, double scale, int col, int row, const QImage& img);

    // Accept a sharp region overlay rendered at true zoom (reuses PdfRenderer::requestRegion path).
    void setRegion(int page, double scale, QRect regionPx, const QImage& img);
    void invalidateSharp();
    void invalidateTiles();
    void invalidateTileTextures();

    // Show a blurred placeholder (thumbnail) while the full render loads.
    void setPlaceholder(const QImage& img);

    QSize currentPageImageSize() const;

    double   zoom()        const { return m_zoom; }
    int      currentPage() const { return m_pageIndex; }
    ViewTool tool()        const { return m_tool; }
    ViewMode viewMode()    const { return m_viewMode; }
    bool     hasImage()    const { return m_hasImage && !m_loading; }

    QPointF widgetToPdf(const QPointF& wp) const;
    QPointF pdfToWidget(const QPointF& pp) const;
    QRectF  pdfRectToWidget(const QRectF& r) const;
    void    centerOnPageRect(const QRectF& rectDisp);

    // Tai lieu dung de doc link (set tu MainWindow luc tao tab). Khong so huu.
    void setLinksDocument(PdfDocument* doc) { m_linksDoc = doc; }

    // Nhay link noi bo xong: ve vien khung dich ~1 giay roi mo dan
    // (SPEC_PDF_LINKS muc 4). rectDisp o toa do hien thi cua trang hien tai.
    void flashRect(const QRectF& rectDisp);

    // Xoa lua chon chu (goi khi doi trang / doi tool).
    void    clearTextSelection();
    // Rect chon chu (toa do hien thi) push tu MainWindow (SPEC_TEXTSEL_ADOBE).
    void setSelectionRects(const QList<QRectF>& dispRects);
    void clearSelectionRects();

signals:
    void zoomChanged(double scale);
    void scrolledToPage(int pageIndex);
    void noteRequested(int pageIndex, QPointF pdfPoint);
    void noteEditRequested(int pageIndex, int annotIndex);
    void textRegionSelected(int pageIndex, QRectF pageRectPts, QPoint globalPos);
    void shapeCommitRequested(int pageIndex, AnnotTool tool, QPointF start, QPointF end);
    void textBoxRequested(int pageIndex, QRectF rectPdf);
    void annotationPickRequested(int pageIndex, QPointF pdfPoint);
    void annotationContextRequested(int pageIndex, QPointF pdfPoint, QPoint globalPos);
    void annotationMoveRequested(int pageIndex, double dx, double dy);
    void signatureRectPicked(int pageIndex, QRectF rectPt);
    void freehandCommitRequested(int pageIndex, const QVector<QPointF>& points);
    // Chon chu theo chi so ky tu (SPEC_TEXTSEL_ADOBE). MainWindow ghi vao
    // DocTab::textSel va day rect ve lai.
    void textSelectionChanged(int anchorPage, int anchorChar,
                              int focusPage, int focusChar);
    void textSelectionCleared();
    // Chuot phai dang co vung chon → menu "Copy text" (SPEC_TEXTSEL_ADOBE).
    void copySelectionRequested(QPoint globalPos);
    // Emitted after debounce (~120ms) when pan/zoom changes the visible region.
    void tilesNeeded(int page, double scale, QRect viewportPx);
    // Roi chuot qua link (SPEC_PDF_LINKS): chuoi rong = roi khoi link.
    void linkHovered(const QString& text);
    // Bam chuot trai vao link (chi khi tool la Pan/Select): tra ve link that.
    void linkActivated(int pageIndex, const PdfLink& link);

protected:
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void paintGL() override;
    void wheelEvent(QWheelEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void keyPressEvent(QKeyEvent*) override;

private:
    void uploadTexture(const QImage& img);
    QMatrix4x4 computeTransform() const;
    QMatrix4x4 vectorTransform() const;
    QPointF pageOrigin() const;

    // GL resources
    QOpenGLShaderProgram*      m_program = nullptr;
    QOpenGLVertexArrayObject   m_vao;
    QOpenGLBuffer              m_vbo;
    GLuint  m_texture     = 0;
    int     m_uTransform  = -1;
    int     m_uHasTex     = -1;
    int     m_uBgColor    = -1;

    // Vector overlay GL resources
    std::shared_ptr<VectorLayer> m_vecLayer;
    std::shared_ptr<ForeignAnnotLayer> m_fgnLayer;
    int    m_fgnRegPage  = -1;
    double m_fgnRegScale = 0.0;
    QRect  m_fgnRegRect;
    QImage m_fgnRegImg;
    GLuint  m_vecVao = 0, m_vecVboPos = 0, m_vecVboCol = 0, m_vecVboQuad = 0, m_vecVboWidth = 0, m_vecVboDepth = 0, m_vecVboClip = 0;
    int     m_vecUploadedPage = -1;
    QOpenGLShaderProgram* m_vecProg = nullptr;
    int     m_vecMvpLoc = -1;
    int     m_vecViewportLoc = -1;
    int     m_vecPxPerPtLoc = -1;
    QOpenGLShaderProgram* m_fillProg = nullptr;
    int     m_fillMvpLoc = -1;
    GLuint  m_fillVao = 0, m_fillVboPos = 0, m_fillVboCol = 0, m_fillVboDepth = 0, m_fillVboClip = 0;
    QOpenGLShaderProgram* m_tileProg = nullptr;
    int m_tileMvpLoc = -1, m_tileRectLoc = -1, m_tileDepthLoc = -1, m_tileTexLoc = -1;
    int m_tileIsAlphaLoc = -1, m_tileColorLoc = -1;
    QVector<GLuint> m_tileTexText;
    QVector<GLuint> m_tileTexImg;
    quint32 m_tileTexGen = 0xFFFFFFFFu;
    GLuint m_tileVao = 0;
    bool    shouldUseVectorOverlay() const;
    void    drawVectorOverlay();
    void    drawPageBase(bool pureVector);
    void    drawSharpRegion(QPainter& p, const QPointF& orig, bool pureVector);
    void    drawForeignAnnotLayers(QPainter& p, const QPointF& orig, double pw, double ph, bool pureVector);
    void    drawMarkupOverlay(QPainter& p, const QPointF& orig, bool pureVector);
    mutable bool m_vecLastOverlayState = false; // ponytail: tracks last shouldUseVectorOverlay result for logging
    bool    m_vecDrawLogged = false;             // ponytail: log first successful vector draw only
    double  m_lastTileLogZoom = -1;

    // Pending upload
    QImage  m_pendingImage;
    bool    m_textureDirty = false;
    int     m_texW = -1, m_texH = -1;

    // View state
    int     m_pageIndex   = 0;
    QSizeF  m_pageSizePt;
    QPointF m_pageBoxOrigin;   // (0,0) voi PDF thong thuong
    double  m_zoom        = 1.0;
    QPointF m_panOffset;
    double  m_flipAccum = 0.0;
    QElapsedTimer m_flipCooldown;
    QImage  m_lastImage;
    int     m_lastImagePage = -1;
    QImage  m_placeholder;  // thumbnail shown while full render loads
    bool    m_hasImage    = false;
    bool    m_loading     = false;
    bool    m_darkMode    = false;
    ViewTool m_tool       = ViewTool::Pan;
    ViewMode m_viewMode   = ViewMode::Single;

    // Mouse
    bool    m_panning      = false;
    QPointF m_lastMousePos;

    // Shape drawing
    bool    m_drawingShape = false;
    QPointF m_shapeStart;
    QPointF m_shapeEnd;

    // Text selection (Ctrl+drag)
    bool    m_selecting  = false;
    QPointF m_selStart;
    QPointF m_selEnd;

    // Annotation selection rect (markup move/resize)
    QRectF m_selRect;
    bool   m_hasSel = false;

    // ── Chon chu theo chi so ky tu (SPEC_TEXTSEL_ADOBE) ────────────────────
    bool    m_selDragging   = false;
    int     m_selAnchorPage = -1;
    int     m_selAnchorChar = -1;
    int     m_selFocusPage  = -1;
    int     m_selFocusChar  = -1;
    QList<QRectF> m_selRects;      // toa do hien thi de ve (mau xanh Adobe)
    QElapsedTimer m_clickClock;    // dem dblclick lien tiep (2=dup, 3=ba)
    bool m_clickValid = false;
    // Vung chon do nhay dup/ba tao ra: nha chuot phai GIU NGUYEN (khong mo rong).
    bool m_selClickGesture = false;

    // load=false (mac dinh): chi doc dem PageCache — mouseMove khong duoc nap.
    // load=true: duoc phep nap (mousePress / bat dau chon chu).
    bool resolvePageSpacePos(const QPointF& widgetPos, QPointF* pagePt,
                             bool load = false) const;
    void beginTextSelection(const QPointF& widgetPos, int clickCount);
    void updateTextSelectionFocus(const QPointF& widgetPos);
    void emitSelectionState();
    void clearTextSelectionInternal();
    void updateSelectCursor(const QPointF& widgetPos);

    // Signature pick mode
    bool    m_sigPickMode = false;
    bool    m_sigActive   = false;
    QPointF m_sigStart;
    QPointF m_sigEnd;

    // ── Link (SPEC_PDF_LINKS) ──────────────────────────────────────────────
    PdfDocument* m_linksDoc = nullptr;
    bool m_hoveringLink = false;
    void updateLinkHover(const QPointF& widgetPos);
    void onLinksReady(quintptr doc, int pageIndex);
    bool tryActivateLink(QMouseEvent* e);
    QPointF m_lastHoverPos;   // vi tri con tro lan mouseMove cuoi (de linksReady cap nhat)
    QTimer* m_flashTimer = nullptr;
    QElapsedTimer m_flashClock;
    QRectF  m_flashRect;

    // Drag-to-move annotation
    bool    m_draggingAnnot = false;
    QPointF m_dragStart;
    QRectF  m_dragOrigRect;
    QPointF m_dragPixelDelta;
    QString m_dragUid;
    QRectF  m_dragNoteRect;
    QPointF m_dragNoteOffsetPt;
    // Freehand drawing
    bool    m_drawingFreehand = false;
    QVector<QPointF> m_freehandPoints;
    QPointF m_freehandLastWidgetPt;

    // ponytail: most recent partial skipped during pan — flushed on pan-end
    QImage  m_pendingPartImg;
    double  m_pendingPartScale = 0.0;
    int     m_pendingPartPage  = -1;

    // Overlays (drawn via QPainter on top of GL)
    QList<AnnotVisual>      m_annotVisuals;
    QList<QRectF>           m_highlights;
    int                     m_currentHighlightIdx = -1;
    QVector<PendingMarkup>  m_pendingMarkups;

    QTimer* m_zoomTimer     = nullptr;

    // ── Tiling ────────────────────────────────────────────────────────────────
    QTimer* m_tileTimer    = nullptr;
    // Tiles for the current page/view: key = (col,row), value = rendered image.
    QHash<QPair<int,int>, QImage> m_tiles;
    int     m_tilePage     = -1;
    double  m_tileScale    = 0.0;
    int     m_sharpPage    = -1;
    double  m_sharpScale   = 0.0;
    QRect   m_sharpRegion;
    QImage  m_sharpImage;
    void requestTiles();
    void scheduleTiles();
};
