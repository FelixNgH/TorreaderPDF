#include "PdfGpuView.h"
#include <QOpenGLExtraFunctions>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QPainter>
#include <QPainterPath>
#include <QDebug>
#include <QFontDatabase>
#include <cmath>
#include <QVector4D>

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

// ── Ghost baseline & font helpers (§3.5) ────────────────────────────────────
QPointF trGhostBaseline(const QRectF& dispRect) {
    return QPointF(dispRect.left() + 4.0, dispRect.bottom() - 2.0);
}
qreal trGhostPixelSize(float fontSizePt, double zoom) {
    return static_cast<qreal>(fontSizePt) * zoom;
}
static QFont trNoteFont(const QString& fallbackFamily) {
    static bool loaded = false;
    static QString dejaVuFamily;
    if (!loaded) {
        loaded = true;
        int id = QFontDatabase::addApplicationFont(":/fonts/DejaVuSans.ttf");
        if (id >= 0) {
            QStringList families = QFontDatabase::applicationFontFamilies(id);
            if (!families.isEmpty()) dejaVuFamily = families.first();
        }
        if (dejaVuFamily.isEmpty()) {
            qWarning() << "[ghost] DejaVuSans not found, using system font";
        }
    }
    return QFont(dejaVuFamily.isEmpty() ? fallbackFamily : dejaVuFamily);
}

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
        // Vector overlay cleanup
        if (m_vecProg) { delete m_vecProg; m_vecProg = nullptr; }
        if (m_fillProg) { delete m_fillProg; m_fillProg = nullptr; }
        if (m_tileProg) { delete m_tileProg; m_tileProg = nullptr; }
        QOpenGLExtraFunctions* glx = QOpenGLContext::currentContext()->extraFunctions();
        if (glx && m_vecVao) { glx->glDeleteVertexArrays(1, &m_vecVao); m_vecVao = 0; }
        if (m_vecVboPos) { glDeleteBuffers(1, &m_vecVboPos); m_vecVboPos = 0; }
        if (m_vecVboCol) { glDeleteBuffers(1, &m_vecVboCol); m_vecVboCol = 0; }
        if (m_vecVboQuad) { glDeleteBuffers(1, &m_vecVboQuad); m_vecVboQuad = 0; }
        if (m_vecVboWidth) { glDeleteBuffers(1, &m_vecVboWidth); m_vecVboWidth = 0; }
        if (m_vecVboDepth) { glDeleteBuffers(1, &m_vecVboDepth); m_vecVboDepth = 0; }
        if (m_vecVboClip) { glDeleteBuffers(1, &m_vecVboClip); m_vecVboClip = 0; }
        if (glx && m_fillVao) { glx->glDeleteVertexArrays(1, &m_fillVao); m_fillVao = 0; }
        if (m_fillVboPos) { glDeleteBuffers(1, &m_fillVboPos); m_fillVboPos = 0; }
        if (m_fillVboCol) { glDeleteBuffers(1, &m_fillVboCol); m_fillVboCol = 0; }
        if (m_fillVboDepth) { glDeleteBuffers(1, &m_fillVboDepth); m_fillVboDepth = 0; }
        if (m_fillVboClip) { glDeleteBuffers(1, &m_fillVboClip); m_fillVboClip = 0; }
        if (glx && m_tileVao) { glx->glDeleteVertexArrays(1, &m_tileVao); m_tileVao = 0; }
        for (GLuint t : m_tileTexText) if (t) glDeleteTextures(1, &t);
        for (GLuint t : m_tileTexImg) if (t) glDeleteTextures(1, &t);
        m_tileTexText.clear();
        m_tileTexImg.clear();
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

    // Vector overlay shader
    static const char* vecVsrc = R"(
        #version 330 core
        layout(location = 0) in vec2 aCorner;
        layout(location = 1) in vec2 aP0;
        layout(location = 2) in vec2 aP1;
        layout(location = 3) in vec4 aColor;
        layout(location = 4) in float aWidthPt;
        layout(location = 5) in float aDepth;
        layout(location = 6) in float aClipIdx;
        uniform mat4  uMvp;
        uniform vec2  uViewport;
        uniform float uPxPerPt;
        uniform vec4  uClips[64];
        out vec4 vColor;
        flat out vec4 vClip;
        out vec2 vPagePos;
        void main() {
            vec4 c0 = uMvp * vec4(aP0, 0.0, 1.0);
            vec4 c1 = uMvp * vec4(aP1, 0.0, 1.0);
            vec2 s0 = (c0.xy / c0.w * 0.5 + 0.5) * uViewport;
            vec2 s1 = (c1.xy / c1.w * 0.5 + 0.5) * uViewport;
            vec2 d  = s1 - s0;
            float L = length(d);
            d = (L > 1e-6) ? d / L : vec2(1.0, 0.0);
            vec2 n  = vec2(-d.y, d.x);
            float wRaw = aWidthPt * uPxPerPt;
            float cov  = 1.0;
            if (aWidthPt > 0.0 && wRaw < 1.0) cov = max(wRaw, 0.15);
            float wpx  = max(wRaw, 1.0);
            vec2 p = mix(s0, s1, aCorner.x) + n * (aCorner.y - 0.5) * wpx;
            gl_Position = vec4((p / uViewport) * 2.0 - 1.0, aDepth * 2.0 - 1.0, 1.0);
            vColor = vec4(aColor.rgb, aColor.a * cov);
            vClip = uClips[int(aClipIdx)];
            vPagePos = mix(aP0, aP1, aCorner.x);
        }
    )";
    static const char* vecFsrc = R"(
        #version 330 core
        in vec4 vColor;
        flat in vec4 vClip;
        in vec2 vPagePos;
        out vec4 fragColor;
        void main() {
            if (vClip.z > 0.0 && (vPagePos.x < vClip.x || vPagePos.x > vClip.x + vClip.z ||
                                  vPagePos.y < vClip.y || vPagePos.y > vClip.y + vClip.w)) discard;
            fragColor = vColor;
        }
    )";
    m_vecProg = new QOpenGLShaderProgram(this);
    m_vecProg->addShaderFromSourceCode(QOpenGLShader::Vertex, vecVsrc);
    m_vecProg->addShaderFromSourceCode(QOpenGLShader::Fragment, vecFsrc);
    if (!m_vecProg->link()) {
        qDebug() << "[GpuView] vector shader link failed:" << m_vecProg->log();
        delete m_vecProg; m_vecProg = nullptr;
    } else {
        m_vecMvpLoc = m_vecProg->uniformLocation("uMvp");
        m_vecViewportLoc = m_vecProg->uniformLocation("uViewport");
        m_vecPxPerPtLoc = m_vecProg->uniformLocation("uPxPerPt");
    }

    static const char* fillVsrc = R"(
        #version 330 core
        layout(location = 0) in vec2 aPos;
        layout(location = 1) in vec4 aColor;
        layout(location = 2) in float aDepth;
        layout(location = 3) in float aClipIdx;
        uniform mat4 uMvp;
        uniform vec4 uClips[64];
        out vec4 vColor;
        flat out vec4 vClip;
        out vec2 vPagePos;
        void main() {
            vec4 pos = uMvp * vec4(aPos, 0.0, 1.0);
            gl_Position = vec4(pos.xy, (aDepth * 2.0 - 1.0) * pos.w, pos.w);
            vColor = aColor;
            vClip = uClips[int(aClipIdx)];
            vPagePos = aPos;
        }
    )";
    static const char* fillFsrc = R"(
        #version 330 core
        in vec4 vColor;
        flat in vec4 vClip;
        in vec2 vPagePos;
        out vec4 fragColor;
        void main() {
            if (vClip.z > 0.0 && (vPagePos.x < vClip.x || vPagePos.x > vClip.x + vClip.z ||
                                  vPagePos.y < vClip.y || vPagePos.y > vClip.y + vClip.w)) discard;
            fragColor = vColor;
        }
    )";
    m_fillProg = new QOpenGLShaderProgram(this);
    m_fillProg->addShaderFromSourceCode(QOpenGLShader::Vertex, fillVsrc);
    m_fillProg->addShaderFromSourceCode(QOpenGLShader::Fragment, fillFsrc);
    if (!m_fillProg->link()) {
        qDebug() << "[GpuView] fill shader link failed:" << m_fillProg->log();
        delete m_fillProg; m_fillProg = nullptr;
    } else {
        m_fillMvpLoc = m_fillProg->uniformLocation("uMvp");
    }

    static const char* tileVsrc = R"(
        #version 330 core
        layout(location = 0) in vec2 aCorner;
        uniform mat4  uMvp;
        uniform vec4  uRect;
        uniform float uDepth;
        uniform vec4  uClips[64];
        uniform float uClipIdx;
        out vec2 vUV;
        flat out vec4 vClip;
        out vec2 vPagePos;
        void main() {
            vec2 p = uRect.xy + aCorner * uRect.zw;
            vec4 pos = uMvp * vec4(p, 0.0, 1.0);
            gl_Position = vec4(pos.xy, (uDepth * 2.0 - 1.0) * pos.w, pos.w);
            vUV = aCorner;
            vClip = uClips[int(uClipIdx)];
            vPagePos = p;
        }
    )";
    static const char* tileFsrc = R"(
        #version 330 core
        uniform sampler2D uTex;
        uniform int  uIsAlpha;
        uniform vec4 uColor;
        in vec2 vUV;
        flat in vec4 vClip;
        in vec2 vPagePos;
        out vec4 fragColor;
        void main() {
            if (vClip.z > 0.0 && (vPagePos.x < vClip.x || vPagePos.x > vClip.x + vClip.z ||
                                  vPagePos.y < vClip.y || vPagePos.y > vClip.y + vClip.w)) discard;
            vec4 c;
            if (uIsAlpha == 1) c = vec4(uColor.rgb, texture(uTex, vUV).r);
            else               c = texture(uTex, vUV);
            if (c.a < 0.02) discard;
            fragColor = c;
        }
    )";
    m_tileProg = new QOpenGLShaderProgram(this);
    m_tileProg->addShaderFromSourceCode(QOpenGLShader::Vertex, tileVsrc);
    m_tileProg->addShaderFromSourceCode(QOpenGLShader::Fragment, tileFsrc);
    if (!m_tileProg->link()) {
        qDebug() << "[GpuView] tile shader link failed:" << m_tileProg->log();
        delete m_tileProg; m_tileProg = nullptr;
    } else {
        m_tileMvpLoc = m_tileProg->uniformLocation("uMvp");
        m_tileRectLoc = m_tileProg->uniformLocation("uRect");
        m_tileDepthLoc = m_tileProg->uniformLocation("uDepth");
        m_tileTexLoc = m_tileProg->uniformLocation("uTex");
        m_tileIsAlphaLoc = m_tileProg->uniformLocation("uIsAlpha");
        m_tileColorLoc = m_tileProg->uniformLocation("uColor");
    }

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

    static const float tileQuad[] = { 0,0, 0,1, 1,0, 1,1 };
    QOpenGLExtraFunctions* glx = QOpenGLContext::currentContext()->extraFunctions();
    if (!glx) return;
    glx->glGenVertexArrays(1, &m_tileVao);
    glx->glBindVertexArray(m_tileVao);
    GLuint tileVbo;
    glGenBuffers(1, &tileVbo);
    glBindBuffer(GL_ARRAY_BUFFER, tileVbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(tileQuad), tileQuad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
    glx->glBindVertexArray(0);
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

QMatrix4x4 PdfGpuView::vectorTransform() const {
    // Dinh vector o DON VI DIEM PDF CHUA XOAY, Y-down. Doi sang pixel widget, co ap /Rotate.
    const QSizeF vp = m_vecLayer ? m_vecLayer->pageSizePt() : QSizeF();
    if (vp.width() <= 0 || vp.height() <= 0) return QMatrix4x4();

    const double pw   = m_pageSizePt.width()  * m_zoom;   // be ngang trang tren man, px (DA xoay)
    const double ph   = m_pageSizePt.height() * m_zoom;
    const QPointF orig = pageOrigin();                    // DA gom m_panOffset
    const int rot = m_vecLayer->rotation() & 3;

    // Voi rot le, be ngang tren man ung voi CHIEU CAO hop chua xoay va nguoc lai.
    const double sx = (rot & 1) ? (pw / vp.height()) : (pw / vp.width());
    const double sy = (rot & 1) ? (ph / vp.width())  : (ph / vp.height());

    QMatrix4x4 m;
    m.ortho(0.f, (float)width(), (float)height(), 0.f, -1.f, 1.f);
    m.translate((float)orig.x(), (float)orig.y(), 0.f);
    switch (rot) {
        case 1: m.translate((float)pw, 0.f, 0.f);        m.rotate(90.f,  0.f, 0.f, 1.f); break;
        case 2: m.translate((float)pw, (float)ph, 0.f);  m.rotate(180.f, 0.f, 0.f, 1.f); break;
        case 3: m.translate(0.f, (float)ph, 0.f);        m.rotate(270.f, 0.f, 0.f, 1.f); break;
        default: break;
    }
    m.scale((float)sx, (float)sy, 1.f);
    return m;
}

// ── Paint ─────────────────────────────────────────────────────────────────────

void PdfGpuView::paintGL() {
    const bool pureVector =
           m_vecLayer && m_vecLayer->isReady()
        && m_vecLayer->pageIndex() == m_pageIndex
        && m_vecLayer->isComplete()
        && shouldUseVectorOverlay();

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
    if (m_hasImage && m_texture && !m_pageSizePt.isEmpty() && !pureVector) {
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
    } else if (pureVector && !m_pageSizePt.isEmpty()) {
        // Vector thuan: khong ve raster nhung VAN phai co nen trang trang.
        // Ve bang GL (quad trang + u_bgColor) vi net QPainter khong song sot qua
        // beginNativePainting() tren Qt 6.2/Linux -> trang bi trong suot.
        m_program->bind();
        QMatrix4x4 transform = computeTransform();
        glUniformMatrix4fv(m_uTransform, 1, GL_FALSE, transform.constData());
        glUniform1i(m_uHasTex, 0);
        glUniform4f(m_uBgColor, 1.0f, 1.0f, 1.0f, 1.0f);
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
        if (!_needQP) return;
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
        if (pureVector) {
            p.fillRect(QRectF(orig.x(), orig.y(), pw, ph), Qt::white);
        }
        p.fillRect(QRectF(orig.x() + pw, orig.y() + sh, sh, ph), QColor(0, 0, 0, 80));
        p.fillRect(QRectF(orig.x() + sh, orig.y() + ph, pw, sh), QColor(0, 0, 0, 80));

        // ── Draw sharp-region overlay (viewport re-rendered at true zoom) ──
        if (m_sharpPage == m_pageIndex && qAbs(m_sharpScale - m_zoom) < 1e-6 && !m_sharpImage.isNull() && !pureVector) {
            p.save();
            p.setRenderHint(QPainter::Antialiasing, false);
            p.setRenderHint(QPainter::SmoothPixmapTransform, false);
            p.drawImage(QPoint(qRound(orig.x() + m_sharpRegion.x()),
                               qRound(orig.y() + m_sharpRegion.y())), m_sharpImage);
            p.restore();
        }

        // ── Vector overlay (sharp strokes on top of raster + sharp-region) ──
        if (m_vecLayer && m_vecLayer->isReady() && m_vecLayer->pageIndex() == m_pageIndex
            && shouldUseVectorOverlay()) {
            p.beginNativePainting();
            drawVectorOverlay();
            p.endNativePainting();
        }

        // Highlights (display coords: Y-down, rotation applied)
        if (!m_highlights.isEmpty()) {
            p.save();
            double pageH = m_pageSizePt.height();
            for (int hi = 0; hi < m_highlights.size(); ++hi) {
                QRectF nr = m_highlights[hi].normalized();
                if (nr.width() < 0.5 || nr.height() < 0.5) continue;
                double wx = orig.x() + nr.x()      * m_zoom;
                double wy = orig.y() + nr.y()      * m_zoom;
                if (hi == m_currentHighlightIdx) {
                    p.setBrush(QColor(255, 140, 0, 220));
                    p.setPen(QPen(QColor(200, 80, 0, 240), 2.0));
                } else {
                    p.setBrush(QColor(255, 220, 0, 90));
                    p.setPen(QPen(QColor(255, 150, 0, 160), 1.0));
                }
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
            int cntVis = 0, cntSticky = 0, cntDrawn = 0;
            for (const auto& ov : m_annotOverlays) {
                if (ov.pageIndex != m_pageIndex) continue;
                const bool isDragged = m_draggingAnnot && !m_dragUid.isEmpty() && ov.uid == m_dragUid;
                if (isDragged) { p.save(); p.translate(m_dragPixelDelta); }
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
                if (isDragged) p.restore();
            }
            {
                static int sV = -1, sS = -1, sD = -1;
                if (cntVis != sV || cntSticky != sS || cntDrawn != sD) {
                    sV = cntVis; sS = cntSticky; sD = cntDrawn;
                    qDebug().noquote() << "[badge] visualsPage=" << cntVis << "stickyNotes=" << cntSticky << "drawn=" << cntDrawn << "pureVector=" << pureVector;
                }
            }
            p.restore();
        }

        // ── Pending markup overlay (instant feedback until the page re-render bakes it in) ──
        if (!m_pendingMarkups.isEmpty()) {
            p.save();
            p.setRenderHint(QPainter::Antialiasing, true);
            for (const PendingMarkup& pm : m_pendingMarkups) {
                QPointF A = pdfToWidget(pm.a);
                QPointF B = pdfToWidget(pm.b);
                QPen pen(pm.color, qMax(1.0, pm.width * m_zoom));
                p.setPen(pen);
                p.setBrush(pm.fill.alpha() > 0 ? QBrush(pm.fill) : Qt::NoBrush);
                QRectF r = QRectF(A, B).normalized();
                switch (pm.tool) {
                    case AnnotTool::Line:
                        p.drawLine(A, B); break;
                    case AnnotTool::Arrow: {
                        p.drawLine(A, B);
                        double ang = std::atan2(B.y() - A.y(), B.x() - A.x());
                        const double hl = 14.0, d = 0.45;
                        QPointF w1(B.x() - hl * std::cos(ang - d), B.y() - hl * std::sin(ang - d));
                        QPointF w2(B.x() - hl * std::cos(ang + d), B.y() - hl * std::sin(ang + d));
                        p.drawLine(B, w1); p.drawLine(B, w2);
                        break; }
                    case AnnotTool::Rectangle:
                        p.drawRect(r); break;
                    case AnnotTool::Cloud: {
                        QPainterPath path;
                        const double rad = 8.0;
                        auto edge = [&](QPointF p0, QPointF p1, QPointF nrm){
                            double dx=p1.x()-p0.x(), dy=p1.y()-p0.y();
                            double len=std::hypot(dx,dy);
                            if(len<1.0) return;
                            int bumps=qMax(1,int(len/(2.0*rad)));
                            double ux=dx/len, uy=dy/len, seg=len/bumps;
                            for(int i=0;i<bumps;i++){
                                double sx=p0.x()+ux*seg*i, sy=p0.y()+uy*seg*i;
                                for(int k=0;k<=6;k++){
                                    double t=k/6.0;
                                    double px=sx+ux*seg*t, py=sy+uy*seg*t;
                                    double bulge=std::sin(t*3.14159265)*rad;
                                    QPointF q(px+nrm.x()*bulge, py+nrm.y()*bulge);
                                    if(path.isEmpty()) path.moveTo(q); else path.lineTo(q);
                                }
                            }
                        };
                        edge(r.bottomLeft(),  r.bottomRight(), QPointF(0, 1));
                        edge(r.bottomRight(), r.topRight(),    QPointF(1, 0));
                        edge(r.topRight(),    r.topLeft(),     QPointF(0,-1));
                        edge(r.topLeft(),     r.bottomLeft(),  QPointF(-1,0));
                        path.closeSubpath();
                        p.setBrush(Qt::NoBrush);
                        p.drawPath(path);
                        break; }
                    case AnnotTool::Ellipse:
                        p.drawEllipse(r); break;
                    case AnnotTool::Highlight:
                        p.fillRect(r, QColor(255, 255, 0, 110)); break;
                    case AnnotTool::Freehand: {
                        if (pm.freehand.size() >= 2) {
                            QPolygonF poly;
                            for (const QPointF& pt : pm.freehand) poly << pdfToWidget(pt);
                            p.setBrush(Qt::NoBrush);
                            p.drawPolyline(poly);
                        }
                        break; }
                    default: break;
                }
            }
            p.restore();
        }

        // ── Annotation overlay (step 1: drawn from model instead of PDFium render) ──
        if (!m_annotVisuals.isEmpty()) {
            { static int _lastLogPage = -1, _lastLogSize = -1;
              bool anyMatch = false;
              for (const auto& av : m_annotVisuals)
                  if (av.page == m_pageIndex) { anyMatch = true; break; }
              if (!anyMatch && (_lastLogPage != m_pageIndex || _lastLogSize != m_annotVisuals.size())) {
                  _lastLogPage = m_pageIndex;
                  _lastLogSize = m_annotVisuals.size();
                  qDebug().noquote() << "[annot] overlay skipped — visualsPage="
                      << (m_annotVisuals.isEmpty() ? -1 : m_annotVisuals.first().page)
                      << "viewPage=" << m_pageIndex << "n=" << m_annotVisuals.size();
              }
            }
            p.save();
            p.setRenderHint(QPainter::Antialiasing, true);
            QPointF orig = pageOrigin();
            int cntVis = 0, cntSticky = 0, cntDrawn = 0;
            for (const AnnotVisual& av : m_annotVisuals) {
                if (av.page != m_pageIndex) continue;
                ++cntVis;
                if (av.subtype == FPDF_ANNOT_TEXT) ++cntSticky;
                if (!av.paintByOverlay) continue;   // FreeText/Note do lop nen ve (dung font nhung)
                const bool isDragged = m_draggingAnnot && !m_dragUid.isEmpty() && av.uid == m_dragUid;
                if (isDragged) { p.save(); p.translate(m_dragPixelDelta); }
                QPointF dOrig = orig + QPointF(av.rect.x() * m_zoom, av.rect.y() * m_zoom);
                QRectF dRect(dOrig, QSizeF(av.rect.width() * m_zoom, av.rect.height() * m_zoom));
                QPen strokePen(av.stroke.isValid() ? av.stroke : QColor(Qt::red), qMax(1.0, av.border * m_zoom));
                p.setPen(strokePen);
                p.setBrush(av.fill.isValid() && av.fill.alpha() > 0 ? QBrush(av.fill) : Qt::NoBrush);

                switch (av.subtype) {
                    case FPDF_ANNOT_INK: {
                        for (const auto& stroke : av.ink) {
                            if (stroke.size() < 2) continue;
                            QPolygonF poly;
                            for (const QPointF& pt : stroke)
                                poly << QPointF(orig.x() + pt.x() * m_zoom, orig.y() + pt.y() * m_zoom);
                            p.setBrush(Qt::NoBrush);
                            p.drawPolyline(poly);
                        }
                        break;
                    }
                    case FPDF_ANNOT_SQUARE:
                        p.drawRect(dRect);
                        break;
                    case FPDF_ANNOT_CIRCLE:
                        p.drawEllipse(dRect);
                        break;
                    case FPDF_ANNOT_HIGHLIGHT: {
                        if (!av.quads.isEmpty()) {
                            p.setBrush(QColor(255, 255, 0, 90));
                            p.setPen(Qt::NoPen);
                            for (const QRectF& qr : av.quads) {
                                QPointF qo(orig.x() + qr.x() * m_zoom, orig.y() + qr.y() * m_zoom);
                                p.drawRect(QRectF(qo, QSizeF(qr.width() * m_zoom, qr.height() * m_zoom)));
                            }
                        } else {
                            p.setBrush(QColor(255, 255, 0, 90));
                            p.setPen(Qt::NoPen);
                            p.drawRect(dRect);
                        }
                        break;
                    }
                    case FPDF_ANNOT_LINE: {
                        QPointF lA(orig.x() + av.rect.left() * m_zoom, orig.y() + av.rect.top() * m_zoom);
                        QPointF lB(orig.x() + av.rect.right() * m_zoom, orig.y() + av.rect.bottom() * m_zoom);
                        p.drawLine(lA, lB);
                        break;
                    }
                    case FPDF_ANNOT_POLYGON: {
                        // Polygon uses ink strokes as vertex list (first stroke = boundary)
                        if (!av.ink.isEmpty() && av.ink[0].size() >= 3) {
                            QPolygonF poly;
                            for (const QPointF& pt : av.ink[0])
                                poly << QPointF(orig.x() + pt.x() * m_zoom, orig.y() + pt.y() * m_zoom);
                            p.drawPolygon(poly);
                        }
                        break;
                    }
                    case FPDF_ANNOT_FREETEXT: {
                        double fs = qMax(6.0, av.fontSize * m_zoom);
                        QFont ft = p.font();
                        ft.setPointSizeF(fs);
                        p.setFont(ft);
                        p.setPen(av.stroke.isValid() ? QPen(av.stroke) : QPen(Qt::black));
                        p.drawText(dRect, Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, av.text);
                        break;
                    }
                    default: break;
                }
                if (isDragged) p.restore();
            }
            {
                static int sV = -1, sS = -1, sD = -1;
                if (cntVis != sV || cntSticky != sS || cntDrawn != sD) {
                    sV = cntVis; sS = cntSticky; sD = cntDrawn;
                    qDebug().noquote() << "[badge] visualsPage=" << cntVis << "stickyNotes=" << cntSticky << "drawn=" << cntDrawn << "pureVector=" << pureVector;
                }
            }
            p.restore();
        }

    }

    // Text selection overlay (Alt+drag)
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
        } else if (m_tool == ViewTool::Rectangle || m_tool == ViewTool::FreeText || m_tool == ViewTool::Cloud || m_tool == ViewTool::Highlight) {
            p.drawRect(sr.normalized());
        } else if (m_tool == ViewTool::Ellipse) {
            p.drawEllipse(sr.normalized());
        }
        p.restore();
    }

    // Freehand preview while drawing
    if (m_drawingFreehand && m_freehandPoints.size() >= 2) {
        p.save();
        p.setPen(QPen(Qt::blue, 2, Qt::DashLine));
        QPointF orig = pageOrigin();
        for (int i = 1; i < m_freehandPoints.size(); ++i) {
            QPointF a = m_freehandPoints[i-1] * m_zoom + orig;
            QPointF b = m_freehandPoints[i]   * m_zoom + orig;
            p.drawLine(a, b);
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
    // ponytail: skip "Loading…" when pure vector mode is active (no raster image expected)
    if (m_loading && !pureVector) {
        if (!m_hasImage) {
            if (!m_placeholder.isNull() && !m_pageSizePt.isEmpty()) {
                // Draw thumbnail placeholder scaled to page rect
                double pw = m_pageSizePt.width() * m_zoom;
                double ph = m_pageSizePt.height() * m_zoom;
                QPointF orig = pageOrigin();
                QRectF pageRect(orig.x(), orig.y(), pw, ph);
                p.fillRect(pageRect, Qt::white);
                p.drawImage(pageRect, m_placeholder);
            } else if (!m_pageSizePt.isEmpty()) {
                // No thumbnail — draw blank page rect
                double pw = m_pageSizePt.width() * m_zoom;
                double ph = m_pageSizePt.height() * m_zoom;
                QPointF orig = pageOrigin();
                p.fillRect(QRectF(orig.x(), orig.y(), pw, ph), Qt::white);
            } else if (m_pageSizePt.isEmpty()) {
                p.fillRect(rect(), QColor(0, 0, 0, 80));
            }
        }
        QFont f = p.font(); f.setPointSize(26); f.setBold(true); p.setFont(f);
        p.setPen(QPen(QColor(255, 255, 255), 2));
        p.drawText(rect(), Qt::AlignCenter, "Loading…");
    }

}

// ── Page management ───────────────────────────────────────────────────────────

void PdfGpuView::setPage(int pageIndex, const QImage& pageImage, QSizeF pageSizePt) {
    bool pageChanged = (pageIndex != m_pageIndex);
    bool newPage = pageChanged || !m_hasImage;
    int oldPage = m_pageIndex;
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
             << "pageChanged=" << pageChanged
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
    if (pageChanged) {
        m_highlights.clear();
        m_currentHighlightIdx = -1;
        qDebug().noquote() << "[find] highlights cleared (page change" << oldPage << "→" << pageIndex << ")";
    }
    if (newPage) {
        m_panOffset = {};
        m_annotVisuals.clear();
        m_hasSel = false;
        m_tiles.clear();
        m_tilePage = -1;
        m_tileScale = 0.0;
        m_sharpPage = -1; m_sharpImage = {};
        scheduleTiles();
    }
    update();
}

void PdfGpuView::setSecondPage(int, const QImage&, QSizeF) {
    // GPU view is single-mode only
}

void PdfGpuView::updatePageImage(const QImage& pageImage) {
    m_loading  = false;
    m_hasImage = !pageImage.isNull();
    m_pendingMarkups.clear();
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

QSize PdfGpuView::currentPageImageSize() const {
    return m_lastImage.size();
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
        m_currentHighlightIdx = -1;
        qDebug().noquote() << "[find] highlights cleared (page change" << m_pageIndex << "→" << pageIndex << ")";
        m_hasSel     = false;
        m_annotVisuals.clear();
        m_tiles.clear();
        m_tilePage = -1;
        m_tileScale = 0.0;
        m_sharpPage = -1; m_sharpImage = {};
        m_hasImage   = false;
        m_lastImage  = {};
        m_lastImagePage = -1;
        m_placeholder = {};
        m_loading    = true;
        m_pendingMarkups.clear();
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
    m_sharpPage = -1; m_sharpImage = {};
    scheduleTiles();
    update();
}

void PdfGpuView::centerPage() {
    m_panOffset = QPointF();
    invalidateSharp();
    update();
}

void PdfGpuView::requestTiles() {
    if (!m_hasImage || m_pageSizePt.isEmpty()) return;
    if (m_vecLayer && m_vecLayer->isReady() && m_vecLayer->pageIndex() == m_pageIndex
        && m_vecLayer->isComplete() && shouldUseVectorOverlay()) {
        m_sharpPage = -1; m_sharpImage = {};
        return;
    }
    // Vector overlay is additive (draws strokes on top of raster) — let
    // region render run normally so images/fills stay sharp underneath.
    // Only pay for a sharp-region render when the base full-page image
    // (capped at 4000px long edge, = PdfRenderer::kFullRenderMaxPx) is being
    // upscaled. Below that the base is already sharp, so a region render is
    // wasted work and stalls markup/page renders on the shared PDFium mutex.
    constexpr double kBaseMaxPx = 4000.0;
    const double longSidePx = qMax(m_pageSizePt.width(), m_pageSizePt.height()) * m_zoom;
    if (longSidePx <= kBaseMaxPx * 1.02) {
        m_sharpPage = -1; m_sharpImage = {};
        update();
        return;
    }
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

    constexpr int kSnap = 256;
    int sx = (rx / kSnap) * kSnap;
    int sy = (ry / kSnap) * kSnap;
    int ex = ((rx + rw + kSnap - 1) / kSnap) * kSnap;
    int ey = ((ry + rh + kSnap - 1) / kSnap) * kSnap;
    ex = qMin(ex, (int)pw);
    ey = qMin(ey, (int)ph);
    rx = sx; ry = sy; rw = ex - sx; rh = ey - sy;
    if (rw <= 0 || rh <= 0) return;

    QRect viewportPx(rx, ry, rw, rh);
    m_tilePage  = m_pageIndex;
    m_tileScale = m_zoom;
    emit tilesNeeded(m_pageIndex, m_zoom, viewportPx);
}

void PdfGpuView::scheduleTiles() {
    m_tileTimer->start(shouldUseVectorOverlay() ? 1500 : 120);
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

void PdfGpuView::setRegion(int page, double scale, QRect regionPx, const QImage& img) {
    if (img.isNull()) return;
    if (page != m_pageIndex) return;
    if (qAbs(scale - m_zoom) > 1e-6) return;
    m_sharpPage   = page;
    m_sharpScale  = scale;
    m_sharpRegion = regionPx;
    m_sharpImage  = img;
    update();
}

void PdfGpuView::invalidateSharp() {
    m_sharpPage  = -1;
    m_sharpImage = {};
    update();
    scheduleTiles();
}

void PdfGpuView::invalidateTiles() {
    m_tiles.clear();
    m_tilePage = -1;
    m_tileScale = 0.0;
    scheduleTiles();
    update();
}

void PdfGpuView::invalidateTileTextures() {
    makeCurrent();
    for (auto& tex : m_tileTexText) {
        if (tex != 0) glDeleteTextures(1, &tex);
    }
    m_tileTexText.clear();
    for (auto& tex : m_tileTexImg) {
        if (tex != 0) glDeleteTextures(1, &tex);
    }
    m_tileTexImg.clear();
    doneCurrent();
    update();
}

void PdfGpuView::addPendingMarkup(AnnotTool tool, const AnnotStyle& style,
                                  QPointF a, QPointF b, const QVector<QPointF>& freehand) {
    PendingMarkup pm;
    pm.tool = tool;
    pm.color = style.strokeColor;
    pm.width = style.strokeWidth;
    pm.fill = style.fillColor;
    pm.a = a; pm.b = b; pm.freehand = freehand;
    m_pendingMarkups.append(pm);
    update();
}

void PdfGpuView::clearPendingMarkups() {
    m_pendingMarkups.clear();
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
    m_dragPixelDelta = QPointF();
    m_selRect = rectPdf;
    m_hasSel = true;
    update();
}

void PdfGpuView::clearSelectedAnnot() {
    m_hasSel = false;
    clearDragTarget();
    update();
}

void PdfGpuView::setDragTarget(const QString& uid, const QString& ghostText,
                               float fontSizePt, const QColor& ghostColor) {
    m_dragUid     = uid;
    Q_UNUSED(ghostText)
    Q_UNUSED(fontSizePt)
    Q_UNUSED(ghostColor)
}

void PdfGpuView::clearDragTarget() {
    m_dragUid.clear();
}

void PdfGpuView::setDragNote(const QRectF& rPt) {
    m_dragNoteRect = rPt;
    m_dragNoteOffsetPt = QPointF();
    update();
}

void PdfGpuView::clearDragState() {
    m_dragPixelDelta = QPointF();
    m_dragNoteRect = QRectF();
    m_dragNoteOffsetPt = QPointF();
    m_dragUid.clear();
    update();
}

void PdfGpuView::setHighlights(const QList<QRectF>& rects) {
    m_highlights = rects;
    m_currentHighlightIdx = -1;
    update();
}

void PdfGpuView::setHighlights(const QList<QRectF>& all, int currentIdx) {
    m_highlights = all;
    m_currentHighlightIdx = currentIdx;
    update();
}

void PdfGpuView::clearHighlights() {
    m_highlights.clear();
    m_currentHighlightIdx = -1;
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

void PdfGpuView::setAnnotVisuals(const QList<AnnotVisual>& visuals) {
    m_annotVisuals = visuals;
    update();
}

void PdfGpuView::clearAnnotVisuals() {
    m_annotVisuals.clear();
    update();
}

// ── Coordinate helpers ────────────────────────────────────────────────────────

void PdfGpuView::centerOnPageRect(const QRectF& rectDisp) {
    double pw = m_pageSizePt.width()  * m_zoom;
    double ph = m_pageSizePt.height() * m_zoom;
    QPointF c = rectDisp.center();
    m_panOffset = QPointF(pw / 2.0 - c.x() * m_zoom,
                          ph / 2.0 - c.y() * m_zoom);
    update();
}

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
        m_sharpPage = -1; m_sharpImage = {};
        scheduleTiles();
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
    if (m_hasImage) {
        if (e->button() == Qt::RightButton) {
            emit annotationContextRequested(m_pageIndex, widgetToPdf(e->position()), e->globalPosition().toPoint());
            return;
        }
    }
    if (m_tool == ViewTool::Pan && m_hasImage) {
        if (e->button() == Qt::LeftButton) {
            if (m_hasSel) {
                QPointF a = pdfToWidget(m_selRect.topLeft());
                QPointF b = pdfToWidget(m_selRect.bottomRight());
                QRectF wsel = QRectF(a, b).normalized().adjusted(-3, -3, 3, 3);
                if (wsel.contains(e->position())) {
                    m_draggingAnnot = true;
                    m_dragStart = e->position();
                    m_dragOrigRect = m_selRect;
                    m_dragPixelDelta = QPointF(0, 0);
                    m_dragNoteRect = m_dragOrigRect;
                    m_dragNoteOffsetPt = QPointF();
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
            m_tool == ViewTool::Cloud || m_tool == ViewTool::FreeText ||
            m_tool == ViewTool::Highlight) {
            m_drawingShape = true;
            m_shapeStart = e->position();
            m_shapeEnd = e->position();
            return;
        }
        if (m_tool == ViewTool::Freehand) {
            m_drawingFreehand = true;
            m_freehandPoints.clear();
            m_freehandPoints.append(widgetToPdf(e->position()));
            m_freehandLastWidgetPt = e->position();
            return;
        }
    }
    // Alt+Left drag = text selection for translation
    if ((e->modifiers() & Qt::AltModifier) && e->button() == Qt::LeftButton) {
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
        m_dragPixelDelta = e->position() - m_dragStart;
        m_dragNoteOffsetPt = d;
        update();
        return;
    }
    if (m_drawingShape) {
        m_shapeEnd = e->position();
        if ((e->modifiers() & Qt::ShiftModifier) &&
            (m_tool == ViewTool::Line || m_tool == ViewTool::Arrow)) {
            const double kPi = 3.14159265358979323846;
            QPointF d = m_shapeEnd - m_shapeStart;
            double len = std::hypot(d.x(), d.y());
            if (len > 0.0) {
                double ang = std::atan2(d.y(), d.x());
                double step = kPi / 4.0;
                double snapped = std::round(ang / step) * step;
                m_shapeEnd = m_shapeStart + QPointF(std::cos(snapped) * len,
                                                    std::sin(snapped) * len);
            }
        }
        update();
        return;
    }
    if (m_drawingFreehand) {
        QPointF cur = e->position();
        if (m_freehandPoints.isEmpty() || (cur - m_freehandLastWidgetPt).manhattanLength() > 2.0) {
            m_freehandPoints.append(widgetToPdf(cur));
            m_freehandLastWidgetPt = cur;
        }
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
        m_dragPixelDelta = QPointF();
        m_dragNoteRect = QRectF();
        m_dragNoteOffsetPt = QPointF();
        m_dragUid.clear();
        update();
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
                    case ViewTool::Highlight: at = AnnotTool::Highlight; break;
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
    if (m_drawingFreehand) {
        m_drawingFreehand = false;
        if (m_freehandPoints.size() >= 2)
            emit freehandCommitRequested(m_pageIndex, m_freehandPoints);
        m_freehandPoints.clear();
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

// ── Vector overlay ──────────────────────────────────────────────────────────

void PdfGpuView::setVectorLayer(std::shared_ptr<VectorLayer> layer) {
    qDebug().noquote() << "[vector] setVectorLayer page=" << m_pageIndex
                       << "ready=" << (layer && layer->isReady());
    m_vecLayer = layer;
    m_vecUploadedPage = -1;
    if (!layer && m_vecVao) {
        makeCurrent();
        QOpenGLExtraFunctions* glx = QOpenGLContext::currentContext()->extraFunctions();
        if (glx) glx->glDeleteVertexArrays(1, &m_vecVao);
        if (m_vecVboPos) glDeleteBuffers(1, &m_vecVboPos);
        if (m_vecVboCol) glDeleteBuffers(1, &m_vecVboCol);
        if (m_vecVboQuad) glDeleteBuffers(1, &m_vecVboQuad);
        if (m_vecVboWidth) glDeleteBuffers(1, &m_vecVboWidth);
        if (m_vecVboDepth) glDeleteBuffers(1, &m_vecVboDepth);
        if (m_vecVboClip) glDeleteBuffers(1, &m_vecVboClip);
        m_vecVao = 0; m_vecVboPos = 0; m_vecVboCol = 0; m_vecVboQuad = 0; m_vecVboWidth = 0; m_vecVboDepth = 0; m_vecVboClip = 0;
        if (glx && m_fillVao) glx->glDeleteVertexArrays(1, &m_fillVao);
        if (m_fillVboPos) glDeleteBuffers(1, &m_fillVboPos);
        if (m_fillVboCol) glDeleteBuffers(1, &m_fillVboCol);
        if (m_fillVboDepth) glDeleteBuffers(1, &m_fillVboDepth);
        if (m_fillVboClip) glDeleteBuffers(1, &m_fillVboClip);
        m_fillVao = 0; m_fillVboPos = 0; m_fillVboCol = 0; m_fillVboDepth = 0; m_fillVboClip = 0;
        for (GLuint t : m_tileTexText) if (t) glDeleteTextures(1, &t);
        for (GLuint t : m_tileTexImg) if (t) glDeleteTextures(1, &t);
        m_tileTexText.clear();
        m_tileTexImg.clear();
        doneCurrent();
    }
    update();
}

bool PdfGpuView::shouldUseVectorOverlay() const {
    const double onScreenW = m_pageSizePt.width() * m_zoom;
    bool result = m_vecLayer && m_vecLayer->isReady()
        && m_vecLayer->isComplete()
        && m_vecLayer->pageIndex() == m_pageIndex;
    if (result != m_vecLastOverlayState) {
        m_vecLastOverlayState = result;
        qDebug().noquote() << "[vector] overlay" << (result ? "ON" : "OFF")
                           << "zoom=" << m_zoom
                           << "page=" << m_pageIndex
                           << "onScreenW=" << onScreenW
                           << "texW=" << m_texW;
    }
    return result;
}

void PdfGpuView::drawVectorOverlay() {
    if (!m_vecLayer || !m_vecLayer->isReady()) return;
    if (m_vecLayer->widths().isEmpty() && m_vecLayer->fillVerts().isEmpty()) return;

    QOpenGLExtraFunctions* glx = QOpenGLContext::currentContext()->extraFunctions();
    if (!glx) return;

    // Upload VBOs once per page
    if (m_vecUploadedPage != m_vecLayer->pageIndex()) {
        if (m_vecVao) { glx->glDeleteVertexArrays(1, &m_vecVao); m_vecVao = 0; }
        if (m_vecVboPos) { glDeleteBuffers(1, &m_vecVboPos); m_vecVboPos = 0; }
        if (m_vecVboCol) { glDeleteBuffers(1, &m_vecVboCol); m_vecVboCol = 0; }
        if (m_vecVboQuad) { glDeleteBuffers(1, &m_vecVboQuad); m_vecVboQuad = 0; }
        if (m_vecVboWidth) { glDeleteBuffers(1, &m_vecVboWidth); m_vecVboWidth = 0; }
        if (m_vecVboDepth) { glDeleteBuffers(1, &m_vecVboDepth); m_vecVboDepth = 0; }
        if (m_vecVboClip) { glDeleteBuffers(1, &m_vecVboClip); m_vecVboClip = 0; }
        if (m_fillVao) { glx->glDeleteVertexArrays(1, &m_fillVao); m_fillVao = 0; }
        if (m_fillVboPos) { glDeleteBuffers(1, &m_fillVboPos); m_fillVboPos = 0; }
        if (m_fillVboCol) { glDeleteBuffers(1, &m_fillVboCol); m_fillVboCol = 0; }
        if (m_fillVboDepth) { glDeleteBuffers(1, &m_fillVboDepth); m_fillVboDepth = 0; }
        if (m_fillVboClip) { glDeleteBuffers(1, &m_fillVboClip); m_fillVboClip = 0; }

        // -- Fill VBOs --
        if (!m_vecLayer->fillVerts().isEmpty()) {
            glx->glGenVertexArrays(1, &m_fillVao);
            glx->glBindVertexArray(m_fillVao);

            glGenBuffers(1, &m_fillVboPos);
            glBindBuffer(GL_ARRAY_BUFFER, m_fillVboPos);
            glBufferData(GL_ARRAY_BUFFER, m_vecLayer->fillVerts().size() * sizeof(float),
                         m_vecLayer->fillVerts().constData(), GL_STATIC_DRAW);
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);

            glGenBuffers(1, &m_fillVboCol);
            glBindBuffer(GL_ARRAY_BUFFER, m_fillVboCol);
            glBufferData(GL_ARRAY_BUFFER, m_vecLayer->fillColors().size() * sizeof(uint8_t),
                         m_vecLayer->fillColors().constData(), GL_STATIC_DRAW);
            glEnableVertexAttribArray(1);
            glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, 4 * sizeof(uint8_t), nullptr);

            glGenBuffers(1, &m_fillVboDepth);
            glBindBuffer(GL_ARRAY_BUFFER, m_fillVboDepth);
            glBufferData(GL_ARRAY_BUFFER, m_vecLayer->fillDepths().size() * sizeof(float),
                         m_vecLayer->fillDepths().constData(), GL_STATIC_DRAW);
            glEnableVertexAttribArray(2);
            glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, sizeof(float), nullptr);

            if (!m_vecLayer->fillClipIdx().isEmpty()) {
                glGenBuffers(1, &m_fillVboClip);
                glBindBuffer(GL_ARRAY_BUFFER, m_fillVboClip);
                glBufferData(GL_ARRAY_BUFFER, m_vecLayer->fillClipIdx().size() * sizeof(float),
                             m_vecLayer->fillClipIdx().constData(), GL_STATIC_DRAW);
                glEnableVertexAttribArray(3);
                glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, sizeof(float), nullptr);
            }

            glx->glBindVertexArray(0);
        }

        // -- Stroke VBOs --
        if (!m_vecLayer->widths().isEmpty()) {
            glx->glGenVertexArrays(1, &m_vecVao);
            glx->glBindVertexArray(m_vecVao);

            static const float quadVerts[8] = { 0,0, 0,1, 1,0, 1,1 };
            glGenBuffers(1, &m_vecVboQuad);
            glBindBuffer(GL_ARRAY_BUFFER, m_vecVboQuad);
            glBufferData(GL_ARRAY_BUFFER, sizeof(quadVerts), quadVerts, GL_STATIC_DRAW);
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
            glx->glVertexAttribDivisor(0, 0);

            glGenBuffers(1, &m_vecVboPos);
            glBindBuffer(GL_ARRAY_BUFFER, m_vecVboPos);
            glBufferData(GL_ARRAY_BUFFER, m_vecLayer->verts().size() * sizeof(float),
                         m_vecLayer->verts().constData(), GL_STATIC_DRAW);
            glEnableVertexAttribArray(1);
            glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), nullptr);
            glx->glVertexAttribDivisor(1, 1);
            glEnableVertexAttribArray(2);
            glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                                  reinterpret_cast<void*>(2 * sizeof(float)));
            glx->glVertexAttribDivisor(2, 1);

            glGenBuffers(1, &m_vecVboCol);
            glBindBuffer(GL_ARRAY_BUFFER, m_vecVboCol);
            glBufferData(GL_ARRAY_BUFFER, m_vecLayer->colors().size() * sizeof(uint8_t),
                         m_vecLayer->colors().constData(), GL_STATIC_DRAW);
            glEnableVertexAttribArray(3);
            glVertexAttribPointer(3, 4, GL_UNSIGNED_BYTE, GL_TRUE, 4 * sizeof(uint8_t), nullptr);
            glx->glVertexAttribDivisor(3, 1);

            glGenBuffers(1, &m_vecVboWidth);
            glBindBuffer(GL_ARRAY_BUFFER, m_vecVboWidth);
            glBufferData(GL_ARRAY_BUFFER, m_vecLayer->widths().size() * sizeof(float),
                         m_vecLayer->widths().constData(), GL_STATIC_DRAW);
            glEnableVertexAttribArray(4);
            glVertexAttribPointer(4, 1, GL_FLOAT, GL_FALSE, sizeof(float), nullptr);
            glx->glVertexAttribDivisor(4, 1);

            glGenBuffers(1, &m_vecVboDepth);
            glBindBuffer(GL_ARRAY_BUFFER, m_vecVboDepth);
            glBufferData(GL_ARRAY_BUFFER, m_vecLayer->depths().size() * sizeof(float),
                         m_vecLayer->depths().constData(), GL_STATIC_DRAW);
            glEnableVertexAttribArray(5);
            glVertexAttribPointer(5, 1, GL_FLOAT, GL_FALSE, sizeof(float), nullptr);
            glx->glVertexAttribDivisor(5, 1);

            if (!m_vecLayer->clipIdx().isEmpty()) {
                glGenBuffers(1, &m_vecVboClip);
                glBindBuffer(GL_ARRAY_BUFFER, m_vecVboClip);
                glBufferData(GL_ARRAY_BUFFER, m_vecLayer->clipIdx().size() * sizeof(float),
                             m_vecLayer->clipIdx().constData(), GL_STATIC_DRAW);
                glEnableVertexAttribArray(6);
                glVertexAttribPointer(6, 1, GL_FLOAT, GL_FALSE, sizeof(float), nullptr);
                glx->glVertexAttribDivisor(6, 1);
            }

            glx->glBindVertexArray(0);
        }

        m_vecUploadedPage = m_vecLayer->pageIndex();
    }

    GLboolean blendWasOn = glIsEnabled(GL_BLEND);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);
    glClear(GL_DEPTH_BUFFER_BIT);

    const QPointF pgOrig = pageOrigin();
    const double  pgW = m_pageSizePt.width()  * m_zoom;
    const double  pgH = m_pageSizePt.height() * m_zoom;
    const qreal   dpr = devicePixelRatioF();
    const int sx = int(std::floor(pgOrig.x() * dpr));
    const int sy = int(std::floor((height() - (pgOrig.y() + pgH)) * dpr));
    const int sw = int(std::ceil(pgW * dpr));
    const int sh = int(std::ceil(pgH * dpr));
    GLint prevScissor[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_SCISSOR_BOX, prevScissor);
    const GLboolean prevScissorOn = glIsEnabled(GL_SCISSOR_TEST);
    glEnable(GL_SCISSOR_TEST);
    glScissor(sx, sy, qMax(0, sw), qMax(0, sh));

    QMatrix4x4 mvp = vectorTransform();

    auto uploadClips = [&](QOpenGLShaderProgram* prog) {
        if (!prog) return;
        const QVector<QRectF>& cl = m_vecLayer->clips();
        for (int i = 0; i < 64; ++i) {
            const QRectF r = (i < cl.size()) ? cl[i] : QRectF();
            prog->setUniformValue(QString("uClips[%1]").arg(i).toUtf8().constData(),
                                  QVector4D(float(r.x()), float(r.y()), float(r.width()), float(r.height())));
        }
    };

    // -- Draw fills BEFORE strokes --
    if (m_fillProg && m_fillVao && !m_vecLayer->fillVerts().isEmpty()) {
        m_fillProg->bind();
        m_fillProg->setUniformValue(m_fillMvpLoc, mvp);
        uploadClips(m_fillProg);
        glx->glBindVertexArray(m_fillVao);
        glDrawArrays(GL_TRIANGLES, 0, m_vecLayer->fillVerts().size() / 2);
        glx->glBindVertexArray(0);
        m_fillProg->release();
    }

    // -- Draw strokes --
    if (m_vecProg && m_vecVao && !m_vecLayer->widths().isEmpty()) {
        m_vecProg->bind();
        m_vecProg->setUniformValue(m_vecMvpLoc, mvp);
        uploadClips(m_vecProg);
        const QSizeF vp = m_vecLayer->pageSizePt();
        const double vpRef = (m_vecLayer->rotation() & 1) ? vp.height() : vp.width();
        const float pxPerPt = (vpRef > 0.0)
            ? float(m_pageSizePt.width() * m_zoom / vpRef) : float(m_zoom);
        glUniform2f(m_vecViewportLoc, float(width()), float(height()));
        glUniform1f(m_vecPxPerPtLoc, pxPerPt);

        glx->glBindVertexArray(m_vecVao);
        glx->glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, m_vecLayer->widths().size());
        glx->glBindVertexArray(0);

        if (!m_vecDrawLogged) {
            m_vecDrawLogged = true;
            qDebug().noquote() << "[vector] draw OK page=" << m_vecLayer->pageIndex()
                               << "segs=" << m_vecLayer->widths().size();
        }

        m_vecProg->release();
    }

    auto drawTiles = [&](const QVector<TextTile>& tiles, QVector<GLuint>& texs) {
        if (tiles.isEmpty()) return;
        const quint32 gen = m_vecLayer->tilesGeneration();
        if (texs.size() != tiles.size() || m_tileTexGen != gen) {
            for (GLuint t : texs) if (t) glDeleteTextures(1, &t);
            texs.fill(0, tiles.size());
        }
        // tt.rectPt nam o khong gian CHUA xoay cua VectorLayer. Nghich dao mvp de lay
        // vung nhin DUNG khong gian do (mvp da gom ca phep /Rotate).
        QMatrix4x4 invMvp = mvp.inverted();
        auto projectNdc = [&](float cx, float cy) -> QPointF {
            QVector4D v = invMvp * QVector4D(cx, cy, 0, 1);
            return (v.w() != 0.f) ? QPointF(v.x() / v.w(), v.y() / v.w()) : QPointF();
        };
        const QRectF visPt = QRectF(projectNdc(-1.f, 1.f), projectNdc(1.f, -1.f)).normalized();
        for (int i = 0; i < tiles.size(); ++i) {
            const TextTile& tt = tiles[i];
            if (!visPt.intersects(tt.rectPt)) continue;
            QRectF drawRect = tt.rectPt;
            if (m_draggingAnnot && !m_dragNoteRect.isNull() && tt.isNote && m_dragNoteRect.intersects(tt.rectPt)) {
                drawRect.translate(m_dragNoteOffsetPt);
            }
            if (texs[i] == 0) {
                glGenTextures(1, &texs[i]);
                glBindTexture(GL_TEXTURE_2D, texs[i]);
                glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
                if (tt.isAlpha) {
                    const QImage& src = tt.img;
                    glPixelStorei(GL_UNPACK_ROW_LENGTH, src.bytesPerLine());
                    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, src.width(), src.height(), 0,
                                 GL_RED, GL_UNSIGNED_BYTE, src.constBits());
                    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
                } else {
                    QImage src = tt.img.convertToFormat(QImage::Format_RGBA8888);
                    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, src.width(), src.height(), 0,
                                 GL_RGBA, GL_UNSIGNED_BYTE, src.constBits());
                }
                glGenerateMipmap(GL_TEXTURE_2D);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            }
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, texs[i]);
            m_tileProg->setUniformValue(m_tileTexLoc, 0);
            glUniform1i(m_tileIsAlphaLoc, tt.isAlpha ? 1 : 0);
            glUniform4f(m_tileColorLoc, qRed(tt.color)/255.0f, qGreen(tt.color)/255.0f,
                        qBlue(tt.color)/255.0f, 1.0f);
            glUniform4f(m_tileRectLoc, float(drawRect.x()), float(drawRect.y()),
                        float(drawRect.width()), float(drawRect.height()));
            glUniform1f(m_tileDepthLoc, tt.depth);
            glUniform1f(m_tileProg->uniformLocation("uClipIdx"), tt.clipIdx);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        }
    };
    if (m_tileProg && m_tileVao) {
        m_tileProg->bind();
        glUniformMatrix4fv(m_tileMvpLoc, 1, GL_FALSE, mvp.constData());
        uploadClips(m_tileProg);
        glx->glBindVertexArray(m_tileVao);
        drawTiles(m_vecLayer->imageTiles(), m_tileTexImg);
        drawTiles(m_vecLayer->textTiles(),  m_tileTexText);
        m_tileTexGen = m_vecLayer->tilesGeneration();
        glx->glBindVertexArray(0);
        m_tileProg->release();
    }

    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);

    if (prevScissorOn) glScissor(prevScissor[0], prevScissor[1], prevScissor[2], prevScissor[3]);
    else               glDisable(GL_SCISSOR_TEST);

    if (!blendWasOn) glDisable(GL_BLEND);
}
