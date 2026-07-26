#pragma once
#include <QWidget>
#include <QList>
#include <QRectF>
#include <QString>

class QLineEdit;
class QLabel;
class QPushButton;
class QCheckBox;
class QTimer;

// Floating find bar, shown at top-right of the viewport via Ctrl+F.
// Shares the same TextSearch instance as the sidebar SearchPanel.
class FindBar : public QWidget {
    Q_OBJECT
public:
    explicit FindBar(QWidget* parent = nullptr);

    void reset();
    void setFoundCount(int total);
    void setCurrentMatch(int index);

signals:
    void searchRequested(const QString& query, Qt::CaseSensitivity cs);
    void navigateNext();
    void navigatePrev();
    void clearSearchHighlights();
    void findBarOpened();
    void findBarClosed();

public slots:
    void showAndFocus();
    void onFound();
    void onSearchComplete(int total);
    void closeBar();

private slots:
    void onTextChanged(const QString& text);
    void onReturnPressed();
    void onNextClicked();
    void onPrevClicked();

private:
    void updateLabel();
    void triggerSearch();

    QLineEdit*   m_input      = nullptr;
    QLabel*      m_matchLabel = nullptr;
    QPushButton* m_prevBtn    = nullptr;
    QPushButton* m_nextBtn    = nullptr;
    QCheckBox*   m_matchCase  = nullptr;
    QPushButton* m_closeBtn   = nullptr;

    QTimer*      m_debounce   = nullptr;
    int          m_matchCount = 0;
    int          m_matchCurrent = 0;
};
