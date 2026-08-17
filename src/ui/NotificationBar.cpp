#include "NotificationBar.h"
#include <QLabel>
#include <QPushButton>
#include <QHBoxLayout>

// Thanh thong bao MOT HANG (SPEC_FINAL_6 muc 6): chu ngan + nut hanh dong +
// nut x. Mau lay tu ThemeTokens qua QSS global (buildQss trong MainWindow):
// objectName "ocrBar" duoc buildQss dinh nghia nen tu dong theo theme.
NotificationBar::NotificationBar(QWidget* parent)
    : QFrame(parent)
{
    setObjectName("ocrBar");
    setFixedHeight(28);

    m_label = new QLabel(this);
    m_label->setTextFormat(Qt::PlainText);

    m_actionBtn = nullptr;   // tao khi setActionButton

    m_closeBtn = new QPushButton(QString::fromUtf8("\xC3\x97"), this);  // ×
    m_closeBtn->setObjectName("ocrClose");
    m_closeBtn->setFixedSize(24, 24);
    m_closeBtn->setToolTip("Dismiss");
    m_closeBtn->setCursor(Qt::PointingHandCursor);

    m_root = new QHBoxLayout(this);
    m_root->setContentsMargins(8, 0, 4, 0);
    m_root->setSpacing(6);
    m_root->addWidget(m_label, 1);
    m_root->addWidget(m_closeBtn);

    connect(m_closeBtn, &QPushButton::clicked, this, &NotificationBar::onDismiss);
    hide();
}

void NotificationBar::setActionButton(const QString& text, std::function<void()> onClick) {
    if (!m_actionBtn) {
        m_actionBtn = new QPushButton(this);
        m_actionBtn->setObjectName("ocrAction");
        m_actionBtn->setCursor(Qt::PointingHandCursor);
        m_root->insertWidget(m_root->count() - 1, m_actionBtn);   // truoc nut x
    }
    m_actionBtn->setText(text);
    m_actionBtn->show();
    QObject::connect(m_actionBtn, &QPushButton::clicked, this, [onClick]{ onClick(); });
    adjustSize();
}

void NotificationBar::setContent(const QString& title, const QString& body) {
    // Mot hang duy nhat: chi hien body; title bo (khong dung o popup OCR).
    Q_UNUSED(title)
    m_label->setText(body);
}

void NotificationBar::showNotification() {
    adjustSize();
    show();
    raise();
}

bool NotificationBar::wasDismissed(const QString& notifId) {
    Q_UNUSED(notifId)
    return false;
}

void NotificationBar::markDismissed(const QString& notifId) {
    Q_UNUSED(notifId)
}

void NotificationBar::onDismiss() {
    emit dismissed();
    hide();
}
