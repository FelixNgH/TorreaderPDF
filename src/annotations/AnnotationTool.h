#pragma once
#include <QObject>
#include "annotations/AnnotationTypes.h"

// Manages the active annotation tool state and bridges toolbar ↔ PDF view.
class AnnotationTool : public QObject {
    Q_OBJECT
public:
    explicit AnnotationTool(QObject* parent = nullptr);

    AnnotTool   activeTool()  const { return m_tool; }
    AnnotStyle  activeStyle() const { return m_style; }

public slots:
    void setTool(int tool);
    void setStyle(const AnnotStyle& style);

signals:
    void toolChanged(AnnotTool tool, const AnnotStyle& style);

private:
    AnnotTool  m_tool  = AnnotTool::None;
    AnnotStyle m_style;
};
