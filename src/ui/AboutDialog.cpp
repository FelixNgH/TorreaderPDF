#include "AboutDialog.h"
#include "ThemeTokens.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QTabWidget>
#include <QLabel>
#include <QTextBrowser>
#include <QPushButton>
#include <QFont>
#include <QPixmap>
#include <QFile>
#include <QTextStream>
#include <QMessageBox>
#include <QCoreApplication>
#include <QDialog>
#include <QPlainTextEdit>

#ifndef FELIXPDF_VERSION
#define FELIXPDF_VERSION "0.0.0"
#endif

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

<b>Mesa 3D (llvmpipe)</b> — MIT License<br>
Copyright &copy; Mesa 3D contributors.<br>
Software OpenGL renderer (opengl32sw.dll) — fallback GPU.<br><br>



<p style='color:%1; font-size:11px;'>
All third-party libraries are used in compliance with their respective licenses.
Qt is used under the LGPL v3 via dynamic linking, so the Qt libraries can be replaced
or relinked. TorReader PDF bundles no proprietary or non-open-source code.
</p>
)";

static const char* kShortcutText = R"(
<h3 style='margin-top:0'>Keyboard shortcuts</h3>
<table cellspacing='6'>
<tr><td><b>Ctrl+O</b></td><td>Open PDF</td></tr>
<tr><td><b>Ctrl+S</b> / <b>Ctrl+Shift+S</b></td><td>Save / Save As</td></tr>
<tr><td><b>Ctrl+M</b></td><td>Merge PDFs</td></tr>
<tr><td><b>Ctrl+Shift+E</b></td><td>Extract all pages</td></tr>
<tr><td><b>Ctrl+P</b></td><td>Print</td></tr>
<tr><td><b>Ctrl+Shift+C</b></td><td>Toggle Continuous scroll</td></tr>
<tr><td><b>Ctrl+Shift+F</b></td><td>Fit page</td></tr>
<tr><td><b>Ctrl+=</b> / <b>Ctrl+&minus;</b></td><td>Zoom in / out</td></tr>
<tr><td><b>Ctrl+Shift+T</b></td><td>Translate mode</td></tr>
<tr><td><b>F1</b></td><td>Show this help</td></tr>
</table>
<h3>Mouse</h3>
<table cellspacing='6'>
<tr><td><b>Ctrl+Scroll</b></td><td>Zoom in / out</td></tr>
<tr><td><b>Alt+Drag</b></td><td>Select text to translate</td></tr>
<tr><td><b>Scroll</b></td><td>Flip page</td></tr>
<tr><td><b>Right-click thumbnail</b></td><td>Page options: Insert / Delete / Extract</td></tr>
<tr><td><b>Right-click Translate</b></td><td>Reset translation consent</td></tr>
</table>
)";

AboutDialog::AboutDialog(bool dark, QWidget* parent) : QDialog(parent) {
    const ThemeTokens& t = dark ? darkHC() : lightHC();
    setWindowTitle("About TorReader PDF");
    setFixedSize(500, 400);

    auto* tabs = new QTabWidget(this);

    // ── Tab: About ────────────────────────────────────────────────────────────
    auto* aboutWidget = new QWidget;
    auto* aLayout = new QVBoxLayout(aboutWidget);
    aLayout->setSpacing(10);
    aLayout->setContentsMargins(24, 20, 24, 20);

    // Logo card dung mau nen phu cua theme (den o Dark, trang o Light) — khong
    // con mang trang choi giua nen den. Da kiem resources/icons: KHONG co bien
    // the logo toi, nen chi doi nen card, khong tu lat mau anh bang code.
    auto* logoCard = new QLabel;
    logoCard->setAlignment(Qt::AlignCenter);
    logoCard->setStyleSheet(
        QStringLiteral("background: %1; border-radius: 10px; padding: 10px 20px;")
            .arg(t.bgAlt));
    QPixmap logo(":/icons/TorReader.ico");
    if (!logo.isNull())
        logoCard->setPixmap(logo.scaledToHeight(80, Qt::SmoothTransformation));
    else {
        logoCard->setText("TorReader PDF");
        logoCard->setStyleSheet(logoCard->styleSheet() +
            QStringLiteral("font-size: 20px; font-weight: bold; color: %1;")
                .arg(t.accent));
    }
    aLayout->addWidget(logoCard);

    auto* versionLabel = new QLabel(QStringLiteral("TorReader PDF — Version %1").arg(QStringLiteral(FELIXPDF_VERSION)));
    versionLabel->setAlignment(Qt::AlignCenter);
    versionLabel->setStyleSheet(QStringLiteral("color: %1; font-size: 11px;").arg(t.fgDim));
    aLayout->addWidget(versionLabel);

    auto* authorLabel = new QLabel("By <b>FelixNgH</b> (Loc Nguyen Huy)");
    authorLabel->setAlignment(Qt::AlignCenter);
    authorLabel->setTextFormat(Qt::RichText);
    authorLabel->setStyleSheet(QStringLiteral("color: %1; font-size: 11px; margin-top: 2px;").arg(t.fgDim));
    aLayout->addWidget(authorLabel);

    auto* descLabel = new QLabel(
        "Fast, portable PDF viewer built for engineers and architects.\n"
        "View, search, merge, split, and reorder large PDF sets\n"
        "with no installation required.");
    descLabel->setAlignment(Qt::AlignCenter);
    descLabel->setWordWrap(true);
    descLabel->setStyleSheet(QStringLiteral("color: %1; margin-top: 6px;").arg(t.fg));
    aLayout->addWidget(descLabel);

    aLayout->addSpacing(8);

    auto* sponsorLabel = new QLabel(
        QStringLiteral("<span style='color:%1; font-size:11px;'>Sponsored by</span> "
                       "<a href='https://bimserver.cloud'><b>BIMServer.cloud</b></a>")
            .arg(t.fgDim));
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

    // ── Tab: Shortcuts ────────────────────────────────────────────────────────
    auto* shortcutWidget = new QWidget;
    auto* sLayout = new QVBoxLayout(shortcutWidget);
    sLayout->setContentsMargins(8, 8, 8, 8);
    auto* shortcutBrowser = new QTextBrowser;
    shortcutBrowser->setHtml(QString::fromUtf8(kShortcutText));
    shortcutBrowser->setOpenExternalLinks(false);
    sLayout->addWidget(shortcutBrowser, 1);
    tabs->addTab(shortcutWidget, "Shortcuts");

    // ── Tab: Licenses ─────────────────────────────────────────────────────────
    auto* licWidget = new QWidget;
    auto* lLayout = new QVBoxLayout(licWidget);
    lLayout->setContentsMargins(8, 8, 8, 8);
    auto* licBrowser = new QTextBrowser;
    licBrowser->setHtml(QString::fromUtf8(kLicenseText).arg(t.fgDim));
    licBrowser->setOpenExternalLinks(true);
    lLayout->addWidget(licBrowser, 1);
    auto* fullBtn = new QPushButton("Open full notices\u2026");
    lLayout->addWidget(fullBtn, 0, Qt::AlignRight);
    connect(fullBtn, &QPushButton::clicked, this, [this]() {
        QString path = QCoreApplication::applicationDirPath() + "/THIRD-PARTY-LICENSES.txt";
        QFile file(path);
        QString content;
        if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QTextStream stream(&file);
            content = stream.readAll();
            file.close();
        } else {
            content = "Full notices file not found next to the executable.";
        }
        auto* dialog = new QDialog(this);
        dialog->setWindowTitle("Third-Party Licenses");
        dialog->resize(800, 600);
        auto* layout = new QVBoxLayout(dialog);
        auto* textEdit = new QPlainTextEdit(content);
        textEdit->setReadOnly(true);
        QFont monoFont("Courier New", 9);
        monoFont.setStyleHint(QFont::Monospace);
        textEdit->setFont(monoFont);
        layout->addWidget(textEdit);
        auto* licClose = new QPushButton("Close");
        licClose->setFixedWidth(80);
        auto* licBtnRow = new QHBoxLayout;
        licBtnRow->addStretch();
        licBtnRow->addWidget(licClose);
        layout->addLayout(licBtnRow);
        connect(licClose, &QPushButton::clicked, dialog, &QDialog::accept);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->show();
    });
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

void AboutDialog::showShortcutsTab() {
    // Tab order: About=0, Shortcuts=1, Licenses=2
    if (auto* tabs = findChild<QTabWidget*>())
        tabs->setCurrentIndex(1);
}
