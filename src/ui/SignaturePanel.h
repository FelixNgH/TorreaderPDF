#pragma once

#include <QWidget>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QTextEdit>
#include <QList>

// Bao gồm định nghĩa đầy đủ của SignatureInfo
#include "core/PdfSigner.h"

class SignaturePanel : public QWidget {
    Q_OBJECT
public:
    explicit SignaturePanel(QWidget* parent = nullptr);

public slots:
    // Nhận danh sách các chữ ký để hiển thị
    void showSignatures(const QList<SignatureInfo>& sigs);

private:
    QLabel*      m_title    = nullptr;
    QListWidget* m_list     = nullptr;
    QPushButton* m_verifyBtn = nullptr;
    QTextEdit*   m_detail   = nullptr;

    QList<SignatureInfo> m_sigs;
};
