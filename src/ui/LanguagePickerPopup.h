#pragma once
#include <QDialog>
#include <QString>

class QComboBox;
class QLabel;
class QPushButton;
class QToolButton;
class QCheckBox;

// Small floating panel shown right after the user marquee-selects text.
// Lets them confirm "translate FROM <lang> INTO <lang>" before the request
// goes out. Defaults to English -> Vietnamese and remembers the last choice.
class LanguagePickerPopup : public QDialog {
    Q_OBJECT
public:
    explicit LanguagePickerPopup(QWidget* parent = nullptr);

    // Shows the panel near globalPos with a preview of the selected text.
    void askFor(const QString& selectedText, const QPoint& globalPos);

    static QString savedSource();
    static QString savedTarget();
    // True when the user ticked "don't ask again" - callers should then
    // translate straight away using savedSource()/savedTarget().
    static bool rememberChoice();

signals:
    // Emitted when the user confirms. src may be "auto".
    void translateRequested(const QString& text, const QString& srcLang,
                            const QString& dstLang);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    void selectCode(QComboBox* box, const QString& code);
    void onSwap();
    void onTranslate();

    QLabel*      m_lblPreview;
    QComboBox*   m_cbFrom;
    QComboBox*   m_cbTo;
    QToolButton* m_btnSwap;
    QCheckBox*   m_chkRemember;
    QPushButton* m_btnTranslate;
    QPushButton* m_btnCancel;
    QString      m_text;
};
