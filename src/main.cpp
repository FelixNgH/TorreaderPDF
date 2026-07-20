#include <QApplication>
#include <QStyleFactory>
#include <QThreadPool>
#include <QThread>
#include <QFontDatabase>
#include <QFile>
#include <QTextStream>
#include <QDir>
#include <QImage>
#include "ui/MainWindow.h"
#include "core/PdfEditor.h"
#include "core/PdfDocument.h"
#include "annotations/AnnotationManager.h"
#include "annotations/AnnotationLayer.h"
#include "annotations/AnnotationTypes.h"
#include <fpdfview.h>
#include <fpdf_edit.h>
#include <fpdf_annot.h>

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);

    // Hidden headless CLI mode for crash reproduction
    if (argc >= 4 && QString::fromLocal8Bit(argv[1]) == "--merge") {
        QStringList inputs;
        for (int i = 3; i < argc; ++i)
            inputs << QString::fromLocal8Bit(argv[i]);
        PdfEditor editor;
        bool ok = editor.merge(inputs, QString::fromLocal8Bit(argv[2]));
        QFile res(QDir::tempPath() + "/merge_test_result.txt");
        if (res.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
            QTextStream ts(&res);
            ts << (ok ? "OK" : ("FAIL: " + editor.lastError())) << "\n";
        }
        return ok ? 0 : 2;
    }

    // Use all available cores for PDF rendering
    QThreadPool::globalInstance()->setMaxThreadCount(
        qMax(4, QThread::idealThreadCount()));

    // Use Fusion style for consistent cross-platform look
    app.setStyle(QStyleFactory::create("Fusion"));

    // Prefer open-source fonts; fall back gracefully to system fonts.
    {
        const QStringList candidates = {"Noto Sans", "Segoe UI", "Helvetica Neue",
                                        "Helvetica", "Arial"};
        QFont appFont;
        const auto families = QFontDatabase::families();
        for (const QString& f : candidates) {
            if (families.contains(f)) { appFont = QFont(f, 9); break; }
        }
        if (!appFont.family().isEmpty()) app.setFont(appFont);
    }

    // usage: --annot-selftest [out_dir]
    // Headless verify of the annotation pipeline: create a page, add one of each
    // shape at known coords, save, reopen, enumerate, render page 0 -> PNG.
    if (argc >= 2 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--annot-selftest")) {
        QTextStream out(stdout);
        QString dir = (argc >= 3) ? QString::fromLocal8Bit(argv[2]) : QDir::tempPath();
        QString pdfPath = dir + "/annot_selftest.pdf";
        QString pngPath = dir + "/annot_selftest.png";

        PdfDocument::libAddRef();

        FPDF_DOCUMENT doc = FPDF_CreateNewDocument();
        FPDF_PAGE np = FPDFPage_New(doc, 0, 612, 792);
        FPDFPage_GenerateContent(np);
        FPDF_ClosePage(np);

        AnnotationManager mgr;
        mgr.setDocument(doc, pdfPath);
        AnnotationLayer layer;
        layer.setDocument(doc);
        layer.setAnnotationManager(&mgr);

        AnnotStyle style;
        struct S { const char* name; AnnotTool tool; QPointF a; QPointF b; };
        const S shapes[] = {
            {"Line",      AnnotTool::Line,      QPointF(50, 60),  QPointF(250, 60)},
            {"Arrow",     AnnotTool::Arrow,     QPointF(50, 130), QPointF(250, 190)},
            {"Rectangle", AnnotTool::Rectangle, QPointF(300, 60), QPointF(500, 160)},
            {"Ellipse",   AnnotTool::Ellipse,   QPointF(300, 220),QPointF(500, 320)},
            {"Cloud",     AnnotTool::Cloud,     QPointF(50, 260), QPointF(250, 380)},
        };
        for (const S& s : shapes) {
            layer.commitAnnotation(0, s.tool, style, s.a, s.b, {});
            out << "added " << s.name << "\n";
        }
        mgr.createPopupNote(0, QPointF(400, 450), "hello note", "tester");
        mgr.createInlineNote(0, QRectF(360, 520, 200, 30), "floating text", "tester", false, QColor(0, 0, 200));
        mgr.saveDocument();
        out << "saved: " << pdfPath << "\n";

        FPDF_DOCUMENT doc2 = FPDF_LoadDocument(pdfPath.toUtf8().constData(), nullptr);
        bool okLoad = (doc2 != nullptr);
        int found = 0;
        if (doc2) {
            AnnotationManager mgr2;
            mgr2.setDocument(doc2, pdfPath);
            QList<AnnotInfo> all = mgr2.loadAll(FPDF_GetPageCount(doc2));
            found = all.size();
            out << "reopened, annotations found = " << found << "\n";
            for (const AnnotInfo& a : all)
                out << "  p." << (a.pageIndex + 1) << "  " << a.type
                    << "  rect=(" << a.rect.x() << "," << a.rect.y()
                    << " " << a.rect.width() << "x" << a.rect.height() << ")"
                    << "  text=" << a.text << "\n";

            FPDF_PAGE p = FPDF_LoadPage(doc2, 0);
            const int w = 612 * 2, h = 792 * 2;
            QImage image(w, h, QImage::Format_ARGB32);
            image.fill(Qt::white);
            FPDF_BITMAP bmp = FPDFBitmap_CreateEx(w, h, FPDFBitmap_BGRA,
                                                  image.bits(), image.bytesPerLine());
            FPDF_RenderPageBitmap(bmp, p, 0, 0, w, h, 0, FPDF_ANNOT);
            FPDFBitmap_Destroy(bmp);
            FPDF_ClosePage(p);
            bool pngOk = image.save(pngPath);
            out << "rendered PNG: " << pngPath << " ok=" << pngOk << "\n";
            FPDF_CloseDocument(doc2);
        }

        FPDF_CloseDocument(doc);
        PdfDocument::libRelease();

        out << "SELFTEST " << ((okLoad && found >= 6) ? "PASS" : "FAIL")
            << " (expected>=6, got " << found << ")\n";
        out.flush();
        return (okLoad && found >= 6) ? 0 : 1;
    }

    MainWindow window;
    window.setWindowTitle("TorReader");
    window.resize(1280, 800);
    window.show();

    // Open file passed via command line (e.g. drag-to-exe)
    if (argc > 1)
        window.openFile(QString::fromLocal8Bit(argv[1]));

    return app.exec();
}
