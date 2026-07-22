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
#include <QKeyEvent>

#include "annotations/AnnotationTypes.h"

// GPU-accelerated PDF page viewer with viewport tiling.
// Renders the page as a low-res full-page background texture plus a grid of
// sharp 512×512 tile textures for the visible viewport region.
class PdfGpuView : public QOpenGLWidget, protected QOpenGLFunctions {
    Q_OBJECT
public:
    enum class ViewTool { Pan, PlaceNote, Line, Arrow, Rectangle, Ellipse, Cloud, FreeText };
    enum class ViewMode { Single, Double };

    struct AnnotOverlay {
        int     pageIndex;
        QRectF  pdfRect;
        QString snippet;
    };

    explicit PdfGpuView(QWidget* parent = nullptr);
    ~PdfGpuView() override;

    void setPage(int pageIndex, const QImage& pageImage, QSizeF pageSizePt);
    void setSecondPage(int pageIndex, const QImage& pageImage, QSizeF pageSizePt);
    void updatePageImage(const QImage& pageImage);

    // Accept a partial (in-progress) render from ProgressiveRenderTask
    void showPartial(int page, double scale, QImage img);

    void setZoom(double scale);
    void setViewMode(ViewMode mode);
    void setDarkMode(bool dark);
    void beginLoading();
    void setPendingPage(int pageIndex, QSizeF pageSizePt);
    void setTool(ViewTool tool);
    void beginSignaturePick();
    void setHighlights(const QList<QRectF>& rects);
    void clearHighlights();
    void setAnnotOverlays(const QList<AnnotOverlay>& overlays);
    void clearAnnotOverlays();
    void setSelectedAnnot(const QRectF& rectPdf);
    void clearSelectedAnnot();

    // Insert or update a tile in the current view.
    void setTile(int page, double scale, int col, int row, const QImage& img);

    // Show a blurred placeholder (thumbnail) while the full render loads.
    void setPlaceholder(const QImage& img);

    double   zoom()        const { return m_zoom; }
    int      currentPage() const { return m_pageIndex; }
    ViewTool tool()        const { return m_tool; }
    ViewMode viewMode()    const { return m_viewMode; }
    bool     hasImage()    const { return m_hasImage && !m_loading; }

    QPointF widgetToPdf(const QPointF& wp) const;
    QPointF pdfToWidget(const QPointF& pp) const;
    QRectF  pdfRectToWidget(const QRectF& r) const;

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
    // Emitted after debounce (~120ms) when pan/zoom changes the visible region.
    void tilesNeeded(int page, double scale, QRect viewportPx);

protected:
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void paintGL() override;
    void wheelEvent(QWheelEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void keyPressEvent(QKeyEvent*) override;

private:
    void uploadTexture(const QImage& img);
    QMatrix4x4 computeTransform() const;
    QPointF pageOrigin() const;

    // GL resources
    QOpenGLShaderProgram*      m_program = nullptr;
    QOpenGLVertexArrayObject   m_vao;
    QOpenGLBuffer              m_vbo;
    GLuint  m_texture     = 0;
    int     m_uTransform  = -1;
    int     m_uHasTex     = -1;
    int     m_uBgColor    = -1;

    // Pending upload
    QImage  m_pendingImage;
    bool    m_textureDirty = false;
    int     m_texW = -1, m_texH = -1;

    // View state
    int     m_pageIndex   = 0;
    QSizeF  m_pageSizePt;
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

    // Selection
    QRectF m_selRect;
    bool   m_hasSel = false;

    // Signature pick mode
    bool    m_sigPickMode = false;
    bool    m_sigActive   = false;
    QPointF m_sigStart;
    QPointF m_sigEnd;

    // Drag-to-move annotation
    bool    m_draggingAnnot = false;
    QPointF m_dragStart;
    QRectF  m_dragOrigRect;

    // ponytail: most recent partial skipped during pan — flushed on pan-end
    QImage  m_pendingPartImg;
    double  m_pendingPartScale = 0.0;
    int     m_pendingPartPage  = -1;

    // Overlays (drawn via QPainter on top of GL)
    QList<QRectF>           m_highlights;
    QList<AnnotOverlay>     m_annotOverlays;

    QTimer* m_zoomTimer     = nullptr;

    // ── Tiling ────────────────────────────────────────────────────────────────
    QTimer* m_tileTimer    = nullptr;
    // Tiles for the current page/view: key = (col,row), value = rendered image.
    QHash<QPair<int,int>, QImage> m_tiles;
    int     m_tilePage     = -1;
    double  m_tileScale    = 0.0;
    void requestTiles();
};
