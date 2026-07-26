#include "FindBar.h"
#include <QHBoxLayout>
#include <QLineEdit>
#include <QLabel>
#include <QPushButton>
#include <QCheckBox>
#include <QTimer>

FindBar::FindBar(QWidget* parent)
    : QWidget(parent)
{
    setObjectName("findBar");
    setFixedHeight(36);
    setAutoFillBackground(true);

    m_input     = new QLineEdit(this);
    m_matchLabel = new QLabel("0 / 0", this);
    m_prevBtn   = new QPushButton(QStringLiteral("\u25B2"), this); // ▲
    m_nextBtn   = new QPushButton(QStringLiteral("\u25BC"), this); // ▼
    m_matchCase = new QCheckBox("Aa", this);
    m_closeBtn  = new QPushButton(QStringLiteral("\u2715"), this); // ✕

    m_input->setPlaceholderText("Find in document\u2026");
    m_input->setClearButtonEnabled(true);
    m_input->setFixedWidth(200);
    m_matchLabel->setFixedWidth(60);
    m_matchLabel->setAlignment(Qt::AlignCenter);
    m_prevBtn->setFixedWidth(28);
    m_nextBtn->setFixedWidth(28);
    m_matchCase->setToolTip("Match case");
    m_closeBtn->setFixedWidth(28);

    auto* lay = new QHBoxLayout(this);
    lay->setContentsMargins(6, 2, 6, 2);
    lay->setSpacing(4);
    lay->addWidget(m_input);
    lay->addWidget(m_matchLabel);
    lay->addWidget(m_prevBtn);
    lay->addWidget(m_nextBtn);
    lay->addWidget(m_matchCase);
    lay->addWidget(m_closeBtn);

    connect(m_input, &QLineEdit::returnPressed, this, &FindBar::onReturnPressed);
    connect(m_input, &QLineEdit::textChanged, this, &FindBar::onTextChanged);
    connect(m_prevBtn, &QPushButton::clicked, this, &FindBar::onPrevClicked);
    connect(m_nextBtn, &QPushButton::clicked, this, &FindBar::onNextClicked);
    connect(m_closeBtn, &QPushButton::clicked, this, &FindBar::closeBar);
    connect(m_matchCase, &QCheckBox::toggled, this, [this]() {
        if (!m_input->text().isEmpty())
            onTextChanged(m_input->text());
    });

    m_debounce = new QTimer(this);
    m_debounce->setSingleShot(true);
    m_debounce->setInterval(300);
    connect(m_debounce, &QTimer::timeout, this, &FindBar::triggerSearch);

    hide();
}

void FindBar::reset() {
    m_matchCount = 0;
    m_matchCurrent = 0;
    m_input->clear();
    updateLabel();
}

void FindBar::setFoundCount(int total) {
    m_matchCount = total;
    m_matchCurrent = 0;
    updateLabel();
}

void FindBar::setCurrentMatch(int index) {
    m_matchCurrent = qBound(0, index, m_matchCount - 1);
    updateLabel();
}

void FindBar::showAndFocus() {
    show();
    raise();
    m_input->setFocus();
    m_input->selectAll();
    emit findBarOpened();
}

void FindBar::closeBar() {
    hide();
    m_matchCount = 0;
    m_matchCurrent = 0;
    updateLabel();
    emit clearSearchHighlights();
    emit findBarClosed();
}

void FindBar::onFound() {
    ++m_matchCount;
    m_matchLabel->setText(QString("… / %1").arg(m_matchCount));
}

void FindBar::onSearchComplete(int total) {
    m_matchCount = total;
    m_matchCurrent = 0;
    updateLabel();
}

void FindBar::onTextChanged(const QString& text) {
    if (text.isEmpty()) {
        m_debounce->stop();
        m_matchCount = 0;
        m_matchCurrent = 0;
        updateLabel();
        emit clearSearchHighlights();
        return;
    }
    m_debounce->start();
}

void FindBar::triggerSearch() {
    QString q = m_input->text().trimmed();
    if (q.isEmpty()) return;
    m_matchCount = 0;
    m_matchCurrent = 0;
    updateLabel();
    emit searchRequested(q, m_matchCase->isChecked() ? Qt::CaseSensitive : Qt::CaseInsensitive);
}

void FindBar::onReturnPressed() {
    if (!isVisible()) return;
    if (m_input->text().trimmed().isEmpty()) return;
    // If we haven't run a search yet (e.g. user typed and pressed Enter before debounce)
    if (m_matchCount == 0) {
        triggerSearch();
        // navigate next will happen after searchComplete
        return;
    }
    emit navigateNext();
}

void FindBar::onNextClicked() {
    if (m_matchCount == 0) return;
    emit navigateNext();
}

void FindBar::onPrevClicked() {
    if (m_matchCount == 0) return;
    emit navigatePrev();
}

void FindBar::updateLabel() {
    if (m_matchCount == 0) {
        m_matchLabel->setText("0 / 0");
        m_input->setStyleSheet(m_input->text().isEmpty() ? "" : "border: 1px solid red;");
    } else {
        m_matchLabel->setText(QString("%1 / %2").arg(m_matchCurrent + 1).arg(m_matchCount));
        m_input->setStyleSheet("");
    }
}
