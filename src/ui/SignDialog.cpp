#include "SignDialog.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QLineEdit>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QColorDialog>
#include <QFileDialog>
#include <QFile>
#include <QMessageBox>

#include <openssl/pkcs12.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>

// ── RAII wrappers for OpenSSL types ──────────────────────────────────────────

template<typename T, auto FreeFn>
struct SslObj {
    T* p = nullptr;
    SslObj() = default;
    explicit SslObj(T* ptr) : p(ptr) {}
    ~SslObj() { if (p) FreeFn(p); }
    SslObj(const SslObj&) = delete;
    SslObj& operator=(const SslObj&) = delete;
    T** operator&() { return &p; }
    operator T*() const { return p; }
    T* operator->() const { return p; }
    T* release() { T* r = p; p = nullptr; return r; }
};

using Pkcs12Ptr  = SslObj<PKCS12, PKCS12_free>;
using EvpPkeyPtr = SslObj<EVP_PKEY, EVP_PKEY_free>;
using X509Ptr    = SslObj<X509, X509_free>;
using BioPtr     = SslObj<BIO, BIO_free>;

struct ChainPtr {
    STACK_OF(X509)* p = nullptr;
    ~ChainPtr() { if (p) sk_X509_pop_free(p, X509_free); }
    ChainPtr() = default;
    explicit ChainPtr(STACK_OF(X509)* ptr) : p(ptr) {}
    ChainPtr(const ChainPtr&) = delete;
    ChainPtr& operator=(const ChainPtr&) = delete;
    STACK_OF(X509)** operator&() { return &p; }
    operator STACK_OF(X509)*() const { return p; }
    STACK_OF(X509)* release() { STACK_OF(X509)* r = p; p = nullptr; return r; }
};

SignDialog::SignDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle("Sign PDF — TorReader");
    setMinimumWidth(460);

    auto* form = new QFormLayout;

    m_pfxPath = new QLineEdit(this);
    m_pfxPath->setPlaceholderText("Select a .pfx / .p12 file…");
    auto* browseBtn = new QPushButton("Browse…", this);
    auto* pfxRow = new QHBoxLayout;
    pfxRow->addWidget(m_pfxPath, 1);
    pfxRow->addWidget(browseBtn);
    form->addRow("Certificate:", pfxRow);

    m_password = new QLineEdit(this);
    m_password->setEchoMode(QLineEdit::Password);
    m_password->setPlaceholderText("PFX file password");
    form->addRow("Password:", m_password);

    m_certInfo = new QLabel(this);
    m_certInfo->setWordWrap(true);
    m_certInfo->setStyleSheet("color: #888; font-size: 11px; padding: 4px 0;");
    form->addRow("", m_certInfo);

    m_reason = new QLineEdit(this);
    m_reason->setPlaceholderText("e.g. Reviewed drawings (optional)");
    form->addRow("Reason:", m_reason);

    m_location = new QLineEdit(this);
    m_location->setPlaceholderText("e.g. Hanoi (optional)");
    form->addRow("Location:", m_location);

    m_visibleChk = new QCheckBox("Show a visible signature stamp on the page (you will draw a box after clicking Sign)", this);
    form->addRow("", m_visibleChk);

    m_textColorBtn = new QPushButton(this);
    m_textColorBtn->setFixedWidth(60);
    m_textColorBtn->setStyleSheet("background: " + m_textColor.name() + "; border: 1px solid #999;");
    connect(m_textColorBtn, &QPushButton::clicked, this, [this]() {
        QColor c = QColorDialog::getColor(m_textColor, this, "Text Color");
        if (c.isValid()) { m_textColor = c; m_textColorBtn->setStyleSheet("background: " + m_textColor.name() + "; border: 1px solid #999;"); }
    });
    form->addRow("Text color:", m_textColorBtn);

    m_fontSpin = new QSpinBox(this);
    m_fontSpin->setRange(0, 48);
    m_fontSpin->setValue(0);
    m_fontSpin->setSuffix(" pt");
    m_fontSpin->setSpecialValueText("Auto");
    form->addRow("Font size:", m_fontSpin);

    m_fillChk = new QCheckBox("Fill background", this);
    form->addRow("", m_fillChk);

    m_fillColorBtn = new QPushButton(this);
    m_fillColorBtn->setFixedWidth(60);
    m_fillColorBtn->setStyleSheet("background: " + m_fillColor.name() + "; border: 1px solid #999;");
    connect(m_fillColorBtn, &QPushButton::clicked, this, [this]() {
        QColor c = QColorDialog::getColor(m_fillColor, this, "Fill Color");
        if (c.isValid()) { m_fillColor = c; m_fillColorBtn->setStyleSheet("background: " + m_fillColor.name() + "; border: 1px solid #999;"); }
    });
    form->addRow("Fill color:", m_fillColorBtn);

    auto* btnLayout = new QHBoxLayout;
    m_okBtn = new QPushButton("Sign", this);
    m_okBtn->setDefault(true);
    m_okBtn->setEnabled(false);
    auto* cancelBtn = new QPushButton("Cancel", this);
    btnLayout->addStretch();
    btnLayout->addWidget(m_okBtn);
    btnLayout->addWidget(cancelBtn);

    auto* main = new QVBoxLayout(this);
    main->addLayout(form);
    main->addLayout(btnLayout);

    connect(browseBtn, &QPushButton::clicked, this, &SignDialog::onBrowsePfx);
    connect(m_password, &QLineEdit::textChanged, this, &SignDialog::onValidate);
    connect(m_pfxPath, &QLineEdit::textChanged, this, &SignDialog::onValidate);
    connect(m_okBtn, &QPushButton::clicked, this, &QDialog::accept);
    connect(cancelBtn, &QPushButton::clicked, this, &QDialog::reject);
}

SignParams SignDialog::params() const
{
    SignParams p;
    p.pfxPath  = m_pfxPath->text().trimmed();
    p.password = m_password->text();
    p.reason   = m_reason->text().trimmed();
    p.location = m_location->text().trimmed();
    p.textR = m_textColor.red();  p.textG = m_textColor.green();  p.textB = m_textColor.blue();
    p.stampFont = m_fontSpin->value();
    p.fillBg = m_fillChk->isChecked();
    p.fillR = m_fillColor.red();  p.fillG = m_fillColor.green();  p.fillB = m_fillColor.blue();
    return p;
}

void SignDialog::onBrowsePfx()
{
    QString path = QFileDialog::getOpenFileName(
        this, "Select Certificate File", {},
        "PKCS#12 (*.pfx *.p12);;All Files (*)");
    if (!path.isEmpty()) {
        m_pfxPath->setText(path);
        onValidate();
    }
}

void SignDialog::onValidate()
{
    QString path = m_pfxPath->text().trimmed();
    QString pwd  = m_password->text();
    m_valid = false;
    m_okBtn->setEnabled(false);
    m_certInfo->clear();

    if (path.isEmpty() || pwd.isEmpty()) return;

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        m_certInfo->setText("<span style='color:red;'>Cannot open file</span>");
        return;
    }
    QByteArray data = f.readAll();
    f.close();

    BioPtr bio(BIO_new_mem_buf(data.constData(), static_cast<int>(data.size())));
    if (!bio) return;

    Pkcs12Ptr p12(d2i_PKCS12_bio(bio, nullptr));
    if (!p12) return;

    EvpPkeyPtr pkey;
    X509Ptr cert;
    ChainPtr chain;
    QByteArray pwdUtf8 = pwd.toUtf8();
    int rc = PKCS12_parse(p12, pwdUtf8.constData(), &pkey, &cert, &chain);

    QString cn;
    if (rc == 1 && cert) {
        X509_NAME* subj = X509_get_subject_name(cert);
        if (subj) {
            char buf[256] = {};
            if (X509_NAME_get_text_by_NID(subj, NID_commonName, buf, sizeof(buf)) > 0)
                cn = QString::fromUtf8(buf);
        }
        const ASN1_TIME* notAfter = X509_get0_notAfter(cert);
        if (notAfter) {
            m_certInfo->setText(QString("<span style='color:green;'>✓ %1 — certificate valid</span>")
                .arg(cn.isEmpty() ? "Loaded" : cn));
        }
        m_valid = true;
        m_okBtn->setEnabled(true);
    } else {
        unsigned long err = ERR_get_error();
        char ebuf[256] = {};
        ERR_error_string_n(err, ebuf, sizeof(ebuf));
        m_certInfo->setText(QString("<span style='color:red;'>%1</span>")
            .arg(QString::fromUtf8(ebuf)));
        m_valid = false;
    }
}
