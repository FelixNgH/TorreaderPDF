#pragma once
#include <QDialog>
#include "core/PdfSigner.h"

class QLineEdit;
class QLabel;
class QPushButton;

class SignDialog : public QDialog {
    Q_OBJECT
public:
    explicit SignDialog(QWidget* parent = nullptr);

    SignParams params() const;
    bool isValid() const { return m_valid; }

private slots:
    void onBrowsePfx();
    void onValidate();

private:
    QLineEdit*   m_pfxPath     = nullptr;
    QLineEdit*   m_password    = nullptr;
    QLineEdit*   m_reason      = nullptr;
    QLineEdit*   m_location    = nullptr;
    QLabel*      m_certInfo    = nullptr;
    QPushButton* m_okBtn       = nullptr;
    bool         m_valid       = false;
};
