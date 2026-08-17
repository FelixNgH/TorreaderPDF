#include "SearchPanel.h"
#include "KeylogProbe.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QLabel>
#include <QCheckBox>
#include <QSignalBlocker>

SearchPanel::SearchPanel(QWidget* parent) : QWidget(parent) {
    m_input     = new QLineEdit(this);
    installKeylogProbe(m_input);
    m_searchBtn = new QPushButton("Search", this);
    m_results   = new QListWidget(this);
    m_countLabel= new QLabel(this);
    m_matchDiacritics = new QCheckBox("Match diacritics", this);
    m_matchDiacritics->setToolTip(
        "When off, search ignores Vietnamese diacritics (MAT finds MẶT)");

    m_input->setPlaceholderText("Search text… (Ctrl+F)");
    m_input->setClearButtonEnabled(true);
    m_results->setAlternatingRowColors(true);

    auto* topRow = new QHBoxLayout;
    topRow->addWidget(m_input, 1);
    topRow->addWidget(m_searchBtn);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->addLayout(topRow);
    layout->addWidget(m_matchDiacritics);
    layout->addWidget(m_results, 1);
    layout->addWidget(m_countLabel);

    connect(m_searchBtn, &QPushButton::clicked, this, [this]{
        clearResults();
        emit searchRequested(m_input->text(), m_matchDiacritics->isChecked());
    });
    connect(m_input, &QLineEdit::returnPressed, m_searchBtn, &QPushButton::click);
    connect(m_matchDiacritics, &QCheckBox::toggled, this, [this](bool){
        if (!m_input->text().isEmpty()) {
            clearResults();
            emit searchRequested(m_input->text(), m_matchDiacritics->isChecked());
        }
    });
    connect(m_results, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
        if (!item) return;
        int page = item->data(Qt::UserRole).toInt();
        QList<QRectF> rects = item->data(Qt::UserRole + 1).value<QList<QRectF>>();
        emit resultSelected(page, rects);
    });
    connect(m_input, &QLineEdit::textChanged, this, [this](const QString& t) {
        if (t.isEmpty()) {
            clearResults();
            emit searchCleared();
        }
    });
}

void SearchPanel::focusInput() {
    m_input->setFocus();
    m_input->selectAll();
}

void SearchPanel::addResult(const SearchResult& result) {
    QString label = QString("Page %1: %2")
                        .arg(result.pageIndex + 1)
                        .arg(result.contextSnippet.left(60));
    auto* item = new QListWidgetItem(label, m_results);
    item->setData(Qt::UserRole, result.pageIndex);
    item->setData(Qt::UserRole + 1, QVariant::fromValue(result.rects));
    ++m_count;
    m_countLabel->setText(QString("%1 result(s)").arg(m_count));
}

void SearchPanel::setSearchProgress(int pagesScanned, int totalPages) {
    m_countLabel->setText(QString("Scanning… %1 / %2").arg(pagesScanned).arg(totalPages));
}

void SearchPanel::clearResults() {
    m_results->clear();
    m_countLabel->clear();
    m_count = 0;
}

void SearchPanel::reset() {
    // QSignalBlocker de reset tu ma khong phat searchCleared (chi khi NGUOI
    // DUNG xoa ky tu moi phat) — tranh xoa nham ket qua tab moi dang nap lai.
    { const QSignalBlocker b(m_input); m_input->clear(); }
    m_results->clear();
    m_countLabel->clear();
    m_count = 0;
}

void SearchPanel::setQuery(const QString& query) {
    const QSignalBlocker b(m_input);
    m_input->setText(query);
}
