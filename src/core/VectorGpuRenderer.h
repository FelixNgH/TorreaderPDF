#pragma once
#include <QHash>
#include <QSet>
#include <QVector>
#include <QSize>
#include <QMatrix4x4>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include "core/VectorLayer.h"

class VectorGpuRenderer : protected QOpenGLFunctions {
public:
    void initialize();
    void release();
    void draw(VectorLayer& layer, const QMatrix4x4& mvp, const QSize& viewportPx, float pxPerPt);

private:
    struct Buffers {
        GLuint vao = 0, vboPos = 0, vboCol = 0, vboQuad = 0, vboW = 0, vboDepth = 0, vboClip = 0;
        GLuint fillVao = 0, fillPos = 0, fillCol = 0, fillDepth = 0, fillClip = 0;
        GLuint tileVao = 0;
        QVector<GLuint> texText, texImg;
        quint32 tilesGen = 0xFFFFFFFFu;
        int segs = 0, fillVerts = 0;
        bool uploaded = false;
    };
    QHash<quint64, Buffers> m_bufs;
    QSet<quint64> m_usedKeys;

    QOpenGLShaderProgram* m_vecProg = nullptr;
    int m_vecMvpLoc = -1, m_vecViewportLoc = -1, m_vecPxPerPtLoc = -1;

    QOpenGLShaderProgram* m_fillProg = nullptr;
    int m_fillMvpLoc = -1;

    QOpenGLShaderProgram* m_tileProg = nullptr;
    int m_tileMvpLoc = -1, m_tileRectLoc = -1, m_tileDepthLoc = -1, m_tileTexLoc = -1;
    int m_tileIsAlphaLoc = -1, m_tileColorLoc = -1;

    void uploadBuffers(VectorLayer& layer, Buffers& buf);
    void destroyBuffers(Buffers& buf);
};
