#include "UpdateBanner.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QDesktopServices>
#include <QUrl>
#include <QGraphicsDropShadowEffect>
#include <QStringList>

UpdateBanner::UpdateBanner(QWidget* parent) : QFrame(parent) {
    setFixedSize(320, 130);
    setObjectName("updateCard");
    hide();

    // Drop shadow
    auto* shadow = new QGraphicsDropShadowEffect(this);
    shadow->setBlurRadius(24);
    shadow->setOffset(0, 4);
    shadow->setColor(QColor(0, 0, 0, 60));
    setGraphicsEffect(shadow);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(18, 14, 14, 16);
    root->setSpacing(10);

    // ── Header: icon + title + X ──────────────────────────────────────────
    auto* hdr = new QHBoxLayout;
    hdr->setSpacing(8);

    auto* icon = new QLabel("\xF0\x9F\x94\x94"); // 🔔
    icon->setStyleSheet("font-size:16px;");
    hdr->addWidget(icon);

    auto* title = new QLabel("Update Available");
    title->setObjectName("updateTitle");
    title->setStyleSheet("font-weight:600; font-size:13px;");
    hdr->addWidget(title, 1);

    auto* closeBtn = new QPushButton("\xC3\x97"); // ×
    closeBtn->setObjectName("updateClose");
    closeBtn->setFixedSize(24, 24);
    closeBtn->setCursor(Qt::PointingHandCursor);
    closeBtn->setToolTip("Dismiss");
    connect(closeBtn, &QPushButton::clicked, this, &QWidget::hide);
    hdr->addWidget(closeBtn);

    root->addLayout(hdr);

    // ── Message ───────────────────────────────────────────────────────────
    m_label = new QLabel;
    m_label->setObjectName("updateMsg");
    m_label->setWordWrap(true);
    m_label->setStyleSheet("font-size:12px;");
    root->addWidget(m_label);

    m_bodyLabel = new QLabel;
    m_bodyLabel->setObjectName("updateBody");
    m_bodyLabel->setWordWrap(true);
    m_bodyLabel->setTextFormat(Qt::RichText);
    m_bodyLabel->setStyleSheet("font-size:12px;");
    root->addWidget(m_bodyLabel);
    m_bodyLabel->hide();

    // ── Download button ───────────────────────────────────────────────────
    auto* dlBtn = new QPushButton("\xE2\xAC\x87 Download Now"); // ⬇
    dlBtn->setObjectName("updateDl");
    dlBtn->setFixedHeight(30);
    dlBtn->setCursor(Qt::PointingHandCursor);
    connect(dlBtn, &QPushButton::clicked, this, [this]() {
        QDesktopServices::openUrl(QUrl(m_downloadUrl));
    });
    root->addWidget(dlBtn);

    applyTheme(false);
}


void UpdateBanner::showUpdate(const QString& version, const QString& title,
                              const QString& body, const QString& downloadUrl) {
    m_downloadUrl = downloadUrl;

    QString heading = title.trimmed();
    if (heading.isEmpty())
        heading = QStringLiteral("Update Available");
    if (auto* titleLabel = findChild<QLabel*>("updateTitle"))
        titleLabel->setText(heading);

    m_label->setText(QStringLiteral("Version <b>%1</b> is now available.").arg(version));

    m_bodyLabel->setTextFormat(Qt::RichText);
    m_bodyLabel->setWordWrap(true);
    m_bodyLabel->setOpenExternalLinks(true);
    if (body.trimmed().isEmpty()) {
        m_bodyLabel->clear();
        m_bodyLabel->hide();
    } else {
        m_bodyLabel->setText(body);
        m_bodyLabel->show();
    }

    show();
    raise();
}

void UpdateBanner::applyTheme(bool dark) {
    if (dark) {
        setStyleSheet(
            "QFrame#updateCard  { background:#1E293B; border:1px solid #334155;"
            "                     border-radius:12px; }"
            "QLabel#updateTitle { color:#F1F5F9; }"
            "QLabel#updateMsg   { color:#94A3B8; }"
            "QLabel#updateBody { color:#94A3B8; font-size:12px; }"
            "QPushButton#updateDl { background:#1AEBFF; color:#000000; border:1px solid #1AEBFF;"
            "  border-radius:6px; font-size:12px; font-weight:600; }"
            "QPushButton#updateDl:hover { border:1px solid #F38518; }"
            "QPushButton#updateClose { background:transparent; color:#64748B;"
            "  border:none; border-radius:4px; font-size:14px; font-weight:bold; }"
            "QPushButton#updateClose:hover { background:#334155; color:#F1F5F9; }"
        );
    } else {
        setStyleSheet(
            "QFrame#updateCard  { background:white; border:1px solid #E2E8F0;"
            "                     border-radius:12px; }"
            "QLabel#updateTitle { color:#1E293B; }"
            "QLabel#updateMsg   { color:#64748B; }"
            "QLabel#updateBody { color:#64748B; font-size:12px; }"
            "QPushButton#updateDl { background:#0F4A85; color:#FFFFFF; border:1px solid #0F4A85;"
            "  border-radius:6px; font-size:12px; font-weight:600; }"
            "QPushButton#updateDl:hover { border:1px solid #006BB3; }"
            "QPushButton#updateClose { background:transparent; color:#94A3B8;"
            "  border:none; border-radius:4px; font-size:14px; font-weight:bold; }"
            "QPushButton#updateClose:hover { background:#F1F5F9; color:#1E293B; }"
        );
    }
}
