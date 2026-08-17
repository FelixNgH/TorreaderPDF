#pragma once
#include <QFrame>
#include <QString>
class QLabel;

class UpdateBanner : public QFrame {
    Q_OBJECT
public:
    explicit UpdateBanner(QWidget* parent = nullptr);
    void showUpdate(const QString& version, const QString& title,
                    const QString& body, const QString& downloadUrl);
    void applyTheme(bool dark);

private:
    QLabel*  m_label = nullptr;
    QLabel*  m_bodyLabel = nullptr;
    QString  m_downloadUrl;
};
