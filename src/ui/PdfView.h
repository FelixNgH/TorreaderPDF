#pragma once
#include <QWidget>
#include <QImage>
#include <QPixmap>
#include <QPointF>
#include <QList>
#include <QRectF>
#include <QString>
#include <QTimer>

class PdfView : public QWidget {
    Q_OBJECT
public:
    enum class ViewTool { Pan, PlaceNote };
    enum class ViewMode { Single, Double };

    struct AnnotOverlay {
        int    pageIndex;
        QRectF pdfRect;   // in PDF coords (Y up)
        QString snippet;  // first 30 chars of note text
    };

    explicit PdfView(QWidget* parent = nullptr);

    // Called when navigating to a NEW page (resets pan).
    void setPage(int pageIndex, const QImage& pageImage, QSizeF pageSizePt);
    // Second page for Double page mode.
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
    // True once a page image has been received (used to choose setPage vs updatePageImage).
    bool     hasImage()    const { return !m_pixmap.isNull() && !m_loading; }

    QPointF widgetToPdf(const QPointF& wp) const;
    QPointF pdfToWidget(const QPointF& pp) const;
    QRectF  pdfRectToWidget(const QRectF& r) const;

signals:
    void zoomChanged(double scale);
    void scrolledToPage(int pageIndex);
    void noteRequested(int pageIndex, QPointF pdfPoint);
    void noteEditRequested(int pageIndex, int annotIndex);

protected:
    void paintEvent(QPaintEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void resizeEvent(QResizeEvent*) override;

private:
    QPointF pageOrigin() const;   // left page in single; left page in double
    QPointF page2Origin() const;  // right page in double mode

    QPixmap  m_pixmap;
    QPixmap  m_pixmap2;
    QSizeF   m_pageSizePt;
    QSizeF   m_pageSizePt2;
    int      m_pageIndex   = 0;
    int      m_pageIndex2  = -1;
    double   m_zoom        = 1.0;
    QPointF  m_panOffset;
    QPointF  m_lastMousePos;
    bool     m_panning     = false;
    bool     m_darkMode    = false;
    bool     m_loading     = false;
    ViewTool     m_tool        = ViewTool::Pan;
    ViewMode     m_viewMode    = ViewMode::Single;
    QList<QRectF>       m_highlights;
    QList<AnnotOverlay> m_annotOverlays;

    QTimer*  m_zoomTimer   = nullptr;
};
