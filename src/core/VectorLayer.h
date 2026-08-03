#pragma once
#include <QVector>
#include <QSizeF>
#include <QRectF>
#include <QImage>
#include <QtGlobal>
#include <atomic>
#include <fpdfview.h>

struct TextTile {
    QRectF rectPt;
    QImage img;
    float depth = 1.0f;
    float clipIdx = 0.0f;
    QRgb color = qRgb(0,0,0);
    bool isAlpha = false;
    bool isNote = false;
};

class VectorLayer {
public:
    VectorLayer();
    bool build(FPDF_DOCUMENT doc, int pageIndex);
    int rebuildNoteTiles(FPDF_DOCUMENT doc, FPDF_PAGE page);
    bool isReady() const { return m_ready; }
    bool isComplete() const { return m_complete; }
    int  pageIndex() const { return m_page; }
    quint64 uid() const { return m_uid; }
    void clear();
    const QVector<float>&   verts()  const { return m_verts; }
    const QVector<uint8_t>& colors() const { return m_colors; }
    const QVector<float>&   widths() const { return m_widths; }
    const QVector<TextTile>& textTiles() const { return m_texts; }
    const QVector<float>&   fillVerts()  const { return m_fillVerts; }
    const QVector<uint8_t>& fillColors() const { return m_fillColors; }
    const QVector<float>&   depths()     const { return m_depths; }
    const QVector<float>&   fillDepths() const { return m_fillDepths; }
    const QVector<TextTile>& imageTiles() const { return m_images; }
    const QVector<QRectF>&  clips() const { return m_clips; }
    const QVector<float>&   clipIdx() const { return m_clipIdx; }
    const QVector<float>&   fillClipIdx() const { return m_fillClipIdx; }
    // Kich thuoc hop crop CHUA xoay (khong gian toa do cua m_verts/rectPt/m_clips)
    QSizeF pageSizePt() const { return m_pageSize; }
    // So vong xoay cua trang: 0=0deg, 1=90, 2=180, 3=270 (theo /Rotate)
    int rotation() const { return m_rotation; }
    int translateNoteTiles(const QRectF& hitRect, const QPointF& d);
    quint32 tilesGeneration() const { return m_tilesGen; }
private:
    quint64 m_uid;
    bool    m_ready = false;
    bool    m_complete = false;
    int     m_page  = -1;
    int     m_rotation = 0;
    QSizeF  m_pageSize;
    QVector<float>   m_verts;
    QVector<uint8_t> m_colors;
    QVector<float>   m_widths;
    QVector<TextTile> m_texts;
    QVector<float>   m_fillVerts;
    QVector<uint8_t> m_fillColors;
    QVector<float>   m_depths;
    QVector<float>   m_fillDepths;
    QVector<TextTile> m_images;
    QVector<QRectF>  m_clips;
    QVector<float>   m_clipIdx;
    QVector<float>   m_fillClipIdx;
    quint32 m_tilesGen = 0;
    QVector<int> m_noteObjIdx;
    int m_buildObjCount = 0;
};
