#include "TranslationPopup.h"
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPainter>
#include <QClipboard>
#include <QGuiApplication>
#include <QScreen>
#include <QPropertyAnimation>

TranslationPopup::TranslationPopup(QWidget* parent)
    : QWidget(parent,
              Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint)
{
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setFixedWidth(340);

    m_lblOriginal = new QLabel(this);
    m_lblOriginal->setWordWrap(true);
    m_lblOriginal->setStyleSheet("color:#666; font-size:9pt; font-style:italic;");

    m_lblTranslated = new QLabel(this);
    m_lblTranslated->setWordWrap(true);
    m_lblTranslated->setStyleSheet("font-weight:bold; font-size:11pt; color:#1a1a1a;");

    m_lblCopied = new QLabel("Copied!", this);
    m_lblCopied->setStyleSheet("color:#2E8B57; font-size:9pt;");
    m_lblCopied->setVisible(false);

    m_btnCopy  = new QPushButton("Copy", this);
    m_btnCopy->setFlat(true);
    m_btnCopy->setCursor(Qt::PointingHandCursor);

    m_btnClose = new QPushButton("\xC3\x97", this);
    m_btnClose->setCursor(Qt::PointingHandCursor);
    m_btnClose->setFixedSize(20, 20);
    m_btnClose->setStyleSheet(
        "QPushButton {"
        "  color:#6B7280; font-size:13px; font-weight:bold;"
        "  border:none; border-radius:4px; padding:0; background:transparent; }"
        "QPushButton:hover { background:#E5E7EB; color:#111827; }");

    auto* hdr = new QHBoxLayout;
    hdr->addWidget(m_btnCopy);
    hdr->addWidget(m_lblCopied);
    hdr->addStretch();
    hdr->addWidget(m_btnClose);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(14, 12, 14, 12);
    layout->setSpacing(6);
    layout->addWidget(m_lblOriginal);
    layout->addWidget(m_lblTranslated);
    layout->addLayout(hdr);
    setLayout(layout);

    m_autoClose = new QTimer(this);
    m_autoClose->setSingleShot(true);
    connect(m_autoClose, &QTimer::timeout, this, &QWidget::hide);
    connect(m_btnCopy,  &QPushButton::clicked, this, &TranslationPopup::onCopy);
    connect(m_btnClose, &QPushButton::clicked, this, &QWidget::hide);
}

void TranslationPopup::showTranslation(const QString& original,
                                        const QString& translation,
                                        const QPoint& globalPos)
{
    m_lblOriginal->setText(original);
    m_lblTranslated->setText(translation.isEmpty()
                             ? "<i>(no translation)</i>" : translation);
    m_translation = translation;
    m_lblCopied->setVisible(false);

    adjustSize();

    QRect screen = QGuiApplication::primaryScreen()->availableGeometry();
    int x = qBound(screen.left(), globalPos.x() - width() / 2,
                   screen.right()  - width());
    int y = globalPos.y() - height() - 12;
    if (y < screen.top()) y = globalPos.y() + 12;
    move(x, y);

    setWindowOpacity(0.0);
    show();
    raise();

    auto* anim = new QPropertyAnimation(this, "windowOpacity", this);
    anim->setDuration(150);
    anim->setStartValue(0.0);
    anim->setEndValue(1.0);
    anim->start(QAbstractAnimation::DeleteWhenStopped);

    m_autoClose->start(6000);
}

void TranslationPopup::onCopy() {
    QGuiApplication::clipboard()->setText(m_translation);
    m_lblCopied->setVisible(true);
    QTimer::singleShot(1200, this, [this] { m_lblCopied->setVisible(false); });
}

void TranslationPopup::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setBrush(QColor(255, 255, 255, 245));
    p.setPen(QPen(QColor(200, 200, 200), 1));
    p.drawRoundedRect(rect().adjusted(1, 1, -1, -1), 8, 8);
}
