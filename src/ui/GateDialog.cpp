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
#include <QDebug>

GateDialog::GateDialog(const QString& title, const QString& body,
                       bool blocking, QWidget* parent)
    : QDialog(parent), m_blocking(blocking)
{
    setWindowTitle(blocking ? "Update Required" : "Update Available");
    setModal(true);
    resize(460, 300);
    setMinimumSize(460, 300);
    if (blocking)
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
        ? "A new version is available.<br>Please download from "
          "<a href=\"https://torreader.cloud\">torreader.cloud</a>."
        : body;
    auto* bodyLbl = new QLabel(displayBody, this);
    bodyLbl->setWordWrap(true);
    bodyLbl->setTextFormat(Qt::RichText);
    bodyLbl->setOpenExternalLinks(true);
    bodyLbl->setTextInteractionFlags(Qt::TextBrowserInteraction);
    bodyLbl->setStyleSheet("font-size:10pt;");
    root->addWidget(bodyLbl);

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

    if (blocking) {
        m_exitBtn = new QPushButton("Exit", this);
        connect(m_exitBtn, &QPushButton::clicked, this, [this]() {
            qDebug() << "[gate] blocking exit";
            qApp->quit();
        });
        btnRow->addWidget(m_exitBtn);
    } else {
        auto* closeBtn = new QPushButton("Close", this);
        connect(closeBtn, &QPushButton::clicked, this, [this]() {
            accept();
        });
        btnRow->addWidget(closeBtn);
    }

    root->addLayout(btnRow);

    qDebug() << "[gate] dialog created blocking=" << (blocking ? 1 : 0);
}

void GateDialog::closeEvent(QCloseEvent* e) {
    if (m_blocking) {
        qDebug() << "[gate] closeEvent ignored (blocking)";
        e->ignore();
    } else {
        e->accept();
    }
}

void GateDialog::reject() {
    if (m_blocking) {
        qDebug() << "[gate] Esc pressed — ignored (blocking)";
    } else {
        QDialog::reject();
    }
}
