#include "NoteInputDialog.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QTextEdit>
#include <QLineEdit>
#include <QDialogButtonBox>

NoteInputDialog::NoteInputDialog(const QString& initialText, QWidget* parent, bool singleLine)
    : QDialog(parent), m_singleLine(singleLine) {
    setWindowTitle("Add Note");
    setMinimumWidth(420);
    setStyleSheet(
        "QDialog { background:#2D2D30; color:#D4D4D4; }"
        "QLabel  { color:#D4D4D4; }"
        "QTextEdit, QLineEdit { background:#3C3F41; color:#D4D4D4; border:1px solid #555;"
        "  border-radius:3px; padding:4px; }"
        "QDialogButtonBox QPushButton { background:#3C3F41; color:#D4D4D4; padding:4px 14px;"
        "  border:1px solid #555; border-radius:3px; }"
        "QDialogButtonBox QPushButton:hover { background:#007ACC; }"
    );

    auto* layout = new QVBoxLayout(this);
    layout->setSpacing(8);
    layout->setContentsMargins(12, 12, 12, 12);

    layout->addWidget(new QLabel("Note:"));
    if (singleLine) {
        m_lineEdit = new QLineEdit;
        m_lineEdit->setText(initialText);
        m_lineEdit->setMinimumWidth(280);
        layout->addWidget(m_lineEdit);
        m_lineEdit->setFocus();
    } else {
        m_textEdit = new QTextEdit;
        m_textEdit->setPlainText(initialText);
        m_textEdit->setMinimumHeight(100);
        layout->addWidget(m_textEdit);
        m_textEdit->setFocus();
    }

    layout->addWidget(new QLabel("Author (optional):"));
    m_author = new QLineEdit;
    layout->addWidget(m_author);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

QString NoteInputDialog::text()   const { return m_singleLine ? m_lineEdit->text() : m_textEdit->toPlainText(); }
QString NoteInputDialog::author() const { return m_author->text(); }
