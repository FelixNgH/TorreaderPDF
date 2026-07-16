#pragma once
#include <QWidget>
#include <QString>

class QLabel;
class QPushButton;
class QTimer;

class TranslationPopup : public QWidget {
    Q_OBJECT
public:
    explicit TranslationPopup(QWidget* parent = nullptr);
    void showTranslation(const QString& original, const QString& translation,
                         const QPoint& globalPos);

protected:
    void paintEvent(QPaintEvent* event) override;

private slots:
    void onCopy();

private:
    QLabel*       m_lblOriginal;
    QLabel*       m_lblTranslated;
    QLabel*       m_lblCopied;
    QPushButton*  m_btnCopy;
    QPushButton*  m_btnClose;
    QTimer*       m_autoClose;
    QString       m_translation;
};
