#pragma once
#include <QObject>
#include <fpdfview.h>

class QPrinter;

class PdfPrinter : public QObject {
    Q_OBJECT
public:
    explicit PdfPrinter(QObject* parent = nullptr);

    void printAll(FPDF_DOCUMENT doc, QPrinter* printer);
    void printRange(FPDF_DOCUMENT doc, QPrinter* printer, int first, int last);
    void printPage(FPDF_DOCUMENT doc, QPrinter* printer, int pageIdx);

signals:
    void progress(int done, int total);
};
