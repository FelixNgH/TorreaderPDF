#pragma once
#include <QDialog>
#include <QString>

class QLineEdit;
class QTextEdit;
class QPlainTextEdit;
class QShortcut;
struct ThemeTokens;

class NoteInputDialog : public QDialog {
    Q_OBJECT
public:
    // darkTheme: false = SÁNG (mặc định app), true = TỐI (Dark Mode).
    // Hộp nhập của MainWindow VÀ popup sidebar đều dựng qua đây ⇒ cùng
    // một bảng màu, KHÔNG hard-code (0927 LƯỢT 6 / VIỆC 2).
    explicit NoteInputDialog(const QString& initialText = {}, QWidget* parent = nullptr,
                             bool singleLine = false, bool darkTheme = false);

    QString text()   const;
    QString author() const;

private:
    QTextEdit*     m_textEdit  = nullptr;   // giu lai: mode 2 dong cu
    QPlainTextEdit* m_plainEdit = nullptr;  // 0927: mode nhieu dong (FreeText)
    QLineEdit*     m_lineEdit  = nullptr;
    QLineEdit*     m_author    = nullptr;
    QShortcut*     m_okShortcut = nullptr;  // 0927: Ctrl+Enter = OK
    bool           m_singleLine = false;
};
