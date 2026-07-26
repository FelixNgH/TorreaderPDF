#pragma once
#include <QWidget>
#include <QListWidget>
#include <QRectF>
#include "core/TextSearch.h"

class QLineEdit;
class QPushButton;
class QLabel;

class SearchPanel : public QWidget {
    Q_OBJECT
public:
    explicit SearchPanel(QWidget* parent = nullptr);

    void focusInput();

public slots:
    void addResult(const SearchResult& result);
    void clearResults();
    void setSearchProgress(int pagesScanned, int totalPages);

signals:
    void searchRequested(const QString& query);
    void resultSelected(int pageIndex, QRectF boundingBox);
    void searchCleared();


private:
    QLineEdit*   m_input      = nullptr;
    QPushButton* m_searchBtn  = nullptr;
    QListWidget* m_results    = nullptr;
    QLabel*      m_countLabel = nullptr;
    int          m_count      = 0;
};
