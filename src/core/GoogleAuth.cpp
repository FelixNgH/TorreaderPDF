#include "GoogleAuth.h"
#include <QSettings>
#include <QDialog>
#include <QVBoxLayout>
#include <QLabel>
#include <QDialogButtonBox>
#include <QPushButton>

GoogleAuth::GoogleAuth(QObject* parent) : QObject(parent) {}

bool GoogleAuth::readConsent() const {
    QSettings s;
    return s.value("translation/consented", false).toBool();
}

void GoogleAuth::writeConsent(bool value) {
    QSettings s;
    s.setValue("translation/consented", value);
}

bool GoogleAuth::isConsented() const {
    return readConsent();
}

void GoogleAuth::requestConsent(QWidget* parent) {
    QDialog dlg(parent);
    dlg.setWindowTitle("Enable Translation — TorReader PDF");
    dlg.setFixedWidth(440);
    dlg.setWindowModality(Qt::ApplicationModal);

    auto* layout = new QVBoxLayout(&dlg);
    layout->setSpacing(12);
    layout->setContentsMargins(20, 20, 20, 20);

    auto* label = new QLabel;
    label->setText(
        "<b>TorReader PDF uses Google Translate</b> to translate selected text.<br><br>"
        "By clicking <b>Enable</b>, you confirm:<br>"
        "\xE2\x80\xA2 You agree to "
        "<a href='https://policies.google.com/terms'>Google Terms of Service</a><br>"
        "\xE2\x80\xA2 Translation requests are sent from <b>your device</b> to Google<br>"
        "\xE2\x80\xA2 <b>B&amp;B Creations is not responsible</b> for translation quality<br><br>"
        "Translation is <b>FREE</b> via Google Translate.<br>"
        "No Google account login required.");
    label->setWordWrap(true);
    label->setOpenExternalLinks(true);
    layout->addWidget(label);

    auto* btns = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    btns->button(QDialogButtonBox::Ok)->setText("\xE2\x9C\x94 Enable Translation");
    btns->button(QDialogButtonBox::Cancel)->setText("Cancel");
    layout->addWidget(btns);

    QObject::connect(btns, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(btns, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    dlg.raise();
    dlg.activateWindow();
    if (dlg.exec() == QDialog::Accepted)
        writeConsent(true);
}

bool GoogleAuth::checkAndRequest(QWidget* parent) {
    auto* auth = new GoogleAuth(nullptr);
    bool consented = auth->isConsented();
    if (!consented) {
        auth->requestConsent(parent);
        consented = auth->isConsented();
    }
    delete auth;
    return consented;
}

bool GoogleAuth::resetConsent() {
    QSettings s;
    s.setValue("translation/consented", false);
    s.sync();
    return true;
}
