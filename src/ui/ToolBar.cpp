#include "ToolBar.h"
#include <QActionGroup>
#include <QComboBox>
#include <QSpinBox>
#include <QLabel>

// Parallel array to AnnotTool enum — order must match AnnotationTypes.h
static const struct { AnnotTool tool; const char* label; } kTools[] = {
    { AnnotTool::None,          "Select"      },
    { AnnotTool::TextComment,   "Comment"     },
    { AnnotTool::Highlight,     "Highlight"   },
    { AnnotTool::Underline,     "Underline"   },
    { AnnotTool::Strikethrough, "Strikethrough"},
    { AnnotTool::Arrow,         "Arrow"       },
    { AnnotTool::Rectangle,     "Rectangle"   },
    { AnnotTool::Ellipse,       "Ellipse"     },
    { AnnotTool::Line,          "Line"        },
    { AnnotTool::Freehand,      "Freehand"    },
    { AnnotTool::Stamp,         "Stamp"       },
};

ToolBar::ToolBar(QWidget* parent) : QToolBar(parent) {
    setWindowTitle("Annotations");
    m_group = new QActionGroup(this);
    m_group->setExclusive(true);

    for (const auto& t : kTools) {
        auto* act = addAction(t.label);
        act->setCheckable(true);
        act->setData(static_cast<int>(t.tool));
        m_group->addAction(act);
        connect(act, &QAction::triggered, this, [this, act]{
            emit toolChanged(act->data().toInt());
        });
    }
    m_group->actions().first()->setChecked(true); // default: Select

    addSeparator();
    addWidget(new QLabel(" Color:", this));

    m_colorBox = new QComboBox(this);
    m_colorBox->addItems({"Red", "Blue", "Black", "Green"});
    addWidget(m_colorBox);

    addWidget(new QLabel(" Width:", this));
    m_widthBox = new QSpinBox(this);
    m_widthBox->setRange(1, 10);
    m_widthBox->setValue(2);
    addWidget(m_widthBox);

    auto emitStyle = [this]{ emit styleChanged(currentStyle()); };
    connect(m_colorBox, &QComboBox::currentTextChanged, this, emitStyle);
    connect(m_widthBox, &QSpinBox::valueChanged, this, emitStyle);
}

void ToolBar::setDocumentOpen(bool open) {
    const auto acts = m_group->actions();
    for (int i = 1; i < acts.size(); ++i)
        acts[i]->setEnabled(open);
    m_colorBox->setEnabled(open);
    m_widthBox->setEnabled(open);
}

AnnotStyle ToolBar::currentStyle() const {
    AnnotStyle s;
    // fix: use strokeColor/strokeWidth per AnnotationTypes.h, not color/lineWidth
    const QString c = m_colorBox->currentText();
    s.strokeColor = (c == "Red")   ? Qt::red   :
                    (c == "Blue")  ? Qt::blue  :
                    (c == "Green") ? Qt::green : Qt::black;
    s.strokeWidth = static_cast<float>(m_widthBox->value());
    return s;
}
