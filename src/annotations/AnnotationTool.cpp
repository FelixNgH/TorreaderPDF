#include "AnnotationTool.h"

AnnotationTool::AnnotationTool(QObject* parent) : QObject(parent) {}

void AnnotationTool::setTool(int tool) {
    AnnotTool newTool = static_cast<AnnotTool>(tool);
    if (m_tool == newTool)
        return;
    m_tool = newTool;
    emit toolChanged(m_tool, m_style);
}

void AnnotationTool::setStyle(const AnnotStyle& style) {
    m_style = style;
    emit toolChanged(m_tool, m_style);
}
