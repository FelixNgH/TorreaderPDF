#pragma once
#include <QDialog>
#include <QCheckBox>
#include <QPushButton>
#include <QColor>
#include "core/PdfSigner.h"

class QLineEdit;
class QLabel;
class QSpinBox;

class SignDialog : public QDialog {
    Q_OBJECT
public:
    explicit SignDialog(QWidget* parent = nullptr);

    SignParams params() const;
    bool isValid() const { return m_valid; }
    bool visibleSignature() const { return m_visibleChk && m_visibleChk->isChecked(); }

private slots:
    void onBrowsePfx();
    void onValidate();

private:
    QLineEdit*   m_pfxPath     = nullptr;
    QLineEdit*   m_password    = nullptr;
    QLineEdit*   m_reason      = nullptr;
    QLineEdit*   m_location    = nullptr;
    QLabel*      m_certInfo     = nullptr;
    QCheckBox*   m_visibleChk   = nullptr;
    QPushButton* m_textColorBtn = nullptr;
    QSpinBox*    m_fontSpin     = nullptr;
    QCheckBox*   m_fillChk      = nullptr;
    QPushButton* m_fillColorBtn = nullptr;
    QPushButton* m_okBtn        = nullptr;
    bool         m_valid        = false;
    QColor       m_textColor    = Qt::black;
    QColor       m_fillColor    = QColor(255, 245, 200);
};
