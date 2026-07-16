#pragma once
#include <QDialog>
#include <QString>

class QTextEdit;
class QLineEdit;

class NoteInputDialog : public QDialog {
    Q_OBJECT
public:
    explicit NoteInputDialog(const QString& initialText = {}, QWidget* parent = nullptr);

    QString text()   const;
    QString author() const;

private:
    QTextEdit* m_text   = nullptr;
    QLineEdit* m_author = nullptr;
};
