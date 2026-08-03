#include "VectorGpuRenderer.h"
#include <QOpenGLExtraFunctions>
#include <QOpenGLContext>
#include <QDebug>
#include <QRectF>
#include <cmath>

void VectorGpuRenderer::initialize()
{
    initializeOpenGLFunctions();

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
    m_vecProg = new QOpenGLShaderProgram();
    m_vecProg->addShaderFromSourceCode(QOpenGLShader::Vertex, vecVsrc);
    m_vecProg->addShaderFromSourceCode(QOpenGLShader::Fragment, vecFsrc);
    if (!m_vecProg->link()) {
        qDebug() << "[vgr] vector shader link failed:" << m_vecProg->log();
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
    m_fillProg = new QOpenGLShaderProgram();
    m_fillProg->addShaderFromSourceCode(QOpenGLShader::Vertex, fillVsrc);
    m_fillProg->addShaderFromSourceCode(QOpenGLShader::Fragment, fillFsrc);
    if (!m_fillProg->link()) {
        qDebug() << "[vgr] fill shader link failed:" << m_fillProg->log();
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
    m_tileProg = new QOpenGLShaderProgram();
    m_tileProg->addShaderFromSourceCode(QOpenGLShader::Vertex, tileVsrc);
    m_tileProg->addShaderFromSourceCode(QOpenGLShader::Fragment, tileFsrc);
    if (!m_tileProg->link()) {
        qDebug() << "[vgr] tile shader link failed:" << m_tileProg->log();
        delete m_tileProg; m_tileProg = nullptr;
    } else {
        m_tileMvpLoc = m_tileProg->uniformLocation("uMvp");
        m_tileRectLoc = m_tileProg->uniformLocation("uRect");
        m_tileDepthLoc = m_tileProg->uniformLocation("uDepth");
        m_tileTexLoc = m_tileProg->uniformLocation("uTex");
        m_tileIsAlphaLoc = m_tileProg->uniformLocation("uIsAlpha");
        m_tileColorLoc = m_tileProg->uniformLocation("uColor");
    }
}

void VectorGpuRenderer::release()
{
    for (auto it = m_bufs.begin(); it != m_bufs.end(); ++it) {
        destroyBuffers(it.value());
    }
    m_bufs.clear();
    m_usedKeys.clear();
    if (m_vecProg) { delete m_vecProg; m_vecProg = nullptr; }
    if (m_fillProg) { delete m_fillProg; m_fillProg = nullptr; }
    if (m_tileProg) { delete m_tileProg; m_tileProg = nullptr; }
}

void VectorGpuRenderer::destroyBuffers(Buffers& b)
{
    QOpenGLExtraFunctions* glx = QOpenGLContext::currentContext()->extraFunctions();
    if (!glx) return;
    if (b.vao) glx->glDeleteVertexArrays(1, &b.vao);
    if (b.vboPos) glDeleteBuffers(1, &b.vboPos);
    if (b.vboCol) glDeleteBuffers(1, &b.vboCol);
    if (b.vboQuad) glDeleteBuffers(1, &b.vboQuad);
    if (b.vboW) glDeleteBuffers(1, &b.vboW);
    if (b.vboDepth) glDeleteBuffers(1, &b.vboDepth);
    if (b.vboClip) glDeleteBuffers(1, &b.vboClip);
    if (b.fillVao) glx->glDeleteVertexArrays(1, &b.fillVao);
    if (b.fillPos) glDeleteBuffers(1, &b.fillPos);
    if (b.fillCol) glDeleteBuffers(1, &b.fillCol);
    if (b.fillDepth) glDeleteBuffers(1, &b.fillDepth);
    if (b.fillClip) glDeleteBuffers(1, &b.fillClip);
    if (b.tileVao) glx->glDeleteVertexArrays(1, &b.tileVao);
    for (GLuint t : b.texText) if (t) glDeleteTextures(1, &t);
    for (GLuint t : b.texImg) if (t) glDeleteTextures(1, &t);
    b = Buffers{};
}

void VectorGpuRenderer::uploadBuffers(VectorLayer& layer, Buffers& buf)
{
    QOpenGLExtraFunctions* glx = QOpenGLContext::currentContext()->extraFunctions();
    if (!glx) return;

    if (buf.vao) { glx->glDeleteVertexArrays(1, &buf.vao); buf.vao = 0; }
    if (buf.vboPos) { glDeleteBuffers(1, &buf.vboPos); buf.vboPos = 0; }
    if (buf.vboCol) { glDeleteBuffers(1, &buf.vboCol); buf.vboCol = 0; }
    if (buf.vboQuad) { glDeleteBuffers(1, &buf.vboQuad); buf.vboQuad = 0; }
    if (buf.vboW) { glDeleteBuffers(1, &buf.vboW); buf.vboW = 0; }
    if (buf.vboDepth) { glDeleteBuffers(1, &buf.vboDepth); buf.vboDepth = 0; }
    if (buf.vboClip) { glDeleteBuffers(1, &buf.vboClip); buf.vboClip = 0; }
    if (buf.fillVao) { glx->glDeleteVertexArrays(1, &buf.fillVao); buf.fillVao = 0; }
    if (buf.fillPos) { glDeleteBuffers(1, &buf.fillPos); buf.fillPos = 0; }
    if (buf.fillCol) { glDeleteBuffers(1, &buf.fillCol); buf.fillCol = 0; }
    if (buf.fillDepth) { glDeleteBuffers(1, &buf.fillDepth); buf.fillDepth = 0; }
    if (buf.fillClip) { glDeleteBuffers(1, &buf.fillClip); buf.fillClip = 0; }

    if (!layer.fillVerts().isEmpty()) {
        glx->glGenVertexArrays(1, &buf.fillVao);
        glx->glBindVertexArray(buf.fillVao);

        glGenBuffers(1, &buf.fillPos);
        glBindBuffer(GL_ARRAY_BUFFER, buf.fillPos);
        glBufferData(GL_ARRAY_BUFFER, layer.fillVerts().size() * sizeof(float),
                     layer.fillVerts().constData(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);

        glGenBuffers(1, &buf.fillCol);
        glBindBuffer(GL_ARRAY_BUFFER, buf.fillCol);
        glBufferData(GL_ARRAY_BUFFER, layer.fillColors().size() * sizeof(uint8_t),
                     layer.fillColors().constData(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, 4 * sizeof(uint8_t), nullptr);

        glGenBuffers(1, &buf.fillDepth);
        glBindBuffer(GL_ARRAY_BUFFER, buf.fillDepth);
        glBufferData(GL_ARRAY_BUFFER, layer.fillDepths().size() * sizeof(float),
                     layer.fillDepths().constData(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, sizeof(float), nullptr);

        if (!layer.fillClipIdx().isEmpty()) {
            glGenBuffers(1, &buf.fillClip);
            glBindBuffer(GL_ARRAY_BUFFER, buf.fillClip);
            glBufferData(GL_ARRAY_BUFFER, layer.fillClipIdx().size() * sizeof(float),
                         layer.fillClipIdx().constData(), GL_STATIC_DRAW);
            glEnableVertexAttribArray(3);
            glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, sizeof(float), nullptr);
        }

        glx->glBindVertexArray(0);
        buf.fillVerts = layer.fillVerts().size() / 2;
    }

    if (!layer.widths().isEmpty()) {
        glx->glGenVertexArrays(1, &buf.vao);
        glx->glBindVertexArray(buf.vao);

        static const float quadVerts[8] = { 0,0, 0,1, 1,0, 1,1 };
        glGenBuffers(1, &buf.vboQuad);
        glBindBuffer(GL_ARRAY_BUFFER, buf.vboQuad);
        glBufferData(GL_ARRAY_BUFFER, sizeof(quadVerts), quadVerts, GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
        glx->glVertexAttribDivisor(0, 0);

        glGenBuffers(1, &buf.vboPos);
        glBindBuffer(GL_ARRAY_BUFFER, buf.vboPos);
        glBufferData(GL_ARRAY_BUFFER, layer.verts().size() * sizeof(float),
                     layer.verts().constData(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), nullptr);
        glx->glVertexAttribDivisor(1, 1);
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                              reinterpret_cast<void*>(2 * sizeof(float)));
        glx->glVertexAttribDivisor(2, 1);

        glGenBuffers(1, &buf.vboCol);
        glBindBuffer(GL_ARRAY_BUFFER, buf.vboCol);
        glBufferData(GL_ARRAY_BUFFER, layer.colors().size() * sizeof(uint8_t),
                     layer.colors().constData(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(3);
        glVertexAttribPointer(3, 4, GL_UNSIGNED_BYTE, GL_TRUE, 4 * sizeof(uint8_t), nullptr);
        glx->glVertexAttribDivisor(3, 1);

        glGenBuffers(1, &buf.vboW);
        glBindBuffer(GL_ARRAY_BUFFER, buf.vboW);
        glBufferData(GL_ARRAY_BUFFER, layer.widths().size() * sizeof(float),
                     layer.widths().constData(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(4);
        glVertexAttribPointer(4, 1, GL_FLOAT, GL_FALSE, sizeof(float), nullptr);
        glx->glVertexAttribDivisor(4, 1);

        glGenBuffers(1, &buf.vboDepth);
        glBindBuffer(GL_ARRAY_BUFFER, buf.vboDepth);
        glBufferData(GL_ARRAY_BUFFER, layer.depths().size() * sizeof(float),
                     layer.depths().constData(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(5);
        glVertexAttribPointer(5, 1, GL_FLOAT, GL_FALSE, sizeof(float), nullptr);
        glx->glVertexAttribDivisor(5, 1);

        if (!layer.clipIdx().isEmpty()) {
            glGenBuffers(1, &buf.vboClip);
            glBindBuffer(GL_ARRAY_BUFFER, buf.vboClip);
            glBufferData(GL_ARRAY_BUFFER, layer.clipIdx().size() * sizeof(float),
                         layer.clipIdx().constData(), GL_STATIC_DRAW);
            glEnableVertexAttribArray(6);
            glVertexAttribPointer(6, 1, GL_FLOAT, GL_FALSE, sizeof(float), nullptr);
            glx->glVertexAttribDivisor(6, 1);
        }

        glx->glBindVertexArray(0);
        buf.segs = layer.widths().size();
    }

    if (!buf.tileVao) {
        glx->glGenVertexArrays(1, &buf.tileVao);
        glx->glBindVertexArray(buf.tileVao);
        GLuint tileVbo;
        static const float tileQuad[] = { 0,0, 0,1, 1,0, 1,1 };
        glGenBuffers(1, &tileVbo);
        glBindBuffer(GL_ARRAY_BUFFER, tileVbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(tileQuad), tileQuad, GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
        glx->glBindVertexArray(0);
    }

    buf.uploaded = true;
}

void VectorGpuRenderer::draw(VectorLayer& layer, const QMatrix4x4& mvp,
                             const QSize& viewportPx, float pxPerPt)
{
    if (!layer.isReady()) return;
    if (layer.widths().isEmpty() && layer.fillVerts().isEmpty() &&
        layer.textTiles().isEmpty() && layer.imageTiles().isEmpty()) return;

    QOpenGLExtraFunctions* glx = QOpenGLContext::currentContext()->extraFunctions();
    if (!glx) return;

    const quint64 uid = layer.uid();
    Buffers& buf = m_bufs[uid];
    m_usedKeys.insert(uid);

    const quint32 gen = layer.tilesGeneration();
    const int expectedSegs = layer.widths().size();
    const int expectedFillVerts = layer.fillVerts().size() / 2;

    const bool needRebuild = !buf.uploaded
                          || buf.segs != expectedSegs
                          || buf.fillVerts != expectedFillVerts
                          || buf.tilesGen != gen;

    if (needRebuild) {
        qDebug() << "[vgr] rebuild buffers uid=" << uid
                 << "segs=" << expectedSegs
                 << "fillVerts=" << expectedFillVerts
                 << "tilesGen=" << gen;
        destroyBuffers(buf);
        uploadBuffers(layer, buf);
        buf.tilesGen = gen;
    }

    if (m_bufs.size() > 6) {
        auto it = m_bufs.begin();
        while (it != m_bufs.end()) {
            if (!m_usedKeys.contains(it.key())) {
                destroyBuffers(it.value());
                it = m_bufs.erase(it);
            } else {
                ++it;
            }
        }
        m_usedKeys.clear();
        m_usedKeys.insert(uid);
    }

    GLboolean blendWasOn = glIsEnabled(GL_BLEND);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);
    glClear(GL_DEPTH_BUFFER_BIT);

    auto uploadClips = [&](QOpenGLShaderProgram* prog) {
        if (!prog) return;
        const QVector<QRectF>& cl = layer.clips();
        for (int i = 0; i < 64; ++i) {
            const QRectF r = (i < cl.size()) ? cl[i] : QRectF();
            prog->setUniformValue(QString("uClips[%1]").arg(i).toUtf8().constData(),
                                  QVector4D(float(r.x()), float(r.y()), float(r.width()), float(r.height())));
        }
    };

    if (m_fillProg && buf.fillVao && !layer.fillVerts().isEmpty()) {
        m_fillProg->bind();
        m_fillProg->setUniformValue(m_fillMvpLoc, mvp);
        uploadClips(m_fillProg);
        glx->glBindVertexArray(buf.fillVao);
        glDrawArrays(GL_TRIANGLES, 0, buf.fillVerts);
        glx->glBindVertexArray(0);
        m_fillProg->release();
    }

    if (m_vecProg && buf.vao && !layer.widths().isEmpty()) {
        m_vecProg->bind();
        m_vecProg->setUniformValue(m_vecMvpLoc, mvp);
        uploadClips(m_vecProg);
        glUniform2f(m_vecViewportLoc, float(viewportPx.width()), float(viewportPx.height()));
        glUniform1f(m_vecPxPerPtLoc, pxPerPt);

        glx->glBindVertexArray(buf.vao);
        glx->glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, buf.segs);
        glx->glBindVertexArray(0);
        m_vecProg->release();
    }

    auto drawTilesLocal = [&](const QVector<TextTile>& tiles, QVector<GLuint>& texs) {
        if (tiles.isEmpty()) return;
        if (texs.size() != tiles.size()) {
            for (GLuint t : texs) if (t) glDeleteTextures(1, &t);
            texs.fill(0, tiles.size());
        }
        QMatrix4x4 inv = mvp.inverted();
        auto project = [&](float cx, float cy) -> QPointF {
            QVector4D v = inv * QVector4D(cx, cy, 0, 1);
            return (v.w() != 0.f) ? QPointF(v.x() / v.w(), v.y() / v.w()) : QPointF();
        };
        QRectF visPt = QRectF(project(-1.f, 1.f), project(1.f, -1.f)).normalized();

        for (int i = 0; i < tiles.size(); ++i) {
            const TextTile& tt = tiles[i];
            if (!visPt.intersects(tt.rectPt)) continue;
            QRectF drawRect = tt.rectPt;
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

    if (m_tileProg && buf.tileVao) {
        m_tileProg->bind();
        glUniformMatrix4fv(m_tileMvpLoc, 1, GL_FALSE, mvp.constData());
        uploadClips(m_tileProg);
        glx->glBindVertexArray(buf.tileVao);
        drawTilesLocal(layer.imageTiles(), buf.texImg);
        drawTilesLocal(layer.textTiles(), buf.texText);
        glx->glBindVertexArray(0);
        m_tileProg->release();
    }

    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    if (!blendWasOn) glDisable(GL_BLEND);

    {
        static QHash<quint64, quint32> s_logged;
        quint32 loggedGen = s_logged.value(uid, 0xFFFFFFFFu);
        if (loggedGen != gen) {
            s_logged[uid] = gen;
            qDebug() << "[vgr] draw page-layer uid=" << uid
                     << "segs=" << buf.segs
                     << "fills=" << buf.fillVerts
                     << "tiles=" << (layer.textTiles().size() + layer.imageTiles().size());
        }
    }
}
