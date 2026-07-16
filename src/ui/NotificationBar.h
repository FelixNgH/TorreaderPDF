#pragma once
#include <QFrame>
#include <QString>

class QLabel;
class QPushButton;

class NotificationBar : public QFrame {
    Q_OBJECT
public:
    explicit NotificationBar(QWidget* parent = nullptr);
    void setContent(const QString& title, const QString& body);
    void showNotification();

    static bool wasDismissed(const QString& notifId);
    static void markDismissed(const QString& notifId);

signals:
    void dismissed();

private slots:
    void onDismiss();

private:
    QLabel*       m_title;
    QLabel*       m_body;
    QPushButton*  m_closeBtn;
};
