#include "NotificationBar.h"
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSettings>
#include <QPropertyAnimation>

NotificationBar::NotificationBar(QWidget* parent)
    : QFrame(parent)
{
    setFixedWidth(300);
    setObjectName("notifBar");
    setStyleSheet(
        "QFrame#notifBar { background:white; border:1px solid #E2E8F0;"
        "  border-radius:12px; }"
        "QPushButton#notifClose { background:transparent; color:#5C5C5C;"
        "  border:none; border-radius:4px; font-size:14px; font-weight:bold; }"
        "QPushButton#notifClose:hover { background:#E5E5E5; color:#1A1A1A; }"
    );
    hide();

    auto* icon = new QLabel("\xF0\x9F\x94\x94", this); // 🔔
    icon->setStyleSheet("font-size:14px;");
    icon->setFixedWidth(20);

    m_title = new QLabel(this);
    m_title->setStyleSheet("font-weight:bold; font-size:10pt;");

    m_closeBtn = new QPushButton("\xC3\x97", this);  // ×
    m_closeBtn->setObjectName("notifClose");
    m_closeBtn->setFixedSize(24, 24);
    m_closeBtn->setCursor(Qt::PointingHandCursor);
    m_closeBtn->setToolTip("Dismiss");

    auto* hdr = new QHBoxLayout;
    hdr->setSpacing(6);
    hdr->addWidget(icon);
    hdr->addWidget(m_title, 1);
    hdr->addWidget(m_closeBtn);

    m_body = new QLabel(this);
    m_body->setWordWrap(true);
    m_body->setStyleSheet("font-size:9pt; color:#1a1a1a;");

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(10, 10, 10, 10);
    root->setSpacing(6);
    root->addLayout(hdr);
    root->addWidget(m_body);
    setLayout(root);

    connect(m_closeBtn, &QPushButton::clicked, this, &NotificationBar::onDismiss);
}

void NotificationBar::setContent(const QString& title, const QString& body) {
    m_title->setText(title);
    m_body->setText(body);
}

void NotificationBar::showNotification() {
    adjustSize();
    show();
    raise();
}

bool NotificationBar::wasDismissed(const QString& notifId) {
    QSettings s;
    return s.value("notifications/dismissed/" + notifId, false).toBool();
}

void NotificationBar::markDismissed(const QString& notifId) {
    QSettings s;
    s.setValue("notifications/dismissed/" + notifId, true);
}

void NotificationBar::onDismiss() {
    emit dismissed();
    hide();
}
