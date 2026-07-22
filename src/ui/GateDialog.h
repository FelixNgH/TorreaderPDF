#pragma once
#include <QDialog>
#include <QCloseEvent>

class QWidget;
class QLabel;
class QPushButton;
class QTimer;

class GateDialog : public QDialog {
    Q_OBJECT
public:
    explicit GateDialog(const QString& title, const QString& body,
                        bool blocking, QWidget* parent = nullptr);

protected:
    void closeEvent(QCloseEvent* e) override;
    void reject() override;

private:
    void onTick();

    QLabel* m_countdownLbl = nullptr;
    QPushButton* m_exitBtn = nullptr;
    QTimer* m_countdownTimer = nullptr;
    int m_secondsLeft = 10;
    bool m_blocking;
};
