#pragma once
#include <QDialog>
#include <QString>

class QLineEdit;
class QTextEdit;

class NoteInputDialog : public QDialog {
    Q_OBJECT
public:
    explicit NoteInputDialog(const QString& initialText = {}, QWidget* parent = nullptr, bool singleLine = false);

    QString text()   const;
    QString author() const;

private:
    QTextEdit* m_textEdit  = nullptr;
    QLineEdit* m_lineEdit  = nullptr;
    QLineEdit* m_author    = nullptr;
    bool       m_singleLine = false;
};
