#include "PdfGpuView.h"
#include <QMouseEvent>
#include <QWheelEvent>
#include <QPainter>
#include <QDebug>

// Unit quad: position (x,y) in [-1,1] + texcoord (u,v) in [0,1].
// Row 0 of QImage = top of image → UV(0,0) = top-left of texture.
static const float kQuad[] = {
    -1.f, -1.f,  0.f, 0.f,   // top-left
     1.f, -1.f,  1.f, 0.f,   // top-right
    -1.f,  1.f,  0.f, 1.f,   // bottom-left
     1.f,  1.f,  1.f, 1.f,   // bottom-right
};

static const char* kVert = R"(
#version 330 core
layout(location=0) in vec2 aPos;
layout(location=1) in vec2 aUV;
out vec2 vUV;
uniform mat4 u_transform;
void main() {
    vUV = aUV;
    gl_Position = u_transform * vec4(aPos, 0.0, 1.0);
}
)";

static const char* kFrag = R"(
#version 330 core
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D u_tex;
uniform bool u_hasTex;
uniform vec4 u_bgColor;
void main() {
    if (!u_hasTex) { fragColor = u_bgColor; return; }
    // Page is opaque (white canvas background). Force alpha=1 so the gray
    // viewport clear color (glClearColor 80,80,80) can never bleed through
    // anti-aliased / partial-alpha pixels in the page texture. (bug F: gray veil)
    fragColor = vec4(texture(u_tex, vUV).rgb, 1.0);
}
)";

// ── Constructor / Destructor ──────────────────────────────────────────────────

PdfGpuView::PdfGpuView(QWidget* parent)
    : QOpenGLWidget(parent)
    , m_vbo(QOpenGLBuffer::VertexBuffer)
{
    m_zoomTimer = new QTimer(this);
    m_zoomTimer->setSingleShot(true);
    m_zoomTimer->setInterval(150);
    connect(m_zoomTimer, &QTimer::timeout, this, [this]{
        emit zoomChanged(m_zoom);
    });
}

PdfGpuView::~PdfGpuView() {
    // Clean up GL resources if context was initialized
    if (m_program) {
        makeCurrent();
        if (m_texture) { glDeleteTextures(1, &m_texture); m_texture = 0; }
        delete m_program; m_program = nullptr;
        m_vao.destroy();
        m_vbo.destroy();
        doneCurrent();
    }
}

// ── OpenGL lifecycle ──────────────────────────────────────────────────────────

void PdfGpuView::initializeGL() {
    initializeOpenGLFunctions();
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    m_program = new QOpenGLShaderProgram(this);
    m_program->addShaderFromSourceCode(QOpenGLShader::Vertex,   kVert);
    m_program->addShaderFromSourceCode(QOpenGLShader::Fragment, kFrag);
    m_program->link();

    m_program->bind();
    m_program->setUniformValue("u_tex", 0);
    m_uTransform = m_program->uniformLocation("u_transform");
    m_uHasTex    = m_program->uniformLocation("u_hasTex");
    m_uBgColor   = m_program->uniformLocation("u_bgColor");
    m_program->release();

    m_vao.create();
    m_vbo.create();
    m_vbo.setUsagePattern(QOpenGLBuffer::StaticDraw);

    {
        QOpenGLVertexArrayObject::Binder binder(&m_vao);
        m_vbo.bind();
        m_vbo.allocate(kQuad, sizeof(kQuad));
        // position: attrib 0, 2 floats, stride=4 floats, offset=0
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), nullptr);
        // texcoord: attrib 1, 2 floats, stride=4 floats, offset=2 floats
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                              reinterpret_cast<void*>(2 * sizeof(float)));
        m_vbo.release();
    }
}

void PdfGpuView::resizeGL(int, int) {
    update();
}

// ── Texture upload ────────────────────────────────────────────────────────────

void PdfGpuView::uploadTexture(const QImage& img) {
    QImage rgba = img.convertToFormat(QImage::Format_RGBA8888);
    if (!m_texture) glGenTextures(1, &m_texture);
    glBindTexture(GL_TEXTURE_2D, m_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, rgba.width(), rgba.height(),
                 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.constBits());
    m_texW = rgba.width();
    m_texH = rgba.height();
}

// ── Transform ─────────────────────────────────────────────────────────────────

QPointF PdfGpuView::pageOrigin() const {
    double pw = m_pageSizePt.width()  * m_zoom;
    double ph = m_pageSizePt.height() * m_zoom;
    return QPointF(width()  / 2.0 - pw / 2.0,
                   height() / 2.0 - ph / 2.0) + m_panOffset;
}

QMatrix4x4 PdfGpuView::computeTransform() const {
    double pw = m_pageSizePt.width()  * m_zoom;
    double ph = m_pageSizePt.height() * m_zoom;
    QPointF orig = pageOrigin();

    // Orthographic: pixel space, y-down (0,0 = top-left, width,height = bottom-right)
    // ortho(left, right, bottom, top, near, far)
    QMatrix4x4 m;
    m.ortho(0.f, (float)width(), (float)height(), 0.f, -1.f, 1.f);
    // Place unit quad at page rect center in pixel space
    m.translate((float)(orig.x() + pw * 0.5), (float)(orig.y() + ph * 0.5), 0.f);
    m.scale((float)(pw * 0.5), (float)(ph * 0.5), 1.f);
    return m;
}

// ── Paint ─────────────────────────────────────────────────────────────────────

void PdfGpuView::paintGL() {
    // Upload pending texture (must happen inside GL context)
    if (m_textureDirty && !m_pendingImage.isNull()) {
        uploadTexture(m_pendingImage);
        m_pendingImage  = {};
        m_textureDirty  = false;
    }

    // Clear background
    QColor bg = m_darkMode ? QColor(30, 30, 30) : QColor(80, 80, 80);
    glClearColor((float)bg.redF(), (float)bg.greenF(), (float)bg.blueF(), 1.f);
    glClear(GL_COLOR_BUFFER_BIT);

    // Draw page texture
    if (m_hasImage && !m_texture)
        qDebug() << "[GpuView] WARN: hasImage=true but texture=0, page" << m_pageIndex;
    if (m_hasImage && m_pageSizePt.isEmpty())
        qDebug() << "[GpuView] WARN: hasImage=true but pageSizePt is EMPTY, page" << m_pageIndex;
    if (m_hasImage && m_texture && !m_pageSizePt.isEmpty()) {
        // Drop shadow via filled quad slightly offset (draw under page)
        // (Drawn via QPainter below to keep GL code minimal)

        m_program->bind();
        QMatrix4x4 transform = computeTransform();
        glUniformMatrix4fv(m_uTransform, 1, GL_FALSE, transform.constData());
        glUniform1i(m_uHasTex, 1);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, m_texture);
        {
            QOpenGLVertexArrayObject::Binder binder(&m_vao);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        }
        m_program->release();
    }

    // Overlays via QPainter (drawn on top of GL — valid inside paintGL for QOpenGLWidget)
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    if (!m_hasImage && !m_loading) {
        p.setPen(QColor(200, 200, 200));
        QFont f = p.font(); f.setPointSize(13); p.setFont(f);
        p.drawText(rect(), Qt::AlignCenter,
                   "TorReader PDF\n\nOpen a PDF to get started\n"
                   "File → Open   or   drag & drop");
        return;
    }

    if (m_hasImage && !m_pageSizePt.isEmpty()) {
        // Drop shadow — only the strips that extend BEYOND the page's right and
        // bottom edges. A full-page rect here would cover the entire page (QPainter
        // draws on top of the GL texture) and dim every page with a grey veil.
        double pw = m_pageSizePt.width()  * m_zoom;
        double ph = m_pageSizePt.height() * m_zoom;
        QPointF orig = pageOrigin();
        const double sh = 4.0; // shadow thickness
        p.fillRect(QRectF(orig.x() + pw, orig.y() + sh, sh, ph), QColor(0, 0, 0, 80)); // right
        p.fillRect(QRectF(orig.x() + sh, orig.y() + ph, pw, sh), QColor(0, 0, 0, 80)); // bottom

        // Highlights (PDF coords, Y up → convert)
        if (!m_highlights.isEmpty()) {
            p.save();
            p.setBrush(QColor(255, 220, 0, 120));
            p.setPen(QPen(QColor(255, 150, 0, 200), 1.5));
            double pageH = m_pageSizePt.height();
            for (const QRectF& r : m_highlights) {
                QRectF nr = r.normalized();
                if (nr.width() < 0.5 || nr.height() < 0.5) continue;
                double wx = orig.x() + nr.x()          * m_zoom;
                double wy = orig.y() + (pageH - nr.bottom()) * m_zoom;
                p.drawRect(QRectF(wx, wy, nr.width() * m_zoom, nr.height() * m_zoom));
            }
            p.restore();
        }

        // Annotation overlays
        if (!m_annotOverlays.isEmpty()) {
            double pageH = m_pageSizePt.height();
            p.save();
            for (const auto& ov : m_annotOverlays) {
                if (ov.pageIndex != m_pageIndex) continue;
                QRectF nr = ov.pdfRect.normalized();
                double cx = orig.x() + (nr.x() + nr.width()  / 2) * m_zoom;
                double cy = orig.y() + (pageH - nr.y() - nr.height() / 2) * m_zoom;
                QRectF badge(cx - 14, cy - 14, 28, 28);
                p.setBrush(QColor(245, 158, 11, 220));
                p.setPen(QColor(180, 100, 0, 200));
                p.drawRoundedRect(badge, 6, 6);
                p.setPen(Qt::white);
                QFont f = p.font(); f.setPointSize(10); f.setBold(true); p.setFont(f);
                p.drawText(badge, Qt::AlignCenter, "N");
                if (!ov.snippet.isEmpty()) {
                    p.setPen(QColor(50, 50, 50));
                    QFont sf = p.font(); sf.setPointSize(8); sf.setBold(false); p.setFont(sf);
                    p.drawText(QRectF(cx - 60, cy + 16, 120, 20), Qt::AlignCenter, ov.snippet);
                }
            }
            p.restore();
        }
    }

    // Text selection overlay (Ctrl+drag)
    if (m_selecting) {
        QRectF sel = QRectF(m_selStart, m_selEnd).normalized();
        p.setRenderHint(QPainter::Antialiasing, false);
        p.fillRect(sel, QColor(0, 120, 255, 50));
        p.setPen(QPen(QColor(0, 100, 220, 200), 1));
        p.drawRect(sel);
    }

    // Show loading overlay only when no existing image — keeps old page visible
    // while new page is rendering (no grey flash on navigation).
    if (m_loading && !m_hasImage) {
        p.fillRect(rect(), QColor(0, 0, 0, 60));
        p.setPen(Qt::white);
        QFont f = p.font(); f.setPointSize(12); p.setFont(f);
        p.drawText(rect(), Qt::AlignCenter, "Loading…");
    }
}

// ── Page management ───────────────────────────────────────────────────────────

void PdfGpuView::setPage(int pageIndex, const QImage& pageImage, QSizeF pageSizePt) {
    // Treat first load (!m_hasImage) same as new page so pan resets properly.
    bool newPage = (pageIndex != m_pageIndex) || !m_hasImage;
    qDebug() << "[GpuView] setPage idx=" << pageIndex
             << "newPage=" << newPage
             << "imgSize=" << pageImage.size()
             << "pageSizePt=" << pageSizePt
             << "loading=" << m_loading;
    m_pageIndex  = pageIndex;
    m_pageSizePt = pageSizePt;
    m_loading    = false;
    m_hasImage   = !pageImage.isNull();
    if (!pageImage.isNull()) {
        m_pendingImage = pageImage;
        m_textureDirty = true;
    }
    if (newPage) {
        m_panOffset = {};
        m_highlights.clear();
    }
    update();
}

void PdfGpuView::setSecondPage(int, const QImage&, QSizeF) {
    // GPU view is single-mode only
}

void PdfGpuView::updatePageImage(const QImage& pageImage) {
    m_loading  = false;
    m_hasImage = !pageImage.isNull();
    if (!pageImage.isNull()) {
        m_pendingImage = pageImage;
        m_textureDirty = true;
    }
    update();
}

void PdfGpuView::beginLoading() {
    qDebug() << "[GpuView] beginLoading page=" << m_pageIndex
             << "hasImage=" << m_hasImage
             << "pageSizePt=" << m_pageSizePt;
    m_loading = true;
    if (!m_hasImage) update();
}

void PdfGpuView::setZoom(double scale) {
    m_zoom = qBound(0.1, scale, 10.0);
    update();
}

void PdfGpuView::setViewMode(ViewMode mode) {
    m_viewMode = mode;
    update();
}

void PdfGpuView::setDarkMode(bool dark) {
    m_darkMode = dark;
    update();
}

void PdfGpuView::setTool(ViewTool tool) {
    m_tool = tool;
    setCursor(tool == ViewTool::PlaceNote ? Qt::CrossCursor : Qt::ArrowCursor);
}

void PdfGpuView::setHighlights(const QList<QRectF>& rects) {
    m_highlights = rects;
    update();
}

void PdfGpuView::clearHighlights() {
    m_highlights.clear();
    update();
}

void PdfGpuView::setAnnotOverlays(const QList<AnnotOverlay>& overlays) {
    m_annotOverlays = overlays;
    update();
}

void PdfGpuView::clearAnnotOverlays() {
    m_annotOverlays.clear();
    update();
}

// ── Coordinate helpers ────────────────────────────────────────────────────────

QPointF PdfGpuView::widgetToPdf(const QPointF& wp) const {
    return (wp - pageOrigin()) / m_zoom;
}

QPointF PdfGpuView::pdfToWidget(const QPointF& pp) const {
    return pp * m_zoom + pageOrigin();
}

QRectF PdfGpuView::pdfRectToWidget(const QRectF& r) const {
    return QRectF(pdfToWidget(r.topLeft()), pdfToWidget(r.bottomRight()));
}

// ── Input ─────────────────────────────────────────────────────────────────────

void PdfGpuView::wheelEvent(QWheelEvent* e) {
    if (e->modifiers() & Qt::ControlModifier) {
        double delta   = e->angleDelta().y() / 1200.0;
        double newZoom = qBound(0.1, m_zoom + delta, 10.0);
        if (qAbs(newZoom - m_zoom) < 1e-6) return;

        QPointF cursorPdf = widgetToPdf(e->position());
        m_zoom = newZoom;
        QPointF newWidget = pdfToWidget(cursorPdf);
        m_panOffset += e->position() - newWidget;

        update();
        m_zoomTimer->start();
    } else {
        double dy = e->angleDelta().y() * 0.6;
        m_panOffset.ry() += dy;

        double pageHpx = m_pageSizePt.height() * m_zoom;
        // Minimum margin: 12% of viewport height (≈ 1-2 mouse wheel notches).
        // This ensures the user can always scroll a meaningful amount before
        // switching pages — critical when auto-fit zoom makes pageHpx ≈ height().
        double margin  = qMax(height() * 0.12,
                              (pageHpx > height())
                                  ? (pageHpx - height()) / 2.0 + height() * 0.05
                                  : pageHpx * 0.12);

        if (m_panOffset.y() > margin) {
            m_panOffset.setY(0.0);
            emit scrolledToPage(m_pageIndex - 1);
        } else if (m_panOffset.y() < -margin) {
            m_panOffset.setY(0.0);
            emit scrolledToPage(m_pageIndex + 1);
        }
        update();
    }
}

void PdfGpuView::mousePressEvent(QMouseEvent* e) {
    if (m_tool == ViewTool::PlaceNote && e->button() == Qt::LeftButton) {
        if (m_hasImage)
            emit noteRequested(m_pageIndex, widgetToPdf(e->position()));
        return;
    }
    // Ctrl+Left drag = text selection for translation
    if ((e->modifiers() & Qt::ControlModifier) && e->button() == Qt::LeftButton) {
        m_selecting = true;
        m_selStart  = e->position();
        m_selEnd    = e->position();
        setCursor(Qt::IBeamCursor);
        update();
        return;
    }
    if (e->button() == Qt::LeftButton || e->button() == Qt::MiddleButton) {
        m_panning      = true;
        m_lastMousePos = e->position();
        setCursor(Qt::ClosedHandCursor);
    }
}

void PdfGpuView::mouseMoveEvent(QMouseEvent* e) {
    if (m_selecting) {
        m_selEnd = e->position();
        update();
        return;
    }
    if (m_panning) {
        m_panOffset   += e->position() - m_lastMousePos;
        m_lastMousePos = e->position();
        update();
    }
}

void PdfGpuView::mouseReleaseEvent(QMouseEvent* e) {
    if (m_selecting && e->button() == Qt::LeftButton) {
        m_selecting = false;
        setCursor(m_tool == ViewTool::PlaceNote ? Qt::CrossCursor : Qt::ArrowCursor);

        QPointF p0(qMin(m_selStart.x(), m_selEnd.x()), qMin(m_selStart.y(), m_selEnd.y()));
        QPointF p1(qMax(m_selStart.x(), m_selEnd.x()), qMax(m_selStart.y(), m_selEnd.y()));

        // Ensure minimum height so a horizontal drag still captures a line of text
        const double kMinH = 18.0 * m_zoom;
        if (p1.y() - p0.y() < kMinH) {
            double mid = (p0.y() + p1.y()) / 2.0;
            p0.setY(mid - kMinH / 2.0);
            p1.setY(mid + kMinH / 2.0);
        }

        update();

        if (m_hasImage && !m_pageSizePt.isEmpty() && (p1.x() - p0.x()) > 2.0) {
            // widgetToPdf: (widget - pageOrigin) / zoom  →  (x from page-left, y from page-top)
            // PDF coords: y=0 at bottom, increasing upward
            QPointF pdf0 = widgetToPdf(p0);
            QPointF pdf1 = widgetToPdf(p1);
            double  pageH = m_pageSizePt.height();
            // Flip y: pdf_y = pageH - widget_y_from_top
            double pdfYtop = pageH - pdf0.y();  // larger PDF y (upper edge)
            double pdfYbot = pageH - pdf1.y();  // smaller PDF y (lower edge)
            // QRectF: x=left, y=smaller PDF y (.top()), w, h — matches MainWindow handler
            QRectF pageRect(pdf0.x(), pdfYbot, pdf1.x() - pdf0.x(), pdfYtop - pdfYbot);
            emit textRegionSelected(m_pageIndex, pageRect, e->globalPosition().toPoint());
        }
        return;
    }
    m_panning = false;
    setCursor(m_tool == ViewTool::PlaceNote ? Qt::CrossCursor : Qt::ArrowCursor);
}
