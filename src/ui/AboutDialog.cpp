#include "AboutDialog.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QTabWidget>
#include <QLabel>
#include <QTextBrowser>
#include <QPushButton>
#include <QFont>
#include <QPixmap>

static const char* kLicenseText = R"(
<h3>Third-party open-source components</h3>

<b>PDFium</b> — BSD 3-Clause License<br>
Copyright &copy; Google LLC and contributors.<br>
Used for PDF rendering, annotation, and text extraction.<br><br>

<b>QPDF</b> — Apache License 2.0<br>
Copyright &copy; Jay Berkenbilt.<br>
Used for PDF split, merge, reorder, and structural editing.<br><br>

<b>Qt 6</b> — LGPL v3<br>
Copyright &copy; The Qt Company Ltd.<br>
Source code available at <a href='https://code.qt.io'>code.qt.io</a><br><br>

<b>Tesseract OCR</b> — Apache License 2.0<br>
Copyright &copy; Google LLC and contributors.<br>
Used for optical character recognition on scanned pages.<br><br>

<b>Noto Sans</b> — SIL Open Font License 1.1<br>
Copyright &copy; Google LLC.<br>
Default UI font — free for commercial use.<br><br>

<p style='color:#6B7280; font-size:11px;'>
All third-party libraries are used in compliance with their respective licenses.
TorReader PDF does not bundle proprietary fonts or non-open-source code.
</p>
)";

AboutDialog::AboutDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle("About TorReader PDF");
    setFixedSize(500, 400);

    auto* tabs = new QTabWidget(this);

    // ── Tab: About ────────────────────────────────────────────────────────────
    auto* aboutWidget = new QWidget;
    auto* aLayout = new QVBoxLayout(aboutWidget);
    aLayout->setSpacing(10);
    aLayout->setContentsMargins(24, 20, 24, 20);

    // Logo — always shown on white card so edge pixels blend correctly in any theme
    auto* logoCard = new QLabel;
    logoCard->setAlignment(Qt::AlignCenter);
    logoCard->setStyleSheet(
        "background: white; border-radius: 10px; padding: 10px 20px;");
    QPixmap logo(":/icons/Logo_rectangle.png");
    if (!logo.isNull())
        logoCard->setPixmap(logo.scaledToWidth(200, Qt::SmoothTransformation));
    else {
        logoCard->setText("TorReader PDF");
        logoCard->setStyleSheet(logoCard->styleSheet() +
            "font-size: 20px; font-weight: bold; color: #2563EB;");
    }
    aLayout->addWidget(logoCard);

    auto* versionLabel = new QLabel("TorReader PDF — Version 2.0.0");
    versionLabel->setAlignment(Qt::AlignCenter);
    versionLabel->setStyleSheet("color: #6B7280; font-size: 11px;");
    aLayout->addWidget(versionLabel);

    auto* authorLabel = new QLabel("By <b>FelixNgH</b> (Loc Nguyen Huy)");
    authorLabel->setAlignment(Qt::AlignCenter);
    authorLabel->setTextFormat(Qt::RichText);
    authorLabel->setStyleSheet("color: #4B5563; font-size: 11px; margin-top: 2px;");
    aLayout->addWidget(authorLabel);

    auto* descLabel = new QLabel(
        "Fast, portable PDF viewer built for engineers and architects.\n"
        "View, search, merge, split, and reorder large PDF sets\n"
        "with no installation required.");
    descLabel->setAlignment(Qt::AlignCenter);
    descLabel->setWordWrap(true);
    descLabel->setStyleSheet("color: #374151; margin-top: 6px;");
    aLayout->addWidget(descLabel);

    aLayout->addSpacing(8);

    auto* sponsorLabel = new QLabel(
        "<span style='color:#6B7280; font-size:11px;'>Sponsored by</span> "
        "<a href='https://bimserver.cloud'><b>BIMServer.cloud</b></a>");
    sponsorLabel->setAlignment(Qt::AlignCenter);
    sponsorLabel->setOpenExternalLinks(true);
    sponsorLabel->setTextFormat(Qt::RichText);
    sponsorLabel->setStyleSheet("margin-bottom: 2px;");
    aLayout->addWidget(sponsorLabel);

    auto* linksLabel = new QLabel(
        "<a href='https://torreader.cloud'>torreader.cloud</a>");
    linksLabel->setAlignment(Qt::AlignCenter);
    linksLabel->setOpenExternalLinks(true);
    linksLabel->setTextFormat(Qt::RichText);
    aLayout->addWidget(linksLabel);

    auto* socialLabel = new QLabel(
        "<a href='https://twitter.com/FelixNgHuy'>Twitter @FelixNgHuy</a>"
        " &nbsp;·&nbsp; "
        "<a href='https://github.com/FelixNgH'>GitHub @FelixNgH</a>");
    socialLabel->setAlignment(Qt::AlignCenter);
    socialLabel->setOpenExternalLinks(true);
    socialLabel->setTextFormat(Qt::RichText);
    socialLabel->setStyleSheet("font-size: 11px; margin-top: 2px;");
    aLayout->addWidget(socialLabel);

    aLayout->addStretch();
    tabs->addTab(aboutWidget, "About");

    // ── Tab: Licenses ─────────────────────────────────────────────────────────
    auto* licWidget = new QWidget;
    auto* lLayout = new QVBoxLayout(licWidget);
    lLayout->setContentsMargins(8, 8, 8, 8);
    auto* licBrowser = new QTextBrowser;
    licBrowser->setHtml(kLicenseText);
    licBrowser->setOpenExternalLinks(true);
    lLayout->addWidget(licBrowser);
    tabs->addTab(licWidget, "Licenses");

    // ── Main layout ───────────────────────────────────────────────────────────
    auto* main = new QVBoxLayout(this);
    main->setContentsMargins(0, 0, 0, 8);
    main->addWidget(tabs);

    auto* closeBtn = new QPushButton("Close");
    closeBtn->setFixedWidth(80);
    auto* btnRow = new QHBoxLayout;
    btnRow->addStretch();
    btnRow->addWidget(closeBtn);
    btnRow->setContentsMargins(0, 0, 12, 0);
    main->addLayout(btnRow);

    connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);
}
