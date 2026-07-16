#pragma once
#include <QColor>
#include <QPointF>
#include <QRectF>
#include <QString>

enum class AnnotTool {
    None,
    TextComment,   // sticky note
    Highlight,
    Underline,
    Strikethrough,
    Arrow,
    Rectangle,
    Ellipse,
    Line,
    Freehand,
    Stamp,
};

struct AnnotStyle {
    QColor strokeColor = Qt::red;
    QColor fillColor   = Qt::transparent;
    float  strokeWidth = 2.0f;
    float  opacity     = 1.0f;
    QString stampText; // for Stamp tool
};
