#pragma once
#include <QDialog>

class AboutDialog : public QDialog {
    Q_OBJECT
public:
    explicit AboutDialog(bool dark, QWidget* parent = nullptr);
    void showShortcutsTab();
};
