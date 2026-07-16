#pragma once
#include <QDialog>
#include <QPrinter>
#include <QPageSize>
#include "core/PdfDocument.h"

class QComboBox;
class QCheckBox;
class QLineEdit;

class PrintDialog : public QDialog {
    Q_OBJECT
public:
    explicit PrintDialog(PdfDocument* doc, QWidget* parent = nullptr);

    static void print(PdfDocument* doc, QWidget* parent = nullptr);

private slots:
    void onPrint();

private:
    struct PageRange { int first; int last; };
    QList<PageRange> parseRange() const;

    PdfDocument* m_doc;
    QPrinter     m_printer;
    QComboBox*   m_printerCombo = nullptr;
    QComboBox*   m_paperCombo  = nullptr;
    QComboBox*   m_colorCombo  = nullptr;
    QComboBox*   m_dpiCombo    = nullptr;
    QCheckBox*   m_fitPage     = nullptr;
    QLineEdit*   m_rangeEdit   = nullptr;
};
