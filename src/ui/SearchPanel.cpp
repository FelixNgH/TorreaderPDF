#include "SearchPanel.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QLabel>

SearchPanel::SearchPanel(QWidget* parent) : QWidget(parent) {
    m_input     = new QLineEdit(this);
    m_searchBtn = new QPushButton("Search", this);
    m_results   = new QListWidget(this);
    m_countLabel= new QLabel(this);

    m_input->setPlaceholderText("Search text… (Ctrl+F)");
    m_results->setAlternatingRowColors(true);

    auto* topRow = new QHBoxLayout;
    topRow->addWidget(m_input, 1);
    topRow->addWidget(m_searchBtn);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->addLayout(topRow);
    layout->addWidget(m_results, 1);
    layout->addWidget(m_countLabel);

    connect(m_searchBtn, &QPushButton::clicked, this, [this]{
        clearResults();
        emit searchRequested(m_input->text());
    });
    connect(m_input, &QLineEdit::returnPressed, m_searchBtn, &QPushButton::click);
    connect(m_results, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
        if (!item) return;
        int page = item->data(Qt::UserRole).toInt();
        QRectF rect = item->data(Qt::UserRole + 1).toRectF();
        emit resultSelected(page, rect);
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
    item->setData(Qt::UserRole + 1, result.boundingBox);
    ++m_count;
    m_countLabel->setText(QString("%1 result(s)").arg(m_count));
}

void SearchPanel::clearResults() {
    m_results->clear();
    m_countLabel->clear();
    m_count = 0;
}
