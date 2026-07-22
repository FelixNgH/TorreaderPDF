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

<b>Qt 6</b> — GNU LGPL v3<br>
Copyright &copy; The Qt Company Ltd.<br>
Dynamically linked; source code and relinking information at <a href='https://code.qt.io'>code.qt.io</a>.<br><br>

<b>PDFium</b> — BSD 3-Clause License<br>
Copyright &copy; Google LLC and contributors.<br>
PDF rendering, annotation, and text extraction.<br><br>

<b>QPDF</b> — Apache License 2.0<br>
Copyright &copy; Jay Berkenbilt.<br>
PDF split, merge, reorder, and structural editing.<br><br>

<b>OpenSSL</b> — Apache License 2.0<br>
Copyright &copy; The OpenSSL Project Authors.<br>
Digital signatures (PKCS#7) — Windows builds.<br><br>

<b>zlib</b> — zlib License<br>
Copyright &copy; Jean-loup Gailly and Mark Adler.<br>
Lossless compression used by PDFium and QPDF.<br><br>

<b>libjpeg-turbo</b> — BSD 3-Clause / IJG License<br>
Copyright &copy; The libjpeg-turbo Project and contributors.<br>
JPEG image decoding.<br><br>

<b>Little CMS (lcms2)</b> — MIT License<br>
Copyright &copy; Marti Maria Saguer.<br>
ICC colour management for colour-accurate rendering.<br><br>

<b>Rust preview engine (formibpdf)</b> — MIT / Apache-2.0<br>
Built on the crates rayon, flate2, fontdue, and image, each under the MIT or Apache-2.0 license.<br>
Low-resolution thumbnail / preview rendering.<br><br>

<b>Tesseract OCR</b> — Apache License 2.0 (optional)<br>
Copyright &copy; Google LLC and contributors.<br>
Optical character recognition — OCR-enabled builds only.<br><br>

<b>Noto Sans</b> — SIL Open Font License 1.1<br>
Copyright &copy; Google LLC.<br>
UI font — free for commercial use.<br><br>

<p style='color:#6B7280; font-size:11px;'>
All third-party libraries are used in compliance with their respective licenses.
Qt is used under the LGPL v3 via dynamic linking, so the Qt libraries can be replaced
or relinked. TorReader PDF bundles no proprietary or non-open-source code.
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

    auto* versionLabel = new QLabel("TorReader PDF — Version 2.2");
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
