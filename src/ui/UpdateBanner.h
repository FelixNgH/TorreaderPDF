#pragma once
#include <QFrame>
#include <QString>
class QLabel;

class UpdateBanner : public QFrame {
    Q_OBJECT
public:
    explicit UpdateBanner(QWidget* parent = nullptr);
    void showUpdate(const QString& version, const QString& downloadUrl);
    void applyTheme(bool dark);

private:
    QLabel*  m_label = nullptr;
    QString  m_downloadUrl;
};
