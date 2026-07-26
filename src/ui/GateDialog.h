#pragma once
#include <QDialog>
#include <QCloseEvent>

class QWidget;
class QLabel;
class QPushButton;

class GateDialog : public QDialog {
    Q_OBJECT
public:
    explicit GateDialog(const QString& title, const QString& body,
                        bool blocking, QWidget* parent = nullptr);

protected:
    void closeEvent(QCloseEvent* e) override;
    void reject() override;

private:
    QPushButton* m_exitBtn = nullptr;
    bool m_blocking;
};
