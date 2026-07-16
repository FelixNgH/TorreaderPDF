#pragma once
#include <QObject>
#include <QWidget>

class GoogleAuth : public QObject {
    Q_OBJECT
public:
    explicit GoogleAuth(QObject* parent = nullptr);
    bool isConsented() const;
    void requestConsent(QWidget* parent);
    static bool checkAndRequest(QWidget* parent);
    static bool resetConsent();

private:
    bool readConsent() const;
    void writeConsent(bool value);
};
