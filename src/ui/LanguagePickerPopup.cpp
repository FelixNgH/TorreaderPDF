#include "LanguagePickerPopup.h"
#include "../core/Translator.h"

#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <QToolButton>
#include <QCheckBox>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QPainter>
#include <QSettings>
#include <QScreen>
#include <QGuiApplication>
#include <QFontMetrics>

static constexpr const char* kKeySrc      = "translate/srcLang";
static constexpr const char* kKeyDst      = "translate/dstLang";
static constexpr const char* kKeyRemember = "translate/rememberLangs";

QString LanguagePickerPopup::savedSource() {
    QSettings s;
    return s.value(QLatin1String(kKeySrc), QStringLiteral("en")).toString();
}

QString LanguagePickerPopup::savedTarget() {
    QSettings s;
    return s.value(QLatin1String(kKeyDst), QStringLiteral("vi")).toString();
}

bool LanguagePickerPopup::rememberChoice() {
    QSettings s;
    return s.value(QLatin1String(kKeyRemember), false).toBool();
}

LanguagePickerPopup::LanguagePickerPopup(QWidget* parent)
    : QDialog(parent, Qt::Tool | Qt::FramelessWindowHint)
{
    setAttribute(Qt::WA_TranslucentBackground);
    setWindowTitle(tr("Translate"));
    setFixedWidth(360);

    auto* title = new QLabel(tr("Translate selection"), this);
    title->setStyleSheet("font-weight:bold; font-size:11pt; color:#1a1a1a;");

    m_lblPreview = new QLabel(this);
    m_lblPreview->setWordWrap(true);
    m_lblPreview->setStyleSheet("color:#666; font-size:9pt; font-style:italic;");

    m_cbFrom = new QComboBox(this);
    m_cbTo   = new QComboBox(this);
    for (const auto& lang : Translator::languages()) {
        m_cbFrom->addItem(lang.second, lang.first);
        if (lang.first != QLatin1String("auto"))   // no "auto" as a target
            m_cbTo->addItem(lang.second, lang.first);
    }
    selectCode(m_cbFrom, savedSource());
    selectCode(m_cbTo,   savedTarget());

    m_btnSwap = new QToolButton(this);
    m_btnSwap->setText(QString::fromUtf8("\xE2\x87\x84"));   // ⇄
    m_btnSwap->setToolTip(tr("Swap languages"));
    m_btnSwap->setCursor(Qt::PointingHandCursor);
    m_btnSwap->setAutoRaise(true);

    auto* lblFrom = new QLabel(tr("From"), this);
    auto* lblTo   = new QLabel(tr("Into"), this);
    lblFrom->setStyleSheet("color:#444; font-size:9pt;");
    lblTo->setStyleSheet("color:#444; font-size:9pt;");

    auto* grid = new QGridLayout;
    grid->setHorizontalSpacing(8);
    grid->setVerticalSpacing(4);
    grid->addWidget(lblFrom,  0, 0);
    grid->addWidget(lblTo,    0, 2);
    grid->addWidget(m_cbFrom, 1, 0);
    grid->addWidget(m_btnSwap,1, 1);
    grid->addWidget(m_cbTo,   1, 2);
    grid->setColumnStretch(0, 1);
    grid->setColumnStretch(2, 1);

    m_chkRemember = new QCheckBox(tr("Always use this pair (don't ask again)"), this);
    m_chkRemember->setStyleSheet("color:#444; font-size:9pt;");
    m_chkRemember->setChecked(rememberChoice());

    m_btnTranslate = new QPushButton(tr("Translate"), this);
    m_btnTranslate->setDefault(true);
    m_btnTranslate->setCursor(Qt::PointingHandCursor);
    m_btnCancel = new QPushButton(tr("Cancel"), this);
    m_btnCancel->setCursor(Qt::PointingHandCursor);

    auto* btns = new QHBoxLayout;
    btns->addStretch();
    btns->addWidget(m_btnCancel);
    btns->addWidget(m_btnTranslate);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(14, 12, 14, 12);
    layout->setSpacing(8);
    layout->addWidget(title);
    layout->addWidget(m_lblPreview);
    layout->addLayout(grid);
    layout->addWidget(m_chkRemember);
    layout->addLayout(btns);

    connect(m_btnSwap,      &QToolButton::clicked, this, &LanguagePickerPopup::onSwap);
    connect(m_btnTranslate, &QPushButton::clicked, this, &LanguagePickerPopup::onTranslate);
    connect(m_btnCancel,    &QPushButton::clicked, this, &QDialog::reject);
}

void LanguagePickerPopup::selectCode(QComboBox* box, const QString& code) {
    int idx = box->findData(code);
    if (idx < 0)
        idx = box->findData(box == m_cbTo ? QStringLiteral("vi") : QStringLiteral("en"));
    if (idx >= 0)
        box->setCurrentIndex(idx);
}

void LanguagePickerPopup::onSwap() {
    const QString from = m_cbFrom->currentData().toString();
    const QString to   = m_cbTo->currentData().toString();
    // "auto" cannot be a target, so swapping out of it falls back to English.
    selectCode(m_cbTo, from == QLatin1String("auto") ? QStringLiteral("en") : from);
    selectCode(m_cbFrom, to);
}

void LanguagePickerPopup::onTranslate() {
    const QString src = m_cbFrom->currentData().toString();
    const QString dst = m_cbTo->currentData().toString();

    QSettings s;
    s.setValue(QLatin1String(kKeySrc), src);
    s.setValue(QLatin1String(kKeyDst), dst);
    s.setValue(QLatin1String(kKeyRemember), m_chkRemember->isChecked());
    s.sync();

    hide();
    emit translateRequested(m_text, src, dst);
}

void LanguagePickerPopup::askFor(const QString& selectedText, const QPoint& globalPos) {
    m_text = selectedText;

    QString preview = selectedText.simplified();
    if (preview.size() > 160)
        preview = preview.left(157) + QStringLiteral("...");
    // Escaped rather than literal so the file stays ASCII, like the rest of the UI code.
    m_lblPreview->setText(QString::fromUtf8("\xE2\x80\x9C%1\xE2\x80\x9D").arg(preview));

    // Re-read persisted defaults in case they changed elsewhere.
    selectCode(m_cbFrom, savedSource());
    selectCode(m_cbTo,   savedTarget());
    m_chkRemember->setChecked(rememberChoice());

    adjustSize();

    // Place near the cursor, clamped to the screen the cursor is actually on.
    QScreen* scr = QGuiApplication::screenAt(globalPos);
    if (!scr) scr = QGuiApplication::primaryScreen();
    const QRect avail = scr ? scr->availableGeometry() : QRect(0, 0, 1920, 1080);

    // qBound asserts when min > max, which happens if the popup is wider than
    // the screen — clamp the upper bound first.
    const int xMax = qMax(avail.left(), avail.right() - width());
    int x = qBound(avail.left(), globalPos.x() - width() / 2, xMax);
    int y = globalPos.y() + 14;
    if (y + height() > avail.bottom())
        y = qMax(avail.top(), globalPos.y() - height() - 14);
    move(x, y);

    show();
    raise();
    activateWindow();
    m_btnTranslate->setFocus();
}

void LanguagePickerPopup::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setBrush(QColor(255, 255, 255, 250));
    p.setPen(QPen(QColor(190, 190, 190), 1));
    p.drawRoundedRect(rect().adjusted(1, 1, -1, -1), 8, 8);
}
