#include "GateDialog.h"
#include "core/UpdateChecker.h"
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QDesktopServices>
#include <QUrl>
#include <QApplication>
#include <QPixmap>
#include <QTimer>
#include <QDebug>

GateDialog::GateDialog(const QString& title, const QString& body,
                       bool blocking, QWidget* parent)
    : QDialog(parent), m_blocking(blocking)
{
    setWindowTitle(blocking ? "Update Required" : "Update Available");
    setModal(true);
    resize(460, 300);
    setMinimumSize(460, 300);
    // Remove close button (X) from title bar in ALL cases
    setWindowFlags(windowFlags() & ~Qt::WindowCloseButtonHint);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(20, 20, 20, 20);
    root->setSpacing(12);

    // ── Header: icon + title ──────────────────────────────────────────────
    auto* hdr = new QHBoxLayout;
    auto* icon = new QLabel(this);
    icon->setPixmap(QPixmap(":/icons/TorReader.ico")
                        .scaled(32, 32, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    icon->setFixedSize(32, 32);
    QString displayTitle = title.isEmpty()
        ? (blocking ? "Update Required" : "Update Available")
        : title;
    auto* titleLbl = new QLabel(displayTitle, this);
    titleLbl->setStyleSheet("font-size:14pt; font-weight:bold;");
    hdr->addWidget(icon);
    hdr->addWidget(titleLbl, 1);
    root->addLayout(hdr);

    // ── Body text ─────────────────────────────────────────────────────────
    QString displayBody = body.isEmpty()
        ? "A new version is available.\nPlease download the latest version."
        : body;
    auto* bodyLbl = new QLabel(displayBody, this);
    bodyLbl->setWordWrap(true);
    bodyLbl->setStyleSheet("font-size:10pt;");
    root->addWidget(bodyLbl);

    // ── Countdown label ───────────────────────────────────────────────────
    m_countdownLbl = new QLabel(this);
    m_countdownLbl->setAlignment(Qt::AlignCenter);
    m_countdownLbl->setStyleSheet("font-size:10pt; color:#888;");
    root->addWidget(m_countdownLbl);

    root->addStretch();

    // ── Buttons ───────────────────────────────────────────────────────────
    auto* btnRow = new QHBoxLayout;
    btnRow->addStretch();

    auto* downloadBtn = new QPushButton("Download new version", this);
    downloadBtn->setDefault(true);
    connect(downloadBtn, &QPushButton::clicked, this, [this]() {
        qDebug() << "[gate] opening" << UpdateChecker::kDownloadPageUrl;
        QDesktopServices::openUrl(QUrl(UpdateChecker::kDownloadPageUrl));
        if (!m_blocking) accept();
    });
    btnRow->addWidget(downloadBtn);

    // Exit button (blocking only, hidden until countdown finishes)
    m_exitBtn = new QPushButton("Exit", this);
    m_exitBtn->setVisible(false);
    connect(m_exitBtn, &QPushButton::clicked, this, [this]() {
        qDebug() << "[gate] blocking exit";
        qApp->quit();
    });
    btnRow->addWidget(m_exitBtn);

    root->addLayout(btnRow);

    // ── Countdown timer ───────────────────────────────────────────────────
    m_secondsLeft = 10;
    m_countdownLbl->setText(
        QString("This window will close in %1 seconds…").arg(m_secondsLeft));
    m_countdownTimer = new QTimer(this);
    m_countdownTimer->setInterval(1000);
    connect(m_countdownTimer, &QTimer::timeout, this, &GateDialog::onTick);
    m_countdownTimer->start();

    qDebug() << "[gate] auto-close countdown started" << m_secondsLeft << "s";
    qDebug() << "[gate] dialog created blocking=" << (blocking ? 1 : 0);
}

void GateDialog::onTick() {
    --m_secondsLeft;
    if (m_secondsLeft > 0) {
        if (m_blocking)
            m_countdownLbl->setText(
                QString("Auto-close disabled — this window will close in %1 seconds…").arg(m_secondsLeft));
        else
            m_countdownLbl->setText(
                QString("This window will close in %1 seconds…").arg(m_secondsLeft));
        return;
    }

    m_countdownTimer->stop();
    qDebug() << "[gate] auto-closed after countdown";

    if (m_blocking) {
        m_countdownLbl->setText("Please download the new version or exit the application.");
        m_exitBtn->setVisible(true);
        qDebug() << "[gate] blocking — showing Exit button, waiting for user";
    } else {
        accept();
    }
}

void GateDialog::closeEvent(QCloseEvent* e) {
    qDebug() << "[gate] closeEvent ignored (blocking=" << (m_blocking ? 1 : 0) << ")";
    e->ignore();
}

void GateDialog::reject() {
    qDebug() << "[gate] Esc pressed — ignored (blocking=" << (m_blocking ? 1 : 0) << ")";
}
