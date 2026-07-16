#include "PrintDialog.h"
#include "core/PdfPrinter.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QComboBox>
#include <QCheckBox>
#include <QLineEdit>
#include <QPushButton>
#include <QLabel>
#include <QPrintDialog>
#include <QPrinterInfo>
#include <QMessageBox>
#include <QPageSize>

PrintDialog::PrintDialog(PdfDocument* doc, QWidget* parent)
    : QDialog(parent), m_doc(doc) {
    setWindowTitle("Print — TorReader PDF");
    setMinimumWidth(420);

    auto* form = new QFormLayout;

    // Printer selection
    m_printerCombo = new QComboBox;
    {
        QStringList printers = QPrinterInfo::availablePrinterNames();
        if (printers.isEmpty()) {
            m_printerCombo->addItem("(Default Printer)");
        } else {
            for (const QString& name : printers)
                m_printerCombo->addItem(name);
            QString defaultName = QPrinterInfo::defaultPrinter().printerName();
            int idx = printers.indexOf(defaultName);
            if (idx >= 0) m_printerCombo->setCurrentIndex(idx);
        }
    }
    form->addRow("Printer:", m_printerCombo);

    // Paper size
    m_paperCombo = new QComboBox;
    m_paperCombo->addItem("A4",      QVariant::fromValue(QPageSize::A4));
    m_paperCombo->addItem("A3",      QVariant::fromValue(QPageSize::A3));
    m_paperCombo->addItem("A5",      QVariant::fromValue(QPageSize::A5));
    m_paperCombo->addItem("Letter",  QVariant::fromValue(QPageSize::Letter));
    m_paperCombo->addItem("Legal",   QVariant::fromValue(QPageSize::Legal));
    m_paperCombo->addItem("Tabloid", QVariant::fromValue(QPageSize::Tabloid));
    m_paperCombo->setCurrentIndex(0); // A4 default
    form->addRow("Paper size:", m_paperCombo);

    // Color mode
    m_colorCombo = new QComboBox;
    m_colorCombo->addItem("Color",      QVariant::fromValue(int(QPrinter::Color)));
    m_colorCombo->addItem("Grayscale",  QVariant::fromValue(int(QPrinter::GrayScale)));
    m_colorCombo->setCurrentIndex(0);
    form->addRow("Color mode:", m_colorCombo);

    // Print quality (DPI)
    m_dpiCombo = new QComboBox;
    m_dpiCombo->addItems({"150 dpi", "300 dpi", "600 dpi"});
    m_dpiCombo->setCurrentIndex(1);
    form->addRow("Quality:", m_dpiCombo);

    // Page range
    m_rangeEdit = new QLineEdit;
    m_rangeEdit->setPlaceholderText("All pages  (e.g. 1-3, 5, 7-9)");
    form->addRow("Page range:", m_rangeEdit);

    // Fit to page
    m_fitPage = new QCheckBox("Scale to fit page");
    m_fitPage->setChecked(true);
    form->addRow("", m_fitPage);

    auto* btnRow = new QHBoxLayout;
    auto* printerBtn = new QPushButton("Printer settings…");
    auto* printBtn   = new QPushButton("Print");
    auto* cancelBtn  = new QPushButton("Cancel");
    printBtn->setDefault(true);
    btnRow->addWidget(printerBtn);
    btnRow->addStretch();
    btnRow->addWidget(cancelBtn);
    btnRow->addWidget(printBtn);

    auto* main = new QVBoxLayout(this);
    main->addLayout(form);
    main->addSpacing(8);
    main->addLayout(btnRow);

    connect(printerBtn, &QPushButton::clicked, this, [this] {
        QPrintDialog dlg(&m_printer, this);
        dlg.exec();
    });
    connect(printBtn,  &QPushButton::clicked, this, &PrintDialog::onPrint);
    connect(cancelBtn, &QPushButton::clicked, this, &QDialog::reject);
}

QList<PrintDialog::PageRange> PrintDialog::parseRange() const {
    QList<PageRange> result;
    QString text = m_rangeEdit->text().trimmed();
    int maxPage = m_doc->pageCount() - 1;

    if (text.isEmpty()) {
        result.append({0, maxPage});
        return result;
    }
    for (const QString& part : text.split(',', Qt::SkipEmptyParts)) {
        if (part.contains('-')) {
            QStringList lr = part.split('-');
            if (lr.size() == 2) {
                int f = lr[0].trimmed().toInt() - 1;
                int l = lr[1].trimmed().toInt() - 1;
                result.append({qBound(0,f,maxPage), qBound(0,l,maxPage)});
            }
        } else {
            int p = part.trimmed().toInt() - 1;
            if (p >= 0 && p <= maxPage) result.append({p, p});
        }
    }
    if (result.isEmpty()) result.append({0, maxPage});
    return result;
}

void PrintDialog::onPrint() {
    if (!m_doc || !m_doc->isOpen()) return;

    // Apply printer selection
    {
        QString selectedPrinter = m_printerCombo->currentText();
        if (selectedPrinter != "(Default Printer)")
            m_printer.setPrinterName(selectedPrinter);
    }

    // Apply paper size
    auto sizeId = static_cast<QPageSize::PageSizeId>(
        m_paperCombo->currentData().value<QPageSize::PageSizeId>());
    m_printer.setPageSize(QPageSize(sizeId));

    // Apply color mode
    auto colorMode = static_cast<QPrinter::ColorMode>(
        m_colorCombo->currentData().toInt());
    m_printer.setColorMode(colorMode);

    // Apply DPI
    int dpi = m_dpiCombo->currentText().split(' ').first().toInt();
    m_printer.setResolution(dpi);

    PdfPrinter pp;
    connect(&pp, &PdfPrinter::progress, this, [](int, int){});

    auto ranges = parseRange();
    bool first = true;
    for (auto& r : ranges) {
        for (int i = r.first; i <= r.last; ++i) {
            if (!first) m_printer.newPage();
            pp.printPage(m_doc->raw(), &m_printer, i);
            first = false;
        }
    }
    accept();
}

void PrintDialog::print(PdfDocument* doc, QWidget* parent) {
    PrintDialog dlg(doc, parent);
    dlg.exec();
}
