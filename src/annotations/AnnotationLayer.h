#pragma once
#include <QObject>
#include <QPointF>
#include <QVector>
#include <QImage>
#include "annotations/AnnotationTypes.h"
#include "annotations/AnnotationManager.h"
#include <fpdfview.h>
#include <fpdf_annot.h>

class AnnotationManager;

class AnnotationLayer : public QObject {
    Q_OBJECT
public:
    explicit AnnotationLayer(QObject* parent = nullptr);
    void setDocument(FPDF_DOCUMENT doc);
    void setAnnotationManager(AnnotationManager* mgr) { m_annotMgr = mgr; }
    void commitAnnotation(int pageIndex, AnnotTool tool, const AnnotStyle& style,
                          QPointF start, QPointF end, const QVector<QPointF>& freehand);
    // Insert Image (SPEC_INSERT_IMAGE_2026-08-30): chen PNG trong suot thanh Stamp
    // annot. rectDisp o toa do hien thi. Tra TRUID neu xong, rong neu that bai.
    QString insertStampImage(int pageIndex, const QImage& image, QRectF rectDisp);
    QString lastCreatedUid() const { return m_lastCreatedUid; }
    int lastCreatedIndex() const { return m_lastCreatedIndex; }
    AnnotSnapshot lastCreatedSnapshot() const { return m_lastCreatedSnapshot; }

signals:
    void annotationAdded(int pageIndex);
    void annotVisualAdded(int page, const AnnotVisual& av);

private:
    FPDF_DOCUMENT m_doc = nullptr;
    AnnotationManager* m_annotMgr = nullptr;
    QString m_lastCreatedUid;
    int m_lastCreatedIndex = -1;
    AnnotSnapshot m_lastCreatedSnapshot;
};
