#include "PdfGpuView.h"
#include <QMouseEvent>
#include <QWheelEvent>
#include <QPainter>
#include <QDebug>

// ponytail: max live tiles = 120 (~30 MB at 512×512 RGBA). Prevents unbounded
// accumulation during pan; farthest-from-viewport tiles evicted when exceeded.
static constexpr int kMaxLiveTiles = 120;

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

    m_tileTimer = new QTimer(this);
    m_tileTimer->setSingleShot(true);
    m_tileTimer->setInterval(120);
    connect(m_tileTimer, &QTimer::timeout, this, &PdfGpuView::requestTiles);

    setFocusPolicy(Qt::StrongFocus);
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
    QElapsedTimer _tt; _tt.start();
    if (!m_texture) glGenTextures(1, &m_texture);
    glBindTexture(GL_TEXTURE_2D, m_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    int w = img.width(), h = img.height();
    GLint rowLen = img.bytesPerLine() / 4;
    if (rowLen != w)
        glPixelStorei(GL_UNPACK_ROW_LENGTH, rowLen);

    if (w == m_texW && h == m_texH) {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_BGRA, GL_UNSIGNED_BYTE, img.constBits());
    } else {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_BGRA, GL_UNSIGNED_BYTE, img.constBits());
        m_texW = w;
        m_texH = h;
    }

    if (rowLen != w)
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

    qDebug().noquote() << QString("[perf] texUpload ms=%1 w=%2 h=%3")
                              .arg(_tt.elapsed(), 3).arg(w).arg(h);
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
    QElapsedTimer _pf; _pf.start();
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
        qDebug() << "[GpuView] WARN: hasImage=true but pageSizePt is EMPTY (page index" << m_pageIndex << ") — chính path không set kích thước trang";
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
    {
        bool _needQP = (!m_hasImage && !m_loading) ||
            m_selecting || m_drawingShape ||
            (m_sigPickMode && m_sigActive) ||
            (m_loading && !m_hasImage) ||
            (m_hasImage && !m_pageSizePt.isEmpty());
        if (!_needQP) {
            qint64 _pms = _pf.elapsed();
            if (_pms >= 16)
                qDebug() << "[perf] paint ms=" << _pms << "zoom=" << m_zoom
                         << "tiles=" << m_tiles.size()
                         << "annots=" << m_annotOverlays.size()
                         << "panning=" << m_panning;
            return;
        }
    }

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
        double pw = m_pageSizePt.width()  * m_zoom;
        double ph = m_pageSizePt.height() * m_zoom;
        QPointF orig = pageOrigin();
        const double sh = 4.0;
        p.fillRect(QRectF(orig.x() + pw, orig.y() + sh, sh, ph), QColor(0, 0, 0, 80));
        p.fillRect(QRectF(orig.x() + sh, orig.y() + ph, pw, sh), QColor(0, 0, 0, 80));

        // ── Draw tile overlays (sharp textures on top of lo-res background) ──
        if (m_tilePage == m_pageIndex && qAbs(m_tileScale - m_zoom) < 1e-6) {
            const QRectF viewRect = rect();
            p.save();
            p.setRenderHint(QPainter::Antialiasing, false);
            p.setRenderHint(QPainter::SmoothPixmapTransform, false);
            for (auto it = m_tiles.constBegin(); it != m_tiles.constEnd(); ++it) {
                int col = it.key().first;
                int row = it.key().second;
                const qreal tx = orig.x() + col * 512.0;
                const qreal ty = orig.y() + row * 512.0;
                if (!viewRect.intersects(QRectF(tx, ty, 512.0, 512.0)))
                    continue;
                p.drawImage(QPoint(qRound(tx), qRound(ty)), it.value());
            }
            p.restore();
        }

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

        // Selection rectangle
        if (m_hasSel) {
            QPointF a = pdfToWidget(m_selRect.topLeft());
            QPointF b = pdfToWidget(m_selRect.bottomRight());
            QRectF wr = QRectF(a, b).normalized().adjusted(-3, -3, 3, 3);
            p.setRenderHint(QPainter::Antialiasing, false);
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(QColor(0, 120, 215), 1.5, Qt::DashLine));
            p.drawRect(wr);
        }

        // Annotation overlays (sticky note badges only)
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

    // Shape preview while dragging
    if (m_drawingShape) {
        p.save();
        QPen dashPen(Qt::blue, 2, Qt::DashLine);
        p.setPen(dashPen);
        p.setBrush(Qt::NoBrush);
        QRectF sr(m_shapeStart, m_shapeEnd);
        if (m_tool == ViewTool::Line || m_tool == ViewTool::Arrow) {
            p.drawLine(m_shapeStart, m_shapeEnd);
        } else if (m_tool == ViewTool::Rectangle || m_tool == ViewTool::FreeText || m_tool == ViewTool::Cloud) {
            p.drawRect(sr.normalized());
        } else if (m_tool == ViewTool::Ellipse) {
            p.drawEllipse(sr.normalized());
        }
        p.restore();
    }

    // Signature rubber-band
    if (m_sigPickMode && m_sigActive) {
        p.setPen(QPen(QColor(0, 90, 200), 1, Qt::DashLine));
        p.setBrush(QColor(0, 90, 200, 40));
        p.drawRect(QRectF(m_sigStart, m_sigEnd));
        p.setBrush(Qt::NoBrush);
    }

    // Show loading indicator on ANY state: even when a partial image exists,
    // keep the "Loading…" overlay so the user knows rendering is in progress.
    if (m_loading) {
        if (!m_hasImage) {
            if (!m_placeholder.isNull() && !m_pageSizePt.isEmpty()) {
                // Draw thumbnail placeholder scaled to page rect
                double pw = m_pageSizePt.width() * m_zoom;
                double ph = m_pageSizePt.height() * m_zoom;
                QPointF orig = pageOrigin();
                QRectF pageRect(orig.x(), orig.y(), pw, ph);
                p.fillRect(pageRect, Qt::white);
                p.drawImage(pageRect, m_placeholder);
                qDebug() << "[perf] placeholder shown page=" << m_pageIndex << "source=thumb";
            } else if (!m_pageSizePt.isEmpty()) {
                // No thumbnail — draw blank page rect
                double pw = m_pageSizePt.width() * m_zoom;
                double ph = m_pageSizePt.height() * m_zoom;
                QPointF orig = pageOrigin();
                p.fillRect(QRectF(orig.x(), orig.y(), pw, ph), Qt::white);
                qDebug() << "[perf] placeholder shown page=" << m_pageIndex << "source=blank";
            } else if (m_pageSizePt.isEmpty()) {
                p.fillRect(rect(), QColor(0, 0, 0, 80));
            }
        }
        QFont f = p.font(); f.setPointSize(26); f.setBold(true); p.setFont(f);
        p.setPen(QPen(QColor(255, 255, 255), 2));
        p.drawText(rect(), Qt::AlignCenter, "Loading…");
    }

    qint64 _pms = _pf.elapsed();
    if (_pms >= 16)
        qDebug() << "[perf] paint ms=" << _pms << "zoom=" << m_zoom
                 << "tiles=" << m_tiles.size()
                 << "annots=" << m_annotOverlays.size()
                 << "panning=" << m_panning;
}

// ── Page management ───────────────────────────────────────────────────────────

void PdfGpuView::setPage(int pageIndex, const QImage& pageImage, QSizeF pageSizePt) {
    bool newPage = (pageIndex != m_pageIndex) || !m_hasImage;
    // Low-res guard: only skip when the incoming image is from the SAME page
    // as the last cached image, AND is significantly smaller (= blurry).
    // The tolerance (0.9) prevents 1px rounding differences (e.g. 3999 vs 4000)
    // from triggering the guard — that was the original bug: setPendingPage
    // had already set m_pageIndex to the new page, making newPage=false even
    // for a cross-page jump, and 3999 < 4000 caused a silent discard.
    bool samePage = (pageIndex == m_lastImagePage);
    if (samePage && m_hasImage && !pageImage.isNull() &&
        !m_lastImage.isNull() && pageImage.width() < m_lastImage.width() * 0.9) {
        qDebug() << "[GpuView] setPage SKIP lowres page=" << pageIndex
                 << "newW=" << pageImage.width() << "lastW=" << m_lastImage.width()
                 << "lastPage=" << m_lastImagePage;
        return;
    }
    // Skip if image content is identical (same cacheKey/serial) — avoids
    // redundant texUpload from cache-hit pageReady after setPage already ran.
    if (samePage && !pageImage.isNull() && !m_lastImage.isNull() &&
        pageImage.cacheKey() == m_lastImage.cacheKey() &&
        pageImage.size() == m_lastImage.size()) {
        qDebug().noquote() << "[GpuView] setPage skip duplicate cacheKey page=" << pageIndex
                 << "key=" << pageImage.cacheKey()
                 << "lastKey=" << m_lastImage.cacheKey()
                 << "size=" << pageImage.size();
        return;
    }
    // Auto-fix empty pageSizePt from image dimensions (progressive partial
    // may arrive before PdfDocument has the size).
    if (pageSizePt.isEmpty() && !pageImage.isNull()) {
        pageSizePt = QSizeF(pageImage.width(), pageImage.height());
        qDebug() << "[GpuView] auto-fix pageSizePt from image page=" << pageIndex;
    }
    qDebug() << "[GpuView] setPage idx=" << pageIndex
             << "newPage=" << newPage
             << "imgSize=" << pageImage.size()
             << "pageSizePt=" << pageSizePt
             << "loading=" << m_loading;
    { static const bool dump = qEnvironmentVariableIsSet("TORREADER_DUMP");
      if (dump && !pageImage.isNull()) {
          QString fn = QString("gpuview_dump_p%1.png").arg(pageIndex);
          pageImage.save(fn);
          qDebug() << "[dump] saved" << fn << "w=" << pageImage.width() << "h=" << pageImage.height();
      }
    }
    m_pageIndex  = pageIndex;
    m_pageSizePt = pageSizePt;
    m_loading    = false;
    m_hasImage   = !pageImage.isNull();
    m_placeholder = {};  // clear placeholder now that we have the real image
    if (!pageImage.isNull()) {
        m_lastImage = pageImage;
        m_lastImagePage = pageIndex;
        // Full-resolution render — no cap, no tile dependency
        m_pendingImage = pageImage;
        m_textureDirty = true;
    }
    if (newPage) {
        m_panOffset = {};
        m_highlights.clear();
        m_hasSel = false;
        m_tiles.clear();
        m_tilePage = -1;
        m_tileScale = 0.0;
        m_tileTimer->start();
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

void PdfGpuView::setPlaceholder(const QImage& img) {
    m_placeholder = img;
    update();
}

void PdfGpuView::showPartial(int page, double scale, QImage img) {
    if (page != m_pageIndex) {
        qDebug() << "[GpuView] showPartial SKIP wrong page=" << page << "current=" << m_pageIndex;
        return;
    }
    if (img.isNull()) {
        qDebug() << "[GpuView] showPartial SKIP null image page=" << page;
        return;
    }
    // Only replace if resolution is higher or we have no image yet
    if (m_hasImage && !m_lastImage.isNull() && img.width() <= m_lastImage.width()) {
        qDebug() << "[GpuView] showPartial SKIP lowres page=" << page
                 << "newW=" << img.width() << "lastW=" << m_lastImage.width();
        return;
    }
    if (m_panning) {
        qDebug() << "[perf] skip partial during pan page=" << page;
        m_pendingPartImg = img;
        m_pendingPartScale = scale;
        m_pendingPartPage = page;
        return;
    }
    // ponytail: keep m_loading=true so the "Loading…" overlay persists until
    // setPage()/updatePageImage() is called with the final full-resolution image.
    // If pageSizePt is empty, infer from image dimensions / scale so paintGL
    // can render the partial image immediately instead of staying gray.
    if (m_pageSizePt.isEmpty() && scale > 0.0) {
        m_pageSizePt = QSizeF(img.width() / scale, img.height() / scale);
        qDebug() << "[GpuView] WARN fallback pageSizePt from PIXEL size (wrong unit!) page=" << page
                 << "from img" << img.size() << "scale=" << scale;
    }
    m_hasImage = true;
    m_lastImage = img;
    m_lastImagePage = page;
    m_pendingImage = img;
    m_textureDirty = true;
    update();
}

void PdfGpuView::setPendingPage(int pageIndex, QSizeF pageSizePt) {
    bool samePageAndSize = (pageIndex == m_pageIndex && m_pageSizePt == pageSizePt);
    if (samePageAndSize && m_hasImage) {
        qDebug() << "[GpuView] setPendingPage SKIP same page=" << pageIndex << "size=" << pageSizePt;
        return;
    }
    bool pageChanged = (pageIndex != m_pageIndex);
    qDebug() << "[GpuView] setPendingPage idx=" << pageIndex
             << "size=" << pageSizePt << "pageChanged=" << pageChanged;
    m_pageIndex  = pageIndex;
    m_pageSizePt = pageSizePt;
    if (pageChanged) {
        m_panOffset  = {};
        m_highlights.clear();
        m_hasSel     = false;
        m_tiles.clear();
        m_tilePage = -1;
        m_tileScale = 0.0;
        m_hasImage   = false;
        m_lastImage  = {};
        m_lastImagePage = -1;
        m_placeholder = {};
        m_loading    = true;
        qDebug() << "[GpuView] setPendingPage cleared old image — showing placeholder/loading";
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
    m_tiles.clear();
    m_tilePage = -1;
    m_tileScale = 0.0;
    m_tileTimer->start();
    update();
}

void PdfGpuView::requestTiles() {
    if (!m_hasImage || m_pageSizePt.isEmpty()) return;
    double pw = m_pageSizePt.width()  * m_zoom;
    double ph = m_pageSizePt.height() * m_zoom;
    QPointF orig = pageOrigin();

    double visLeft   = qMax(0.0, orig.x());
    double visTop    = qMax(0.0, orig.y());
    double visRight  = qMin((double)width(),  orig.x() + pw);
    double visBottom = qMin((double)height(), orig.y() + ph);
    if (visRight <= visLeft || visBottom <= visTop) return;

    int rx = qMax(0, (int)(visLeft  - orig.x()));
    int ry = qMax(0, (int)(visTop   - orig.y()));
    int rw = qMin((int)pw, (int)(visRight - visLeft));
    int rh = qMin((int)ph, (int)(visBottom - visTop));

    QRect viewportPx(rx, ry, rw, rh);
    m_tilePage  = m_pageIndex;
    m_tileScale = m_zoom;
    emit tilesNeeded(m_pageIndex, m_zoom, viewportPx);
}

void PdfGpuView::setTile(int page, double scale, int col, int row, const QImage& img) {
    if (page != m_pageIndex || qAbs(scale - m_zoom) > 1e-6) return;
    // Reject stale tiles from a different zoom/request cycle
    if (page != m_tilePage || qAbs(m_tileScale - scale) > 1e-6) return;
    if (img.isNull()) return;
    m_tiles[{col, row}] = img;

    // Evict tiles farthest from viewport center when over cap
    if (m_tiles.size() > kMaxLiveTiles) {
        QPointF viewCenter(width() / 2.0, height() / 2.0);
        QPointF orig = pageOrigin();
        using DistKey = QPair<double, QPair<int,int>>;
        QList<DistKey> dists;
        dists.reserve(m_tiles.size());
        for (auto it = m_tiles.constBegin(); it != m_tiles.constEnd(); ++it) {
            int c = it.key().first, r = it.key().second;
            QPointF tc(orig.x() + c * 512.0 + 256.0, orig.y() + r * 512.0 + 256.0);
            dists.append({QPointF(tc - viewCenter).manhattanLength(), {c, r}});
        }
        std::sort(dists.begin(), dists.end(), [](const DistKey& a, const DistKey& b) {
            return a.first > b.first;
        });
        while (m_tiles.size() > kMaxLiveTiles && !dists.isEmpty())
            m_tiles.remove(dists.takeFirst().second);
    }

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
    if (m_sigPickMode) { m_sigPickMode = false; m_sigActive = false; unsetCursor(); }
    m_tool = tool;
    m_hasSel = false;
    setCursor(tool != ViewTool::Pan ? Qt::CrossCursor : Qt::ArrowCursor);
    update();
}

void PdfGpuView::beginSignaturePick() {
    m_sigPickMode = true;
    m_sigActive = false;
    setCursor(Qt::CrossCursor);
    update();
}

void PdfGpuView::setSelectedAnnot(const QRectF& rectPdf) {
    m_selRect = rectPdf;
    m_hasSel = true;
    update();
}

void PdfGpuView::clearSelectedAnnot() {
    m_hasSel = false;
    update();
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

void PdfGpuView::keyPressEvent(QKeyEvent* e) {
    if (m_sigPickMode && e->key() == Qt::Key_Escape) {
        m_sigPickMode = false; m_sigActive = false; unsetCursor(); update();
        return;
    }
    QOpenGLWidget::keyPressEvent(e);
}

void PdfGpuView::wheelEvent(QWheelEvent* e) {
    if (e->modifiers() & Qt::ControlModifier) {
        double delta   = e->angleDelta().y() / 1200.0;
        double newZoom = qBound(0.1, m_zoom + delta, 10.0);
        if (qAbs(newZoom - m_zoom) < 1e-6) return;

        QPointF cursorPdf = widgetToPdf(e->position());
        m_zoom = newZoom;
        QPointF newWidget = pdfToWidget(cursorPdf);
        m_panOffset += e->position() - newWidget;

        m_tiles.clear();
        m_tilePage = -1;
        m_tileScale = 0.0;
        m_tileTimer->start();
        update();
        m_zoomTimer->start();
    } else {
        double dy = e->angleDelta().y() * 0.6;
        double pageHpx = m_pageSizePt.height() * m_zoom;

        // Debounce flips with a short time cooldown so one wheel gesture flips one
        // page. Time-based (not a loading flag) so it can never get stuck.
        auto canFlip = [this]{
            return !m_flipCooldown.isValid() || m_flipCooldown.elapsed() > 220;
        };

        if (pageHpx <= height() + 1.0) {
            // Fitted page: a single notch flips.
            if (canFlip()) {
                if (dy > 0)      { m_flipCooldown.restart(); emit scrolledToPage(m_pageIndex - 1); }
                else if (dy < 0) { m_flipCooldown.restart(); emit scrolledToPage(m_pageIndex + 1); }
            }
            e->accept();
            return;
        }

        // Taller page: pan, and flip when scrolled past the edge.
        m_panOffset.ry() += dy;
        double margin = qMax(height() * 0.12,
                             (pageHpx > height())
                                 ? (pageHpx - height()) / 2.0 + height() * 0.05
                                 : pageHpx * 0.12);
        if (m_panOffset.y() > margin && canFlip()) {
            m_flipCooldown.restart();
            m_panOffset.setY(0.0);
            emit scrolledToPage(m_pageIndex - 1);
        } else if (m_panOffset.y() < -margin && canFlip()) {
            m_flipCooldown.restart();
            m_panOffset.setY(0.0);
            emit scrolledToPage(m_pageIndex + 1);
        }
        update();
    }
}

void PdfGpuView::mousePressEvent(QMouseEvent* e) {
    if (m_sigPickMode && e->button() == Qt::LeftButton) {
        m_sigActive = true;
        m_sigStart = e->position();
        m_sigEnd = e->position();
        update();
        return;
    }
    if (m_tool == ViewTool::Pan && m_hasImage) {
        if (e->button() == Qt::RightButton) {
            emit annotationContextRequested(m_pageIndex, widgetToPdf(e->position()), e->globalPosition().toPoint());
            return;
        }
        if (e->button() == Qt::LeftButton) {
            if (m_hasSel) {
                QPointF a = pdfToWidget(m_selRect.topLeft());
                QPointF b = pdfToWidget(m_selRect.bottomRight());
                QRectF wsel = QRectF(a, b).normalized().adjusted(-3, -3, 3, 3);
                if (wsel.contains(e->position())) {
                    m_draggingAnnot = true;
                    m_dragStart = e->position();
                    m_dragOrigRect = m_selRect;
                    return;
                }
            }
            emit annotationPickRequested(m_pageIndex, widgetToPdf(e->position()));
        }
    }
    if (e->button() == Qt::LeftButton && m_hasImage) {
        if (m_tool == ViewTool::PlaceNote) {
            emit noteRequested(m_pageIndex, widgetToPdf(e->position()));
            return;
        }
        if (m_tool == ViewTool::Line || m_tool == ViewTool::Arrow ||
            m_tool == ViewTool::Rectangle || m_tool == ViewTool::Ellipse ||
            m_tool == ViewTool::Cloud || m_tool == ViewTool::FreeText) {
            m_drawingShape = true;
            m_shapeStart = e->position();
            m_shapeEnd = e->position();
            return;
        }
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
    if (m_sigPickMode && m_sigActive) {
        m_sigEnd = e->position();
        update();
        return;
    }
    if (m_draggingAnnot) {
        QPointF d = (e->position() - m_dragStart) / m_zoom;
        m_selRect = m_dragOrigRect.translated(d);
        update();
        return;
    }
    if (m_drawingShape) {
        m_shapeEnd = e->position();
        update();
        return;
    }
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
    if (m_sigPickMode && m_sigActive && e->button() == Qt::LeftButton) {
        m_sigActive = false;
        m_sigPickMode = false;
        unsetCursor();
        double dx = qAbs(m_sigEnd.x() - m_sigStart.x());
        double dy = qAbs(m_sigEnd.y() - m_sigStart.y());
        if (dx < 8.0 || dy < 8.0) { update(); return; }
        QPointF a = widgetToPdf(m_sigStart);
        QPointF b = widgetToPdf(m_sigEnd);
        double L = qMin(a.x(), b.x());
        double R = qMax(a.x(), b.x());
        double topY = qMin(a.y(), b.y());
        double botY = qMax(a.y(), b.y());
        double Wd = R - L;
        double Hd = botY - topY;
        double pageH = m_pageSizePt.height();
        double pageW = m_pageSizePt.width();
        QRectF rectPt(qMax(0.0, L), qMax(0.0, pageH - botY), qMin(Wd, pageW), qMin(Hd, pageH));
        emit signatureRectPicked(m_pageIndex, rectPt);
        update();
        return;
    }
    if (m_draggingAnnot) {
        m_draggingAnnot = false;
        double dx = (e->position().x() - m_dragStart.x()) / m_zoom;
        double dy = -(e->position().y() - m_dragStart.y()) / m_zoom;
        if (qAbs(dx) > 1.0 || qAbs(dy) > 1.0)
            emit annotationMoveRequested(m_pageIndex, dx, dy);
        return;
    }
    if (m_drawingShape) {
        if (e->button() == Qt::LeftButton) {
            m_drawingShape = false;
            QPointF startPdf = widgetToPdf(m_shapeStart);
            QPointF endPdf   = widgetToPdf(m_shapeEnd);
            if (m_tool == ViewTool::FreeText) {
                QRectF r = ((endPdf - startPdf).manhattanLength() > 5.0)
                           ? QRectF(startPdf, endPdf).normalized()
                           : QRectF(startPdf, QSizeF(160.0, 40.0));
                emit textBoxRequested(m_pageIndex, r);
            } else if ((endPdf - startPdf).manhattanLength() > 5.0) {
                AnnotTool at = AnnotTool::Line;
                switch (m_tool) {
                    case ViewTool::Arrow:     at = AnnotTool::Arrow; break;
                    case ViewTool::Rectangle: at = AnnotTool::Rectangle; break;
                    case ViewTool::Ellipse:   at = AnnotTool::Ellipse; break;
                    case ViewTool::Cloud:     at = AnnotTool::Cloud; break;
                    default:                  at = AnnotTool::Line; break;
                }
                emit shapeCommitRequested(m_pageIndex, at, startPdf, endPdf);
            }
        } else {
            m_drawingShape = false;
        }
        setCursor(m_tool != ViewTool::Pan ? Qt::CrossCursor : Qt::ArrowCursor);
        update();
        return;
    }
    if (m_selecting && e->button() == Qt::LeftButton) {
        m_selecting = false;
        setCursor(m_tool != ViewTool::Pan ? Qt::CrossCursor : Qt::ArrowCursor);

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
    if (!m_pendingPartImg.isNull() && m_pendingPartPage == m_pageIndex) {
        qDebug() << "[perf] flush pending partial after pan page=" << m_pageIndex;
        showPartial(m_pendingPartPage, m_pendingPartScale, m_pendingPartImg);
        m_pendingPartImg = {};
        m_pendingPartPage = -1;
        m_pendingPartScale = 0.0;
    }
    setCursor(m_tool != ViewTool::Pan ? Qt::CrossCursor : Qt::ArrowCursor);
    update();
}
