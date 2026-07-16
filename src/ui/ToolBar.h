#pragma once
#include <QToolBar>
#include "annotations/AnnotationTypes.h"

class QActionGroup;
class QComboBox;
class QSpinBox;

class ToolBar : public QToolBar {
    Q_OBJECT
public:
    explicit ToolBar(QWidget* parent = nullptr);
    void setDocumentOpen(bool open);

signals:
    void toolChanged(int tool);
    void styleChanged(AnnotStyle style);

private:
    QActionGroup* m_group    = nullptr;
    QComboBox*    m_colorBox = nullptr;
    QSpinBox*     m_widthBox = nullptr;
    AnnotStyle    m_style;

    AnnotStyle currentStyle() const;
};
