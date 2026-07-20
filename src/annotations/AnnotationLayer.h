#pragma once
#include <QObject>
#include <QPointF>
#include <QVector>
#include "annotations/AnnotationTypes.h"
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

signals:
    void annotationAdded(int pageIndex);

private:
    FPDF_DOCUMENT m_doc = nullptr;
    AnnotationManager* m_annotMgr = nullptr;
};
