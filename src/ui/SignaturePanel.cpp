#include "SignaturePanel.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QListWidgetItem>

SignaturePanel::SignaturePanel(QWidget* parent)
    : QWidget(parent)
{
    auto* vbox = new QVBoxLayout(this);
    setLayout(vbox);

    // Tiêu đề
    m_title = new QLabel("<b>Chu ky so</b>", this);
    vbox->addWidget(m_title);

    // Danh sách các chữ ký
    m_list = new QListWidget(this);
    vbox->addWidget(m_list);

    // Nút xác minh tất cả
    m_verifyBtn = new QPushButton("Xac minh tat ca", this);
    vbox->addWidget(m_verifyBtn);

    // Khu vực chi tiết (read-only)
    m_detail = new QTextEdit(this);
    m_detail->setReadOnly(true);
    vbox->addWidget(m_detail);

    auto showRow = [this](int row) {
        if (row >= 0 && row < m_sigs.size()) {
            const auto& s = m_sigs[row];
            QString detail = QString("Ten: %1\nThoi gian: %2\nLy do: %3\nCryptographic: %4\n%5")
                .arg(s.signerName)
                .arg(s.signingTime)
                .arg(s.reason)
                .arg(s.cryptoValid ? "HOP LE" : "KHONG HOP LE")
                .arg(s.errorMsg);
            m_detail->setPlainText(detail);
        }
    };
    connect(m_list, &QListWidget::currentRowChanged, this, showRow);
    connect(m_verifyBtn, &QPushButton::clicked, this, [this, showRow]() {
        showRow(m_list->currentRow());
    });
}

void SignaturePanel::showSignatures(const QList<SignatureInfo>& sigs)
{
    // Xóa danh sách cũ và vùng chi tiết
    m_list->clear();
    m_detail->clear();

    // Lưu trữ lại danh sách
    m_sigs = sigs;

    for (int i = 0; i < sigs.size(); ++i) {
        const SignatureInfo& s = sigs[i];

        // Tiền tố Valid/Invalid
        QString prefix = s.cryptoValid ? "Valid:" : "Invalid:";

        // Văn bản hiển thị: signerName (signingTime) prefix
        QString text = QString("%1 (%2) %3")
                .arg(s.signerName)
                .arg(s.signingTime)
                .arg(prefix);

        auto* item = new QListWidgetItem(text, m_list);
        item->setToolTip(s.reason);
        item->setData(Qt::UserRole, i);
        m_list->addItem(item);
    }
}
