#pragma once
#include <QWidget>
#include <QListWidget>
#include <QRectF>
#include <QLineEdit>
#include "core/TextSearch.h"

// QVariant ho tro san QList<QRectF> nhung can khai bao metatype de
// setData(value)/value<T>() dich duoc kieu.
Q_DECLARE_METATYPE(QList<QRectF>)

class QLineEdit;
class QPushButton;
class QLabel;
class QCheckBox;

class SearchPanel : public QWidget {
    Q_OBJECT
public:
    explicit SearchPanel(QWidget* parent = nullptr);

    void focusInput();

    // Xoa CA o nhap + danh sach + nhan dem (khac clearResults: chi xoa danh sach).
    void reset();
    // Gan truy van vao o nhap (dung khi nap lai trang thai cua tab cu).
    void setQuery(const QString& query);
    // Probe (nghiem thu bang so): so ket qua + truy van dang hien thi.
    int     probeCount() const { return m_count; }
    QString probeQuery() const { return m_input ? m_input->text() : QString(); }

public slots:
    void addResult(const SearchResult& result);
    void clearResults();
    void setSearchProgress(int pagesScanned, int totalPages);

signals:
    void searchRequested(const QString& query, bool matchDiacritics);
    void resultSelected(int pageIndex, QList<QRectF> rects);
    void searchCleared();


private:
    QLineEdit*   m_input             = nullptr;
    QPushButton* m_searchBtn         = nullptr;
    QListWidget* m_results           = nullptr;
    QLabel*      m_countLabel        = nullptr;
    QCheckBox*   m_matchDiacritics   = nullptr;
    int          m_count             = 0;
};
