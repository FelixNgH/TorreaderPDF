#pragma once
#include <QFrame>
#include <QString>
#include <functional>

class QLabel;
class QPushButton;
class QHBoxLayout;

// Thanh thong bao MOT HANG dung cho popup OCR (xem NotificationBar.cpp).
// Mau tu ThemeTokens qua QSS global; can setObjectName("ocrBar") o noi dung.
class NotificationBar : public QFrame {
    Q_OBJECT
public:
    explicit NotificationBar(QWidget* parent = nullptr);
    void setContent(const QString& title, const QString& body);
    // Nut hanh dong chinh (VD "Recognize"). Khong co thi khong hien nut.
    void setActionButton(const QString& text, std::function<void()> onClick);
    void showNotification();

    static bool wasDismissed(const QString& notifId);
    static void markDismissed(const QString& notifId);

signals:
    void dismissed();

private slots:
    void onDismiss();

private:
    QLabel*       m_label;
    QPushButton*  m_actionBtn = nullptr;
    QPushButton*  m_closeBtn;
    QHBoxLayout*  m_root;
};
