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
#include <QMatrix4x4>

// GPU-accelerated PDF page viewer (drop-in replacement for PdfView).
// Renders page as an OpenGL texture — pan/zoom = matrix update only, no CPU re-render.
class PdfGpuView : public QOpenGLWidget, protected QOpenGLFunctions {
    Q_OBJECT
public:
    enum class ViewTool { Pan, PlaceNote };
    enum class ViewMode { Single, Double };  // Double is no-op (GPU view is single-mode)

    struct AnnotOverlay {
        int     pageIndex;
        QRectF  pdfRect;   // PDF coords (Y up)
        QString snippet;
    };

    explicit PdfGpuView(QWidget* parent = nullptr);
    ~PdfGpuView() override;

    // Called when navigating to a NEW page (resets pan).
    void setPage(int pageIndex, const QImage& pageImage, QSizeF pageSizePt);
    // Second page (no-op in GPU single-mode view).
    void setSecondPage(int pageIndex, const QImage& pageImage, QSizeF pageSizePt);
    // Called when a fresh render of the SAME page arrives (keeps pan).
    void updatePageImage(const QImage& pageImage);

    void setZoom(double scale);
    void setViewMode(ViewMode mode);
    void setDarkMode(bool dark);
    void beginLoading();
    void setTool(ViewTool tool);
    void setHighlights(const QList<QRectF>& rects);
    void clearHighlights();
    void setAnnotOverlays(const QList<AnnotOverlay>& overlays);
    void clearAnnotOverlays();

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

protected:
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void paintGL() override;
    void wheelEvent(QWheelEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;

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
    int     m_texW = 0, m_texH = 0;

    // View state
    int     m_pageIndex   = 0;
    QSizeF  m_pageSizePt;
    double  m_zoom        = 1.0;
    QPointF m_panOffset;
    bool    m_hasImage    = false;
    bool    m_loading     = false;
    bool    m_darkMode    = false;
    ViewTool m_tool       = ViewTool::Pan;
    ViewMode m_viewMode   = ViewMode::Single;

    // Mouse
    bool    m_panning      = false;
    QPointF m_lastMousePos;

    // Text selection (Ctrl+drag)
    bool    m_selecting  = false;
    QPointF m_selStart;
    QPointF m_selEnd;

    // Overlays (drawn via QPainter on top of GL)
    QList<QRectF>       m_highlights;
    QList<AnnotOverlay> m_annotOverlays;

    QTimer* m_zoomTimer = nullptr;
};
