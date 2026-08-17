#include <QApplication>
#include <QStyleFactory>
#include <QThreadPool>
#include <QThread>
#include <QFontDatabase>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>
#include <QDateTime>
#include <QMutex>
#include <QDebug>
#include <QLibrary>
#include <QVector>
#include <QHash>
#include <QSet>
#include <QCryptographicHash>
#include <QStringList>
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#ifndef TORREADER_NO_PDFIUM
#include "ui/MainWindow.h"
#include "ui/AboutDialog.h"
#include "ui/ThemeTokens.h"
#include "ui/ThumbnailPanel.h"
#include "core/PdfEditor.h"
#include "core/PdfDocument.h"
#include "core/PdfRenderer.h"
#include "core/PdfSigner.h"
#include "core/TextSearch.h"
#include "core/TextSelection.h"
#include "core/OcrEngine.h"
#include "core/OcrTextLayer.h"
#include "core/VectorLayer.h"
#include "core/PdfCoords.h"
#include "core/PdfLinks.h"
#include "annotations/AnnotationManager.h"
#include "annotations/AnnotationLayer.h"
#include "annotations/AnnotationTypes.h"
#include "core/ThumbnailRenderPool.h"
#include "core/TileCacheFile.h"
#include <fpdfview.h>
#include <fpdf_edit.h>
#include <fpdf_save.h>
#include <fpdf_annot.h>
#include <fpdf_progressive.h>
#include <fpdf_text.h>
#include <fpdf_formfill.h>
#include <QPdfWriter>
#include <QPainter>
#include <QPageLayout>
#include <QPageSize>
extern QMutex s_pdfiumMutex;
#endif
#ifdef _WIN32
#include <psapi.h>
#endif
#include <QCoreApplication>
#include <QEventLoop>
#include <QDir>
#include <QImage>
#include <QPixmap>
#include <QMap>
#include <QFileDialog>
#include <QListWidget>
#include <QPushButton>
#include <QLabel>
#include <QTextBrowser>
#include <QAction>
#include <QElapsedTimer>
#include <QRegularExpression>
#include <QMouseEvent>
#include <QComboBox>
#include <QDockWidget>
#include <QLineEdit>
#include <QKeyEvent>
#include <QInputDialog>
#include <QTimer>
#include <QToolBar>
#include <QSettings>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLShaderProgram>
#include <QOpenGLBuffer>
#include <QOpenGLExtraFunctions>

static inline void trHashAdd(QCryptographicHash& h, const char* d, qsizetype n) {
#if QT_VERSION >= QT_VERSION_CHECK(6,3,0)
    h.addData(QByteArrayView(d, n));
#else
    h.addData(d, static_cast<int>(n));
#endif
}

// ── Text log: hung TAT CA qDebug/qInfo/qWarning ra file ─────────────────────
// (SPEC_PROBE_LOG_SNAPSHOT 2026-08-16 muc 1). App Windows la GUI khong co
// console nen moi dong qDebug bi vut di — tu day tat ca chui vao
// %TEMP%\torreader.log. Handler chay tu NHIEU LUONG (OCR o QtConcurrent) nen
// phai khoa bang QMutex tinh.
static QFile   g_logFile;
static QMutex  g_logMutex;

static void logHandler(QtMsgType type, const QMessageLogContext&, const QString& msg) {
    const char level = (type == QtInfoMsg)     ? 'I'
                     : (type == QtWarningMsg)  ? 'W'
                     : (type == QtCriticalMsg) ? 'C'
                     : (type == QtFatalMsg)    ? 'C' : 'D';
    {
        QMutexLocker lk(&g_logMutex);
        if (g_logFile.isOpen()) {
            QByteArray line = QDateTime::currentDateTime().toString("hh:mm:ss.zzz").toUtf8();
            line += " [";
            line += level;
            line += "] ";
            line += msg.toUtf8();
            line += '\n';
            g_logFile.write(line);
            g_logFile.flush();      // KHONG dem: app treo/crash van con log
        }
    }
    // Van goi tiep handler mac dinh: ban Linux chay terminal khong mat log.
    fprintf(stderr, "%s\n", msg.toLocal8Bit().constData());
}

// Mo file log kieu Append + ghi dong phan cach cho lan chay nay. Goi NGAY SAU
// khi tao QApplication (can applicationVersion/applicationFilePath) va TRUOC
// khi tao MainWindow. Qua 10 MB thi doi ten thanh torreader.log.1 (ghi de).
static void installTextLog() {
    QString path = QString::fromLocal8Bit(qgetenv("TORREADER_LOG"));
    if (path.isEmpty()) path = QDir::tempPath() + QLatin1String("/torreader.log");
    const QFileInfo fi(path);
    if (fi.exists() && fi.size() > 10 * 1024 * 1024) {
        const QString rotated = path + QLatin1String(".1");
        QFile::remove(rotated);
        QFile::rename(path, rotated);
    }
    g_logFile.setFileName(path);
    const bool opened = g_logFile.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Append);
    qInstallMessageHandler(logHandler);
    if (opened) {
        const QString sep = QStringLiteral("=== TorReader %1 | %2 | pid=%3 | %4 ===")
                                .arg(QCoreApplication::applicationVersion(),
                                     QDateTime::currentDateTime().toString(Qt::ISODate))
                                .arg(QCoreApplication::applicationPid())
                                .arg(QCoreApplication::applicationFilePath());
        g_logFile.write(sep.toUtf8() + '\n');
        g_logFile.flush();
    }
}

#ifndef TORREADER_NO_PDFIUM
struct TRFileWriter {
    FPDF_FILEWRITE base;
    QFile* file;
    static int WriteBlock(FPDF_FILEWRITE* self, const void* data, unsigned long size) {
        auto* fw = reinterpret_cast<TRFileWriter*>(self);
        return fw->file->write(reinterpret_cast<const char*>(data),
                               static_cast<qint64>(size)) == static_cast<qint64>(size) ? 1 : 0;
    }
};
#endif

int main(int argc, char* argv[]) {
    QApplication::setHighDpiScaleFactorRoundingPolicy(
        Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);

    QApplication app(argc, argv);
    app.setApplicationName("TorReader PDF");
    app.setApplicationVersion(FELIXPDF_VERSION);
    app.setOrganizationName("Loc Nguyen Huy");
    app.setOrganizationDomain("torreader.cloud");

    // Ngay sau QApplication, truoc MainWindow: bat het log ra file.
    installTextLog();
    qDebug() << "[gate] app version =" << FELIXPDF_VERSION;

#ifndef TORREADER_NO_PDFIUM
    // Hidden headless CLI mode for crash reproduction
    if (argc >= 4 && QString::fromLocal8Bit(argv[1]) == "--merge") {
        QStringList inputs;
        for (int i = 3; i < argc; ++i)
            inputs << QString::fromLocal8Bit(argv[i]);
        PdfEditor editor;
        bool ok = editor.merge(inputs, QString::fromLocal8Bit(argv[2]));
        QFile res("C:/temp/merge_test_result.txt");
        if (res.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
            QTextStream ts(&res);
            ts << (ok ? "OK" : ("FAIL: " + editor.lastError())) << "\n";
        }
        return ok ? 0 : 2;
    }

    // usage: --sign-test <input.pdf> <output.pdf> <cert.pfx> <password>
    // Headless self-test signing: sign a PDF with a test certificate, no GUI.
    if (argc >= 6 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--sign-test")) {
        QString inputPath  = QString::fromLocal8Bit(argv[2]);
        QString outputPath = QString::fromLocal8Bit(argv[3]);
        QString certPath   = QString::fromLocal8Bit(argv[4]);
        QString password   = QString::fromLocal8Bit(argv[5]);

        QTextStream out(stdout);
        out << "[signtest] input=" << inputPath << " output=" << outputPath << " cert=" << certPath << "\n";

        PdfDocument::libAddRef();

        SignParams sp;
        sp.pfxPath   = certPath;
        sp.password  = password;
        sp.reason    = QStringLiteral("Signature test");
        sp.location  = QStringLiteral("TorReader headless test");
        sp.pageIndex = 0;
        sp.rectPt    = QRectF(400, 700, 180, 60);
        sp.fillBg    = true;

        out << "[signtest] page=1 rect=400,700,180,60\n";
        out.flush();

        QString errorMsg;
        bool ok = PdfSigner::signDocument(inputPath, outputPath, sp, errorMsg);

        PdfDocument::libRelease();

        if (ok) {
            out << "[signtest] RESULT=OK output=" << outputPath << "\n";
            out.flush();
            return 0;
        } else {
            out << "[signtest] RESULT=FAIL error=" << errorMsg << "\n";
            out.flush();
            return 1;
        }
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

#ifndef TORREADER_NO_PDFIUM
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
            FPDF_RenderPageBitmap(bmp, p, 0, 0, w, h, 0, FPDF_ANNOT | FPDF_RENDER_LIMITEDIMAGECACHE);
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

    // usage: --markup-test <input.pdf>
    // Headless diagnostic: tests whether PDFium draws a newly-created Ink
    // annotation into the rendered bitmap. No markup/render logic changes.
    if (argc >= 3 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--markup-test")) {
        QString inputPath = QString::fromLocal8Bit(argv[2]);
        QTextStream out(stdout);

        PdfDocument::libAddRef();

        PdfDocument doc;
        if (!doc.open(inputPath)) {
            out << "[mtest] FAIL cannot open " << inputPath << "\n";
            out.flush();
            PdfDocument::libRelease();
            return 1;
        }
        int pageCount = doc.pageCount();
        out << "[mtest] pageCount=" << pageCount << "\n";
        out.flush();

        double pageW = 0, pageH = 0;
        int pageRot = 0;
        {
            QMutexLocker lock(&s_pdfiumMutex);
            FPDF_PAGE tmpPage = FPDF_LoadPage(doc.raw(), 0);
            if (!tmpPage) {
                out << "[mtest] FAIL cannot load page 0\n";
                out.flush();
                PdfDocument::libRelease();
                return 1;
            }
            pageW = FPDF_GetPageWidth(tmpPage);
            pageH = FPDF_GetPageHeight(tmpPage);
            pageRot = FPDFPage_GetRotation(tmpPage);
            out << "[mtest] page size=" << pageW << " x " << pageH
                << " rot=" << pageRot << "\n";
            FPDF_ClosePage(tmpPage);
        }
        double scale = 1000.0 / pageW;
        int imgW = qMax(1, static_cast<int>(pageW * scale));
        int imgH = qMax(1, static_cast<int>(pageH * scale));

        auto renderPage0 = [&]() -> QImage {
            QMutexLocker lock(&s_pdfiumMutex);
            FPDF_PAGE p = FPDF_LoadPage(doc.raw(), 0);
            if (!p) return QImage();
            QImage image(imgW, imgH, QImage::Format_ARGB32);
            image.fill(Qt::white);
            FPDF_BITMAP bmp = FPDFBitmap_CreateEx(imgW, imgH, FPDFBitmap_BGRA,
                                                  image.bits(), image.bytesPerLine());
            FPDF_RenderPageBitmap(bmp, p, 0, 0, imgW, imgH, 0,
                                  FPDF_ANNOT | FPDF_RENDER_LIMITEDIMAGECACHE);
            FPDFBitmap_Destroy(bmp);
            FPDF_ClosePage(p);
            return image;
        };

        QImage before = renderPage0();
        if (before.isNull()) {
            out << "[mtest] FAIL render before\n";
            out.flush();
            PdfDocument::libRelease();
            return 1;
        }
        before.save(QStringLiteral("markup_before.png"));
        out << "[mtest] before w=" << before.width() << " h=" << before.height() << "\n";
        out.flush();

        {
            AnnotationManager mgr;
            mgr.setDocument(doc.raw(), inputPath);
            AnnotationLayer layer;
            layer.setDocument(doc.raw());
            layer.setAnnotationManager(&mgr);

            AnnotStyle style;
            style.strokeColor = Qt::red;
            style.strokeWidth = 8.0f;
            style.opacity = 1.0f;

            QPointF start(pageW * 0.15, pageH * 0.15);
            QPointF end(pageW * 0.85, pageH * 0.85);
            layer.commitAnnotation(0, AnnotTool::Line, style, start, end, {});

            // Sticky Note at 25%/25% of displayed page
            mgr.createPopupNote(0, QPointF(pageW * 0.25, pageH * 0.25), "sticky test", "tester");
            // FreeText "HORIZONTAL TEST" at 25%-75% width, 60%-70% height
            mgr.createInlineNote(0, QRectF(pageW * 0.25, pageH * 0.60, pageW * 0.50, pageH * 0.10),
                                  QStringLiteral("HORIZONTAL TEST"), QStringLiteral("tester"),
                                  false, QColor(0, 0, 200));
        }

        {
            QMutexLocker lock(&s_pdfiumMutex);
            FPDF_PAGE p = FPDF_LoadPage(doc.raw(), 0);
            if (p) {
                int rot = FPDFPage_GetRotation(p);
                double pW = FPDF_GetPageWidth(p);
                double pH = FPDF_GetPageHeight(p);
                int annotCount = FPDFPage_GetAnnotCount(p);
                out << "[mtest] committed annotCount=" << annotCount
                    << " rot=" << rot << " disp=" << pW << "x" << pH << "\n";

                if (annotCount > 0) {
                    FPDF_ANNOTATION annot = FPDFPage_GetAnnot(p, 0);
                    if (annot) {
                        int subtype = FPDFAnnot_GetSubtype(annot);
                        out << "[mtest] annot subtype=" << subtype << "\n";

                        FS_RECTF r{};
                        if (FPDFAnnot_GetRect(annot, &r)) {
                            out << "[mtest] annot rect=" << r.left << "," << r.bottom
                                << "," << r.right << "," << r.top << "\n";
                            // Verify: forward transform of start/end should match
                            double sx = pageW * 0.15, sy = pageH * 0.15;
                            double ex = pageW * 0.85, ey = pageH * 0.85;
                            QPointF pa, pb;
                            switch (rot) {
                                case 1: pa = {sy, sx}; pb = {ey, ex}; break;
                                case 2: pa = {pW-sx, sy}; pb = {pW-ex, ey}; break;
                                case 3: pa = {pH-sy, pW-sx}; pb = {pH-ey, pW-ex}; break;
                                default: pa = {sx, pH-sy}; pb = {ex, pH-ey}; break;
                            }
                            out << "[mtest] verify expected rect="
                                << qMin(pa.x(), pb.x()) << ","
                                << qMin(pa.y(), pb.y()) << ","
                                << qMax(pa.x(), pb.x()) << ","
                                << qMax(pa.y(), pb.y()) << "\n";
                        }

                        unsigned long nStrokes = FPDFAnnot_GetInkListCount(annot);
                        out << "[mtest] annot inkStrokes=" << nStrokes << "\n";

                        unsigned long apLen = FPDFAnnot_GetAP(annot, FPDF_ANNOT_APPEARANCEMODE_NORMAL, nullptr, 0);
                        out << "[mtest] annot hasAP=" << apLen << "\n";

                        FPDFPage_CloseAnnot(annot);
                    }
                }
                FPDF_ClosePage(p);
            }
        }
        out.flush();

        QImage after = renderPage0();
        if (after.isNull()) {
            out << "[mtest] FAIL render after\n";
            out.flush();
            PdfDocument::libRelease();
            return 1;
        }
        after.save(QStringLiteral("markup_after.png"));
        out << "[mtest] after w=" << after.width() << " h=" << after.height() << "\n";
        out.flush();

        QImage before32 = before.convertToFormat(QImage::Format_RGB32);
        QImage after32 = after.convertToFormat(QImage::Format_RGB32);
        int changed = 0;
        int compareW = qMin(before32.width(), after32.width());
        int compareH = qMin(before32.height(), after32.height());
        for (int y = 0; y < compareH; ++y) {
            const QRgb* rowB = reinterpret_cast<const QRgb*>(before32.constScanLine(y));
            const QRgb* rowA = reinterpret_cast<const QRgb*>(after32.constScanLine(y));
            for (int x = 0; x < compareW; ++x) {
                if (rowB[x] != rowA[x])
                    ++changed;
            }
        }
        out << "[mtest] pixelsChanged=" << changed << "\n";
        out << "[mtest] RESULT=" << (changed > 0 ? "DRAWN" : "NOTDRAWN") << "\n";
        out.flush();

        auto renderPage0Progressive = [&]() -> QImage {
            QMutexLocker lock(&s_pdfiumMutex);
            FPDF_PAGE p = FPDF_LoadPage(doc.raw(), 0);
            if (!p) return QImage();
            QImage image(imgW, imgH, QImage::Format_ARGB32);
            image.fill(Qt::white);
            FPDF_BITMAP bmp = FPDFBitmap_CreateEx(imgW, imgH, FPDFBitmap_BGRA,
                                                  image.bits(), image.bytesPerLine());
            IFSDK_PAUSE pause;
            pause.version = 1;
            pause.NeedToPauseNow = [](IFSDK_PAUSE*) -> FPDF_BOOL { return false; };
            pause.user = nullptr;
            int status = FPDF_RenderPageBitmap_Start(bmp, p, 0, 0, imgW, imgH, 0,
                                                      FPDF_ANNOT | FPDF_RENDER_LIMITEDIMAGECACHE,
                                                      &pause);
            while (status == FPDF_RENDER_TOBECONTINUED)
                status = FPDF_RenderPage_Continue(p, &pause);
            if (status == FPDF_RENDER_DONE || status == FPDF_RENDER_TOBECONTINUED)
                FPDF_RenderPage_Close(p);
            FPDFBitmap_Destroy(bmp);
            FPDF_ClosePage(p);
            return image;
        };

        QImage progressive = renderPage0Progressive();
        if (progressive.isNull()) {
            out << "[mtest] FAIL render progressive\n";
            out.flush();
            PdfDocument::libRelease();
            return 1;
        }
        progressive.save(QStringLiteral("markup_progressive.png"));
        out << "[mtest] progressive w=" << progressive.width() << " h=" << progressive.height() << "\n";
        out.flush();

        QImage progressive32 = progressive.convertToFormat(QImage::Format_RGB32);
        int progChanged = 0;
        for (int y = 0; y < compareH; ++y) {
            const QRgb* rowB = reinterpret_cast<const QRgb*>(before32.constScanLine(y));
            const QRgb* rowP = reinterpret_cast<const QRgb*>(progressive32.constScanLine(y));
            for (int x = 0; x < compareW; ++x) {
                if (rowB[x] != rowP[x])
                    ++progChanged;
            }
        }
        out << "[mtest] progPixelsChanged=" << progChanged << "\n";
        out << "[mtest] PROGRESSIVE=" << (progChanged > 0 ? "DRAWN" : "NOTDRAWN") << "\n";
        out.flush();

        int oneshotVsProgressive = 0;
        for (int y = 0; y < compareH; ++y) {
            const QRgb* rowA = reinterpret_cast<const QRgb*>(after32.constScanLine(y));
            const QRgb* rowP = reinterpret_cast<const QRgb*>(progressive32.constScanLine(y));
            for (int x = 0; x < compareW; ++x) {
                if (rowA[x] != rowP[x])
                    ++oneshotVsProgressive;
            }
        }
        out << "[mtest] oneshotVsProgressive=" << oneshotVsProgressive << "\n";
        out.flush();

        { // PdfRenderer test — reproduce MainWindow's post-markup pipeline
            auto renderer = std::make_unique<PdfRenderer>();
            renderer->setDocument(&doc);

            QImage rendererImage;
            QEventLoop loop;
            QTimer timer;
            timer.setSingleShot(true);

            QMetaObject::Connection conn = QObject::connect(
                renderer.get(), &PdfRenderer::pageReady,
                [&](int, QImage img) { rendererImage = img; loop.quit(); });

            QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);

            renderer->setTileCache(nullptr);
            renderer->clearCache();
            renderer->requestPage(0, 1.0);

            timer.start(30000);
            loop.exec();
            timer.stop();
            QObject::disconnect(conn);

            if (rendererImage.isNull()) {
                out << "[mtest] rendererImage TIMEOUT\n";
            } else {
                out << "[mtest] rendererImage w=" << rendererImage.width() << " h=" << rendererImage.height() << "\n";
                rendererImage.save(QStringLiteral("markup_renderer.png"));

                QImage scaled = rendererImage.scaled(imgW, imgH, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
                QImage scaled32 = scaled.convertToFormat(QImage::Format_RGB32);
                int changed = 0;
                int cW = qMin(scaled32.width(), before32.width());
                int cH = qMin(scaled32.height(), before32.height());
                int totalPixels = cW * cH;
                for (int y = 0; y < cH; ++y) {
                    const QRgb* rowS = reinterpret_cast<const QRgb*>(scaled32.constScanLine(y));
                    const QRgb* rowB = reinterpret_cast<const QRgb*>(before32.constScanLine(y));
                    for (int x = 0; x < cW; ++x) {
                        if (rowS[x] != rowB[x])
                            ++changed;
                    }
                }
                out << "[mtest] rendererVsBefore=" << changed << "\n";
                out << "[mtest] totalPixels=" << totalPixels << "\n";
                out << "[mtest] RENDERER=" << (changed > totalPixels / 100 ? "DRAWN" : "SUSPECT") << "\n";
            }
            out.flush();
        }

        PdfDocument::libRelease();
        return 0;
    }

    // usage: --markup-test2 <input.pdf>
    // Extended diagnostic: tests whether ThumbnailRenderPool's separate
    // FPDF_LoadDocument handles on the same file interfere with annotation
    // rendering (the suspected root cause of the app vs harness discrepancy).
    if (argc >= 3 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--markup-test2")) {
        QString inputPath = QString::fromLocal8Bit(argv[2]);
        QTextStream out(stdout);
        int failures = 0;
        auto CHECK = [&](const QString& label, bool ok) {
            out << (ok ? "[mt2] PASS" : "[mt2] FAIL") << " " << label << "\n";
            if (!ok) ++failures;
        };

        PdfDocument::libAddRef();

        // Helper: render page 0 to QImage (one-shot, like the app's progressive path)
        auto renderPage0 = [&](PdfDocument& doc, int w, int h) -> QImage {
            QMutexLocker lock(&s_pdfiumMutex);
            FPDF_PAGE p = FPDF_LoadPage(doc.raw(), 0);
            if (!p) return {};
            QImage image(w, h, QImage::Format_ARGB32);
            image.fill(Qt::white);
            FPDF_BITMAP bmp = FPDFBitmap_CreateEx(w, h, FPDFBitmap_BGRA,
                                                  image.bits(), image.bytesPerLine());
            IFSDK_PAUSE pause;
            pause.version = 1;
            pause.NeedToPauseNow = [](IFSDK_PAUSE*) -> FPDF_BOOL { return false; };
            pause.user = nullptr;
            FPDF_RenderPageBitmap_Start(bmp, p, 0, 0, w, h, 0,
                                        FPDF_ANNOT | FPDF_RENDER_LIMITEDIMAGECACHE,
                                        &pause);
            FPDF_RenderPage_Close(p);
            FPDFBitmap_Destroy(bmp);
            FPDF_ClosePage(p);
            return image;
        };

        // Helper: count changed pixels between two images
        auto countChanged = [](const QImage& before, const QImage& after) -> int {
            QImage b32 = before.convertToFormat(QImage::Format_RGB32);
            QImage a32 = after.convertToFormat(QImage::Format_RGB32);
            int cw = qMin(b32.width(), a32.width());
            int ch = qMin(b32.height(), a32.height());
            int changed = 0;
            for (int y = 0; y < ch; ++y) {
                const QRgb* rb = reinterpret_cast<const QRgb*>(b32.constScanLine(y));
                const QRgb* ra = reinterpret_cast<const QRgb*>(a32.constScanLine(y));
                for (int x = 0; x < cw; ++x)
                    if (rb[x] != ra[x]) ++changed;
            }
            return changed;
        };

        // ═════════════════════════════════════════════════════════════════════
        // TEST A: baseline — same as --markup-test (no ThumbnailRenderPool)
        // ═════════════════════════════════════════════════════════════════════
        out << "\n[mt2] === TEST A: baseline (no ThumbnailRenderPool) ===\n";
        {
            PdfDocument docA;
            if (!docA.open(inputPath)) { out << "[mt2] FAIL open\n"; PdfDocument::libRelease(); return 1; }
            double pageW = 0, pageH = 0;
            {
                QMutexLocker lock(&s_pdfiumMutex);
                FPDF_PAGE tp = FPDF_LoadPage(docA.raw(), 0);
                if (tp) { pageW = FPDF_GetPageWidth(tp); pageH = FPDF_GetPageHeight(tp); FPDF_ClosePage(tp); }
            }
            int scale = 1000;
            int w = qMax(1, static_cast<int>(pageW * scale / qMax(pageW, pageH)));
            int h = qMax(1, static_cast<int>(pageH * scale / qMax(pageW, pageH)));

            QImage before = renderPage0(docA, w, h);
            before.save(QStringLiteral("mt2_A_before.png"));

            {
                AnnotationManager mgr;
                mgr.setDocument(docA.raw(), inputPath);
                AnnotationLayer layer;
                layer.setDocument(docA.raw());
                layer.setAnnotationManager(&mgr);
                QPointF start(pageW * 0.1, pageH * 0.1);
                QPointF end(pageW * 0.9, pageH * 0.9);
                AnnotStyle style; style.strokeColor = Qt::red; style.strokeWidth = 8.0f; style.opacity = 1.0f;
                layer.commitAnnotation(0, AnnotTool::Line, style, start, end, {});
            }

            QImage after = renderPage0(docA, w, h);
            after.save(QStringLiteral("mt2_A_after.png"));
            int changed = countChanged(before, after);
            CHECK("A annotation drawn", changed > 0);
            out << "[mt2]   A changed pixels=" << changed << "\n";
        }
        out.flush();

        // ═════════════════════════════════════════════════════════════════════
        // TEST B: open ThumbnailRenderPool on same file BEFORE commit
        // ═════════════════════════════════════════════════════════════════════
        out << "[mt2] === TEST B: with ThumbnailRenderPool ===\n";
        {
            PdfDocument docB;
            if (!docB.open(inputPath)) { out << "[mt2] FAIL open\n"; PdfDocument::libRelease(); return 1; }
            double pageW = 0, pageH = 0;
            {
                QMutexLocker lock(&s_pdfiumMutex);
                FPDF_PAGE tp = FPDF_LoadPage(docB.raw(), 0);
                if (tp) { pageW = FPDF_GetPageWidth(tp); pageH = FPDF_GetPageHeight(tp); FPDF_ClosePage(tp); }
            }
            int scale = 1000;
            int w = qMax(1, static_cast<int>(pageW * scale / qMax(pageW, pageH)));
            int h = qMax(1, static_cast<int>(pageH * scale / qMax(pageW, pageH)));

            // Start ThumbnailRenderPool on same file
            auto thumbPoolB = std::make_unique<ThumbnailRenderPool>();
            bool poolOkB = thumbPoolB->open(inputPath);
            CHECK("B pool open", poolOkB);
            if (poolOkB) {
                thumbPoolB->prefetchRange(0, docB.pageCount() - 1);
                // Let thumb pool render a few pages to populate PDFium caches
                QCoreApplication::processEvents();
                QThread::msleep(500);
                QCoreApplication::processEvents();
            }

            QImage before = renderPage0(docB, w, h);
            before.save(QStringLiteral("mt2_B_before.png"));

            {
                AnnotationManager mgr;
                mgr.setDocument(docB.raw(), inputPath);
                AnnotationLayer layer;
                layer.setDocument(docB.raw());
                layer.setAnnotationManager(&mgr);
                QPointF start(pageW * 0.1, pageH * 0.1);
                QPointF end(pageW * 0.9, pageH * 0.9);
                AnnotStyle style; style.strokeColor = Qt::red; style.strokeWidth = 8.0f; style.opacity = 1.0f;
                layer.commitAnnotation(0, AnnotTool::Line, style, start, end, {});
            }

            // Extra loadPage (mimics app's annotMgr->loadPage after commit)
            {
                QMutexLocker lock(&s_pdfiumMutex);
                FPDF_PAGE lp = FPDF_LoadPage(docB.raw(), 0);
                if (lp) {
                    int cnt = FPDFPage_GetAnnotCount(lp);
                    out << "[mt2]   B annotCount after commit=" << cnt << "\n";
                    CHECK("B annot count > 0", cnt > 0);
                    FPDF_ClosePage(lp);
                }
            }

            QImage after = renderPage0(docB, w, h);
            after.save(QStringLiteral("mt2_B_after.png"));
            int changed = countChanged(before, after);
            // If annotation is drawn, changed > 0; if not, changed == 0 (or near 0)
            CHECK("B annotation drawn with ThumbnailRenderPool", changed > 0);
            out << "[mt2]   B changed pixels=" << changed << "\n";

            thumbPoolB->close();
        }
        out.flush();

        // ═════════════════════════════════════════════════════════════════════
        // TEST C: App-accurate — PdfRenderer with tile cache + ThumbnailRenderPool
        // ═════════════════════════════════════════════════════════════════════
        out << "[mt2] === TEST C: PdfRenderer + tileCache + ThumbnailRenderPool ===\n";
        {
            PdfDocument docC;
            if (!docC.open(inputPath)) { out << "[mt2] FAIL open\n"; PdfDocument::libRelease(); return 1; }

            auto rendererC = std::make_unique<PdfRenderer>();
            rendererC->setDocument(&docC);

            // Open persistent tile cache like the app does
            auto tileCacheC = std::make_shared<TileCacheFile>();
            {
                uint64_t hash = TileCacheFile::hashFile(inputPath);
                QFile szFile(inputPath);
                uint64_t sz   = static_cast<uint64_t>(szFile.size());
                if (tileCacheC->open(inputPath, hash, sz, docC.pageCount()))
                    rendererC->setTileCache(tileCacheC);
            }

            // Start ThumbnailRenderPool like the app does
            auto thumbPoolC = std::make_unique<ThumbnailRenderPool>();
            if (thumbPoolC->open(inputPath)) {
                thumbPoolC->prefetchRange(0, docC.pageCount() - 1);
                QCoreApplication::processEvents();
                QThread::msleep(500);
                QCoreApplication::processEvents();
            }

            double pageW = 0, pageH = 0;
            {
                QMutexLocker lock(&s_pdfiumMutex);
                FPDF_PAGE tp = FPDF_LoadPage(docC.raw(), 0);
                if (tp) { pageW = FPDF_GetPageWidth(tp); pageH = FPDF_GetPageHeight(tp); FPDF_ClosePage(tp); }
            }
            int scale = 1000;
            int w = qMax(1, static_cast<int>(pageW * scale / qMax(pageW, pageH)));
            int h = qMax(1, static_cast<int>(pageH * scale / qMax(pageW, pageH)));

            QImage before = renderPage0(docC, w, h);
            before.save(QStringLiteral("mt2_C_before.png"));

            {
                AnnotationManager mgr;
                mgr.setDocument(docC.raw(), inputPath);
                AnnotationLayer layer;
                layer.setDocument(docC.raw());
                layer.setAnnotationManager(&mgr);
                QPointF start(pageW * 0.1, pageH * 0.1);
                QPointF end(pageW * 0.9, pageH * 0.9);
                AnnotStyle style; style.strokeColor = Qt::red; style.strokeWidth = 8.0f; style.opacity = 1.0f;
                layer.commitAnnotation(0, AnnotTool::Line, style, start, end, {});
            }

            // App-accurate: annotMgr->loadPage after commit
            {
                AnnotationManager mgr;
                mgr.setDocument(docC.raw(), inputPath);
                int n = mgr.loadPage(0).size();
                out << "[mt2]   C annotCount after commit=" << n << "\n";
                CHECK("C annot count > 0", n > 0);
            }

            // App-accurate: setTileCache(nullptr) + clearCache() + requestPage
            rendererC->setTileCache(nullptr);
            rendererC->clearCache();

            QImage rendererImg;
            QEventLoop loopC;
            QTimer timerC;
            timerC.setSingleShot(true);
            QMetaObject::Connection connC = QObject::connect(
                rendererC.get(), &PdfRenderer::pageReady,
                [&](int, QImage img) { rendererImg = img; loopC.quit(); });
            QObject::connect(&timerC, &QTimer::timeout, &loopC, &QEventLoop::quit);

            rendererC->requestPage(0, 1.0);
            timerC.start(30000);
            loopC.exec();
            timerC.stop();
            QObject::disconnect(connC);

            if (rendererImg.isNull()) {
                CHECK("C renderer got image", false);
            } else {
                rendererImg.save(QStringLiteral("mt2_C_after.png"));
                int changed = countChanged(before, rendererImg);
                CHECK("C annotation drawn via PdfRenderer", changed > 0);
                out << "[mt2]   C changed pixels=" << changed << "\n";
            }

            if (thumbPoolC) thumbPoolC->close();
        }
        out.flush();

        out << "[mt2] RESULT=" << (failures == 0 ? "ALL_PASS" : QString("FAILURES=%1").arg(failures)) << "\n";
        out.flush();
        PdfDocument::libRelease();
        return failures == 0 ? 0 : 1;
    }

    // usage: --fontsize-test [temp_dir]
    // Headless diagnostic: verifies rebuildTextNote() changes font size
    // and does not leave a duplicate/orphan annotation.
    // Creates a blank page internally so the test is independent of any input file.
    if (argc >= 2 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--fontsize-test")) {
        QTextStream out(stdout);
        QString outDir = (argc >= 3) ? QString::fromLocal8Bit(argv[2]) : QStringLiteral(".");
        PdfDocument::libAddRef();

        constexpr double pageW = 612.0, pageH = 792.0;
        QString tempPdf = outDir + QStringLiteral("/__fs_temp.pdf");

        FPDF_DOCUMENT doc = FPDF_CreateNewDocument();
        if (!doc) { out << "[fs] FAIL create doc\n"; out.flush(); PdfDocument::libRelease(); return 1; }
        {
            QMutexLocker lock(&s_pdfiumMutex);
            FPDF_PAGE np = FPDFPage_New(doc, 0, pageW, pageH);
            if (!np) { out << "[fs] FAIL create page\n"; out.flush(); FPDF_CloseDocument(doc); PdfDocument::libRelease(); return 1; }
            int rot = FPDFPage_GetRotation(np);
            FPDFPage_GenerateContent(np);
            FPDF_ClosePage(np);
            out << "[fs] pageCount=1 rot=" << rot << "\n";
        }
        out.flush();

        double scale = 1000.0 / pageW;
        int imgW = qMax(1, static_cast<int>(pageW * scale));
        int imgH = qMax(1, static_cast<int>(pageH * scale));

        auto renderPage0 = [&]() -> QImage {
            QMutexLocker lock(&s_pdfiumMutex);
            FPDF_PAGE p = FPDF_LoadPage(doc, 0);
            if (!p) return QImage();
            QImage image(imgW, imgH, QImage::Format_ARGB32);
            image.fill(Qt::white);
            FPDF_BITMAP bmp = FPDFBitmap_CreateEx(imgW, imgH, FPDFBitmap_BGRA,
                                                  image.bits(), image.bytesPerLine());
            FPDF_RenderPageBitmap(bmp, p, 0, 0, imgW, imgH, 0,
                                  FPDF_ANNOT | FPDF_RENDER_LIMITEDIMAGECACHE);
            FPDFBitmap_Destroy(bmp);
            FPDF_ClosePage(p);
            return image;
        };

        // Step 2: baseline render
        QImage B0 = renderPage0();
        if (B0.isNull()) {
            out << "[fs] FAIL render baseline\n"; out.flush();
            FPDF_CloseDocument(doc); PdfDocument::libRelease(); return 1;
        }
        B0.save(outDir + QStringLiteral("/fs_before.png"));

        // Step 3: create one inline FreeText note at 8pt
        QRectF noteRect(pageW * 0.30, pageH * 0.45, pageW * 0.40, pageH * 0.10);
        int annotCountAfterCreate = 0;
        {
            AnnotationManager mgr;
            mgr.setDocument(doc, tempPdf);
            AnnotationLayer layer;
            layer.setDocument(doc);
            layer.setAnnotationManager(&mgr);
            mgr.createInlineNote(0, noteRect, QStringLiteral("SIZE"),
                                 QStringLiteral("tester"), false,
                                 QColor(0, 0, 200), 8.0f);
        }
        {
            QMutexLocker lock(&s_pdfiumMutex);
            FPDF_PAGE p = FPDF_LoadPage(doc, 0);
            if (p) {
                annotCountAfterCreate = FPDFPage_GetAnnotCount(p);
                FPDF_ClosePage(p);
            }
        }
        out << "[fs] created annotCount=" << annotCountAfterCreate << "\n";
        out.flush();

        // Step 4: render at 8pt
        int index = annotCountAfterCreate - 1;
        QImage A8 = renderPage0();
        if (A8.isNull()) {
            out << "[fs] FAIL render 8pt\n"; out.flush();
            FPDF_CloseDocument(doc); PdfDocument::libRelease(); return 1;
        }
        A8.save(outDir + QStringLiteral("/fs_8pt.png"));

        // Step 5: rebuildTextNote to 28pt
        bool rebuildOk = false;
        int annotCountAfterRebuild = 0;
        {
            AnnotationManager mgr;
            mgr.setDocument(doc, tempPdf);
            rebuildOk = mgr.rebuildTextNote(0, index, QColor(0, 0, 200), 28.0f);
        }
        {
            QMutexLocker lock(&s_pdfiumMutex);
            FPDF_PAGE p = FPDF_LoadPage(doc, 0);
            if (p) {
                annotCountAfterRebuild = FPDFPage_GetAnnotCount(p);
                FPDF_ClosePage(p);
            }
        }
        out << "[fs] rebuild returned=" << (rebuildOk ? "1" : "0")
            << " annotCountAfter=" << annotCountAfterRebuild << "\n";
        out.flush();

        // Step 6: render at 28pt
        QImage A28 = renderPage0();
        if (A28.isNull()) {
            out << "[fs] FAIL render 28pt\n"; out.flush();
            FPDF_CloseDocument(doc); PdfDocument::libRelease(); return 1;
        }
        A28.save(outDir + QStringLiteral("/fs_28pt.png"));

        // Step 7: blue-pixel measurement in note region.
        // Annotation is created at 30-70% x, 45-55% y (display coords).
        // After dispToPdf (rot=0) the PDF rect is the same but Y is flipped,
        // so in the rendered image the visible text sits at 30-70% x, 45-55% y.
        // We scan a slightly wider band 30-70% x, 40-60% y to capture all ink.
        QImage B0_32 = B0.convertToFormat(QImage::Format_RGB32);
        QImage A8_32 = A8.convertToFormat(QImage::Format_RGB32);
        QImage A28_32 = A28.convertToFormat(QImage::Format_RGB32);
        int x0 = static_cast<int>(imgW * 0.30);
        int x1 = static_cast<int>(imgW * 0.70);
        int y0 = static_cast<int>(imgH * 0.40);
        int y1 = static_cast<int>(imgH * 0.60);

        auto countBlueChanged = [&](const QImage& img) -> int {
            int n = 0;
            int cw = qMin(img.width(), B0_32.width());
            int ch = qMin(img.height(), B0_32.height());
            int clampX1 = (std::min)(x1, cw);
            int clampY1 = (std::min)(y1, ch);
            for (int y = y0; y < clampY1; ++y) {
                const QRgb* rowB = reinterpret_cast<const QRgb*>(B0_32.constScanLine(y));
                const QRgb* rowI = reinterpret_cast<const QRgb*>(img.constScanLine(y));
                for (int x = (std::max)(x0, 0); x < clampX1; ++x) {
                    if (rowB[x] != rowI[x]) {
                        int r = qRed(rowI[x]);
                        int g = qGreen(rowI[x]);
                        int b = qBlue(rowI[x]);
                        if (b > 120 && r < 120 && g < 120)
                            ++n;
                    }
                }
            }
            return n;
        };

        int n8 = countBlueChanged(A8_32);
        int n28 = countBlueChanged(A28_32);
        out << "[fs] bluePixels 8pt=" << n8 << "  28pt=" << n28 << "\n";

        QString result;
        if (n28 > n8 * 1.5)
            result = QStringLiteral("BIGGER");
        else if (n28 < n8)
            result = QStringLiteral("SMALLER");
        else
            result = QStringLiteral("SAME");
        out << "[fs] RESULT=" << result << "\n";
        out.flush();

        // Step 8: orphan/duplicate check — annotCount should be unchanged
        out << "[fs] annotCountDelta=" << (annotCountAfterRebuild - annotCountAfterCreate) << "\n";
        out.flush();

        // Step 9: verify rebuild rect fits text (should be wider for 28pt)
        {
            AnnotationManager mgr;
            mgr.setDocument(doc, tempPdf);
            auto annots = mgr.loadPage(0);
            if (!annots.isEmpty())
                out << "[fs] rectW after rebuild= " << annots.last().rect.width() << "\n";
        }

        // Step 10: moveNote test — shift 100px right, verify rect + ink moved
        double rectLeftBefore = 0, rectLeftAfter = 0;
        int nMove = 0;
        int moveIndex = annotCountAfterRebuild - 1;
        {
            AnnotationManager mgr;
            mgr.setDocument(doc, tempPdf);
            auto annots = mgr.loadPage(0);
            if (moveIndex >= 0 && moveIndex < annots.size())
                rectLeftBefore = annots[moveIndex].rect.left();
            out << "[fs] rectLeft before move= " << rectLeftBefore << "\n";
        }
        {
            AnnotationManager mgr;
            mgr.setDocument(doc, tempPdf);
            mgr.moveAnnot(0, moveIndex, 100.0, 0.0);
        }
        {
            AnnotationManager mgr;
            mgr.setDocument(doc, tempPdf);
            auto annots = mgr.loadPage(0);
            if (!annots.isEmpty())
                rectLeftAfter = annots.last().rect.left();
            out << "[fs] rectLeft after move= " << rectLeftAfter << "\n";

            QImage AMove = renderPage0();
            if (!AMove.isNull()) {
                AMove.save(outDir + QStringLiteral("/fs_move.png"));
                QImage AMove_32 = AMove.convertToFormat(QImage::Format_RGB32);
                nMove = countBlueChanged(AMove_32);
            }
            out << "[fs] bluePixels after move= " << nMove << "\n";
        }
        out.flush();

        // Step 11: getAnnotEditState read-back verification
        {
            AnnotationManager mgr;
            mgr.setDocument(doc, tempPdf);
            QString type;
            QColor col;
            float w, fs;
            bool rok = mgr.getAnnotEditState(0, moveIndex, type, col, w, fs);
            out << "[fs] readback type=" << type
                << " color=" << col.red() << "," << col.green() << "," << col.blue()
                << " fontSize=" << fs << " ok=" << (rok ? "1" : "0") << "\n";
        }
        out.flush();

        FPDF_CloseDocument(doc);
        PdfDocument::libRelease();
        return 0;
    }

    // usage: --vnfont-test [temp_dir]
    // Verifies Vietnamese Unicode text renders correctly through embedded DejaVuSans font.
    // Creates a blank page internally; no input file needed.
    if (argc >= 2 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--vnfont-test")) {
        QTextStream out(stdout);
        QString outDir = (argc >= 3) ? QString::fromLocal8Bit(argv[2]) : QStringLiteral(".");
        PdfDocument::libAddRef();

        constexpr double pageW = 612.0, pageH = 792.0;
        QString testPath = outDir + QStringLiteral("/vnfont_test.pdf");

        FPDF_DOCUMENT doc = FPDF_CreateNewDocument();
        if (!doc) { out << "VNFONT: FAIL create doc\n"; out.flush(); PdfDocument::libRelease(); return 1; }
        {
            QMutexLocker lock(&s_pdfiumMutex);
            FPDF_PAGE np = FPDFPage_New(doc, 0, pageW, pageH);
            if (!np) { out << "VNFONT: FAIL create page\n"; out.flush(); FPDF_CloseDocument(doc); PdfDocument::libRelease(); return 1; }
            FPDFPage_GenerateContent(np);
            FPDF_ClosePage(np);
        }

        AnnotationManager mgr;
        mgr.setDocument(doc, testPath);

        QString original = QStringLiteral("Hatch Gạch ệ ữ ẩ ỡ Đ đ ọ");
        {
            mgr.createInlineNote(0, QRectF(50, 100, 400, 30), original, "tester", false, QColor(255,0,0), 12.0f);
        }

        // Save to PDF
        {
            QFile f(testPath);
            if (!f.open(QIODevice::WriteOnly)) {
                out << "VNFONT: FAIL cannot open output\n"; out.flush();
                FPDF_CloseDocument(doc); PdfDocument::libRelease(); return 1;
            }
            TRFileWriter fw;
            fw.file = &f;
            fw.base.version = 1;
            fw.base.WriteBlock = TRFileWriter::WriteBlock;
            QMutexLocker lock(&s_pdfiumMutex);
            if (!FPDF_SaveAsCopy(doc, &fw.base, 0)) {
                out << "VNFONT: FAIL save\n"; out.flush();
                FPDF_CloseDocument(doc); PdfDocument::libRelease(); return 1;
            }
        }

        // Re-open and extract text via FPDFText
        {
            QMutexLocker lock(&s_pdfiumMutex);
            FPDF_DOCUMENT doc2 = FPDF_LoadDocument(testPath.toUtf8().constData(), nullptr);
            if (!doc2) {
                out << "VNFONT: FAIL reopen doc\n"; out.flush();
                FPDF_CloseDocument(doc); PdfDocument::libRelease(); return 1;
            }
            FPDF_PAGE page2 = FPDF_LoadPage(doc2, 0);
            if (!page2) {
                out << "VNFONT: FAIL reopen page\n"; out.flush();
                FPDF_CloseDocument(doc2);
                FPDF_CloseDocument(doc); PdfDocument::libRelease(); return 1;
            }
            FPDF_TEXTPAGE textPage = FPDFText_LoadPage(page2);
            if (!textPage) {
                out << "VNFONT: FAIL FPDFText_LoadPage\n"; out.flush();
                FPDF_ClosePage(page2); FPDF_CloseDocument(doc2);
                FPDF_CloseDocument(doc); PdfDocument::libRelease(); return 1;
            }
            int len = FPDFText_CountChars(textPage);
            QString extracted;
            for (int i = 0; i < len; ++i) {
                unsigned int cp = FPDFText_GetUnicode(textPage, i);
                extracted += QChar(static_cast<char32_t>(cp));
            }

            if (extracted == original) {
                out << "VNFONT: PASS\n";
            } else {
                out << "VNFONT: FAIL\n";
                out << "  expected: \"" << original << "\"\n";
                out << "  got:      \"" << extracted << "\"\n";
                int minLen = qMin(original.length(), extracted.length());
                for (int i = 0; i < minLen; ++i) {
                    if (original[i] != extracted[i])
                        out << "  diff at pos " << i << ": expected U+" << QString::number(original[i].unicode(), 16)
                            << " got U+" << QString::number(extracted[i].unicode(), 16) << "\n";
                }
                if (original.length() != extracted.length()) {
                    for (int i = minLen; i < original.length(); ++i)
                        out << "  extra expected char at pos " << i << ": U+" << QString::number(original[i].unicode(), 16) << "\n";
                    for (int i = minLen; i < extracted.length(); ++i)
                        out << "  extra got char at pos " << i << ": U+" << QString::number(extracted[i].unicode(), 16) << "\n";
                }
                FPDFText_ClosePage(textPage);
                FPDF_ClosePage(page2); FPDF_CloseDocument(doc2);
                FPDF_CloseDocument(doc); PdfDocument::libRelease();
                return 1;
            }
            FPDFText_ClosePage(textPage);
            FPDF_ClosePage(page2);
            FPDF_CloseDocument(doc2);
        }

        // Assertion 3: NFC normalization — input NFD should produce NFC output
        {
            QString decomposed = original.normalized(QString::NormalizationForm_D);
            if (decomposed == original) {
                out << "VNFONT: FAIL nfd-setup\n";
                out.flush();
                FPDF_CloseDocument(doc);
                PdfDocument::libRelease();
                return 1;
            }

            QString nfcPath = outDir + QStringLiteral("/vnfont_nfc.pdf");
            FPDF_DOCUMENT nfcDoc = FPDF_CreateNewDocument();
            if (!nfcDoc) {
                out << "VNFONT: FAIL nfc create doc\n";
                out.flush();
                FPDF_CloseDocument(doc);
                PdfDocument::libRelease();
                return 1;
            }
            {
                QMutexLocker lock(&s_pdfiumMutex);
                FPDF_PAGE nfcPage = FPDFPage_New(nfcDoc, 0, pageW, pageH);
                if (!nfcPage) {
                    out << "VNFONT: FAIL nfc create page\n";
                    out.flush();
                    FPDF_CloseDocument(nfcDoc);
                    FPDF_CloseDocument(doc);
                    PdfDocument::libRelease();
                    return 1;
                }
                FPDFPage_GenerateContent(nfcPage);
                FPDF_ClosePage(nfcPage);
            }

            {
                AnnotationManager nfcMgr;
                nfcMgr.setDocument(nfcDoc, nfcPath);
                nfcMgr.createInlineNote(0, QRectF(50, 100, 400, 30), decomposed, "tester", false, QColor(255,0,0), 12.0f);
            }

            // Save
            {
                QFile f(nfcPath);
                if (!f.open(QIODevice::WriteOnly)) {
                    out << "VNFONT: FAIL nfc cannot open output\n";
                    out.flush();
                    FPDF_CloseDocument(nfcDoc);
                    FPDF_CloseDocument(doc);
                    PdfDocument::libRelease();
                    return 1;
                }
                TRFileWriter fw;
                fw.file = &f;
                fw.base.version = 1;
                fw.base.WriteBlock = TRFileWriter::WriteBlock;
                QMutexLocker lock(&s_pdfiumMutex);
                if (!FPDF_SaveAsCopy(nfcDoc, &fw.base, 0)) {
                    out << "VNFONT: FAIL nfc save\n";
                    out.flush();
                    FPDF_CloseDocument(nfcDoc);
                    FPDF_CloseDocument(doc);
                    PdfDocument::libRelease();
                    return 1;
                }
            }

            // Reopen and extract text via FPDFText
            {
                QMutexLocker lock(&s_pdfiumMutex);
                FPDF_DOCUMENT nfcDoc2 = FPDF_LoadDocument(nfcPath.toUtf8().constData(), nullptr);
                if (!nfcDoc2) {
                    out << "VNFONT: FAIL nfc reopen doc\n";
                    out.flush();
                    FPDF_CloseDocument(nfcDoc);
                    FPDF_CloseDocument(doc);
                    PdfDocument::libRelease();
                    return 1;
                }
                FPDF_PAGE nfcPage2 = FPDF_LoadPage(nfcDoc2, 0);
                if (!nfcPage2) {
                    out << "VNFONT: FAIL nfc reopen page\n";
                    out.flush();
                    FPDF_CloseDocument(nfcDoc2);
                    FPDF_CloseDocument(nfcDoc);
                    FPDF_CloseDocument(doc);
                    PdfDocument::libRelease();
                    return 1;
                }
                FPDF_TEXTPAGE nfcTextPage = FPDFText_LoadPage(nfcPage2);
                if (!nfcTextPage) {
                    out << "VNFONT: FAIL nfc FPDFText_LoadPage\n";
                    out.flush();
                    FPDF_ClosePage(nfcPage2);
                    FPDF_CloseDocument(nfcDoc2);
                    FPDF_CloseDocument(nfcDoc);
                    FPDF_CloseDocument(doc);
                    PdfDocument::libRelease();
                    return 1;
                }
                int nfcLen = FPDFText_CountChars(nfcTextPage);
                QString nfcExtracted;
                for (int i = 0; i < nfcLen; ++i) {
                    unsigned int cp = FPDFText_GetUnicode(nfcTextPage, i);
                    nfcExtracted += QChar(static_cast<char32_t>(cp));
                }

                if (nfcExtracted == original) {
                    out << "VNFONT: PASS nfc\n";
                } else {
                    out << "VNFONT: FAIL nfc\n";
                    out << "  expected: \"" << original << "\"\n";
                    out << "  got:      \"" << nfcExtracted << "\"\n";
                    int minLen = qMin(original.length(), nfcExtracted.length());
                    for (int i = 0; i < minLen; ++i) {
                        if (original[i] != nfcExtracted[i])
                            out << "  diff at pos " << i << ": expected U+" << QString::number(original[i].unicode(), 16)
                                << " got U+" << QString::number(nfcExtracted[i].unicode(), 16) << "\n";
                    }
                    if (original.length() != nfcExtracted.length()) {
                        for (int i = minLen; i < original.length(); ++i)
                            out << "  extra expected char at pos " << i << ": U+" << QString::number(original[i].unicode(), 16) << "\n";
                        for (int i = minLen; i < nfcExtracted.length(); ++i)
                            out << "  extra got char at pos " << i << ": U+" << QString::number(nfcExtracted[i].unicode(), 16) << "\n";
                    }
                    FPDFText_ClosePage(nfcTextPage);
                    FPDF_ClosePage(nfcPage2);
                    FPDF_CloseDocument(nfcDoc2);
                    FPDF_CloseDocument(nfcDoc);
                    FPDF_CloseDocument(doc);
                    PdfDocument::libRelease();
                    return 1;
                }
                FPDFText_ClosePage(nfcTextPage);
                FPDF_ClosePage(nfcPage2);
                FPDF_CloseDocument(nfcDoc2);
            }

            FPDF_CloseDocument(nfcDoc);
        }

        // Assertion 4: edit note preserves page object, orientation, HIDDEN flag, TRID, and text
        {
            QString editPath = outDir + QStringLiteral("/vnfont_edit.pdf");
            FPDF_DOCUMENT editDoc = FPDF_CreateNewDocument();
            if (!editDoc) {
                out << "VNFONT: FAIL edit create doc\n"; out.flush();
                FPDF_CloseDocument(doc); PdfDocument::libRelease(); return 1;
            }
            {
                QMutexLocker lock(&s_pdfiumMutex);
                FPDF_PAGE editPage = FPDFPage_New(editDoc, 0, pageW, pageH);
                if (!editPage) {
                    out << "VNFONT: FAIL edit create page\n"; out.flush();
                    FPDF_CloseDocument(editDoc); FPDF_CloseDocument(doc); PdfDocument::libRelease(); return 1;
                }
                FPDFPage_SetRotation(editPage, 1);
                FPDFPage_GenerateContent(editPage);
                FPDF_ClosePage(editPage);
            }

            {
                AnnotationManager editMgr;
                editMgr.setDocument(editDoc, editPath);
                editMgr.createInlineNote(0, QRectF(50, 100, 400, 30), QStringLiteral("Trước khi sửa"), "tester", false, QColor(255,0,0), 12.0f);
            }

            FS_MATRIX beforeMat{};
            bool foundBefore = false;
            {
                QMutexLocker lock(&s_pdfiumMutex);
                FPDF_PAGE ep = FPDF_LoadPage(editDoc, 0);
                if (ep) {
                    int nObj = FPDFPage_CountObjects(ep);
                    for (int i = 0; i < nObj && !foundBefore; ++i) {
                        FPDF_PAGEOBJECT obj = FPDFPage_GetObject(ep, i);
                        if (!obj || FPDFPageObj_GetType(obj) != FPDF_PAGEOBJ_TEXT) continue;
                        int nMarks = FPDFPageObj_CountMarks(obj);
                        for (int m = 0; m < nMarks; ++m) {
                            FPDF_PAGEOBJECTMARK mark = FPDFPageObj_GetMark(obj, static_cast<unsigned long>(m));
                            if (!mark) continue;
                            unsigned long nameLen = 0;
                            if (!FPDFPageObjMark_GetName(mark, nullptr, 0, &nameLen)) continue;
                            std::vector<unsigned short> nameBuf(nameLen / 2 + 1, 0);
                            if (!FPDFPageObjMark_GetName(mark, reinterpret_cast<FPDF_WCHAR*>(nameBuf.data()), nameLen, &nameLen)) continue;
                            QString markName = QString::fromUtf16(reinterpret_cast<const char16_t*>(nameBuf.data()));
                            if (markName == QLatin1String("TRNote")) {
                                FPDFPageObj_GetMatrix(obj, &beforeMat);
                                foundBefore = true;
                                break;
                            }
                        }
                    }
                    FPDF_ClosePage(ep);
                }
            }
            if (!foundBefore) {
                out << "VNFONT: FAIL edit no-textobj-before\n"; out.flush();
                FPDF_CloseDocument(editDoc); FPDF_CloseDocument(doc); PdfDocument::libRelease(); return 1;
            }

            {
                AnnotationManager editMgr;
                editMgr.setDocument(editDoc, editPath);
                if (!editMgr.retextNote(0, 0, QStringLiteral("Sau khi sửa nội dung"))) {
                    out << "VNFONT: FAIL edit retext-returned-false\n"; out.flush();
                    FPDF_CloseDocument(editDoc); FPDF_CloseDocument(doc); PdfDocument::libRelease(); return 1;
                }
            }

            // a+b+c+d: check textobj exists, matrix unchanged, HIDDEN, TRID
            {
                QMutexLocker lock(&s_pdfiumMutex);
                FPDF_PAGE ep2 = FPDF_LoadPage(editDoc, 0);
                if (!ep2) {
                    out << "VNFONT: FAIL edit load-page-after\n"; out.flush();
                    FPDF_CloseDocument(editDoc); FPDF_CloseDocument(doc); PdfDocument::libRelease(); return 1;
                }

                bool textObjFound = false;
                FS_MATRIX afterMat{};
                int nObj2 = FPDFPage_CountObjects(ep2);
                for (int i = 0; i < nObj2 && !textObjFound; ++i) {
                    FPDF_PAGEOBJECT obj = FPDFPage_GetObject(ep2, i);
                    if (!obj || FPDFPageObj_GetType(obj) != FPDF_PAGEOBJ_TEXT) continue;
                    int nMarks = FPDFPageObj_CountMarks(obj);
                    for (int m = 0; m < nMarks; ++m) {
                        FPDF_PAGEOBJECTMARK mark = FPDFPageObj_GetMark(obj, static_cast<unsigned long>(m));
                        if (!mark) continue;
                        unsigned long nameLen = 0;
                        if (!FPDFPageObjMark_GetName(mark, nullptr, 0, &nameLen)) continue;
                        std::vector<unsigned short> nameBuf(nameLen / 2 + 1, 0);
                        if (!FPDFPageObjMark_GetName(mark, reinterpret_cast<FPDF_WCHAR*>(nameBuf.data()), nameLen, &nameLen)) continue;
                        QString markName = QString::fromUtf16(reinterpret_cast<const char16_t*>(nameBuf.data()));
                        if (markName == QLatin1String("TRNote")) {
                            FPDFPageObj_GetMatrix(obj, &afterMat);
                            textObjFound = true;
                            break;
                        }
                    }
                }
                if (!textObjFound) {
                    out << "VNFONT: FAIL edit textobj-gone\n"; out.flush();
                    FPDF_ClosePage(ep2); FPDF_CloseDocument(editDoc); FPDF_CloseDocument(doc); PdfDocument::libRelease(); return 1;
                }

                if (qAbs(beforeMat.a - afterMat.a) > 1e-4f || qAbs(beforeMat.b - afterMat.b) > 1e-4f ||
                    qAbs(beforeMat.c - afterMat.c) > 1e-4f || qAbs(beforeMat.d - afterMat.d) > 1e-4f) {
                    out << "VNFONT: FAIL edit matrix-changed\n";
                    out << "  before: a=" << beforeMat.a << " b=" << beforeMat.b << " c=" << beforeMat.c << " d=" << beforeMat.d << "\n";
                    out << "  after:  a=" << afterMat.a << " b=" << afterMat.b << " c=" << afterMat.c << " d=" << afterMat.d << "\n";
                    out.flush();
                    FPDF_ClosePage(ep2); FPDF_CloseDocument(editDoc); FPDF_CloseDocument(doc); PdfDocument::libRelease(); return 1;
                }

                FPDF_ANNOTATION annot = FPDFPage_GetAnnot(ep2, 0);
                if (!annot) {
                    out << "VNFONT: FAIL edit no-annot\n"; out.flush();
                    FPDF_ClosePage(ep2); FPDF_CloseDocument(editDoc); FPDF_CloseDocument(doc); PdfDocument::libRelease(); return 1;
                }
                int flags = FPDFAnnot_GetFlags(annot);
                if (!(flags & FPDF_ANNOT_FLAG_HIDDEN)) {
                    out << "VNFONT: FAIL edit not-hidden\n"; out.flush();
                    FPDFPage_CloseAnnot(annot); FPDF_ClosePage(ep2); FPDF_CloseDocument(editDoc); FPDF_CloseDocument(doc); PdfDocument::libRelease(); return 1;
                }
                if (!FPDFAnnot_HasKey(annot, "TRID")) {
                    out << "VNFONT: FAIL edit trid-lost\n"; out.flush();
                    FPDFPage_CloseAnnot(annot); FPDF_ClosePage(ep2); FPDF_CloseDocument(editDoc); FPDF_CloseDocument(doc); PdfDocument::libRelease(); return 1;
                }
                FPDFPage_CloseAnnot(annot);
                FPDF_ClosePage(ep2);
            }

            // Save, reopen, extract text, compare
            {
                QFile f(editPath);
                if (!f.open(QIODevice::WriteOnly)) {
                    out << "VNFONT: FAIL edit cannot open output\n"; out.flush();
                    FPDF_CloseDocument(editDoc); FPDF_CloseDocument(doc); PdfDocument::libRelease(); return 1;
                }
                TRFileWriter fw;
                fw.file = &f;
                fw.base.version = 1;
                fw.base.WriteBlock = TRFileWriter::WriteBlock;
                {
                    QMutexLocker lock(&s_pdfiumMutex);
                    if (!FPDF_SaveAsCopy(editDoc, &fw.base, 0)) {
                        out << "VNFONT: FAIL edit save\n"; out.flush();
                        FPDF_CloseDocument(editDoc); FPDF_CloseDocument(doc); PdfDocument::libRelease(); return 1;
                    }
                }
            }

            {
                QMutexLocker lock(&s_pdfiumMutex);
                FPDF_DOCUMENT reopenDoc = FPDF_LoadDocument(editPath.toUtf8().constData(), nullptr);
                if (!reopenDoc) {
                    out << "VNFONT: FAIL edit reopen doc\n"; out.flush();
                    FPDF_CloseDocument(editDoc); FPDF_CloseDocument(doc); PdfDocument::libRelease(); return 1;
                }
                FPDF_PAGE reopenPage = FPDF_LoadPage(reopenDoc, 0);
                if (!reopenPage) {
                    out << "VNFONT: FAIL edit reopen page\n"; out.flush();
                    FPDF_CloseDocument(reopenDoc); FPDF_CloseDocument(editDoc); FPDF_CloseDocument(doc); PdfDocument::libRelease(); return 1;
                }
                FPDF_TEXTPAGE textPage = FPDFText_LoadPage(reopenPage);
                if (!textPage) {
                    out << "VNFONT: FAIL edit FPDFText_LoadPage\n"; out.flush();
                    FPDF_ClosePage(reopenPage); FPDF_CloseDocument(reopenDoc);
                    FPDF_CloseDocument(editDoc); FPDF_CloseDocument(doc); PdfDocument::libRelease(); return 1;
                }
                int len = FPDFText_CountChars(textPage);
                QString extracted;
                for (int i = 0; i < len; ++i) {
                    unsigned int cp = FPDFText_GetUnicode(textPage, i);
                    extracted += QChar(static_cast<char32_t>(cp));
                }
                QString editExpected = QStringLiteral("Sau khi sửa nội dung");
                if (extracted == editExpected) {
                    out << "VNFONT: PASS edit\n";
                } else {
                    out << "VNFONT: FAIL edit text\n";
                    out << "  expected: \"" << editExpected << "\"\n";
                    out << "  got:      \"" << extracted << "\"\n";
                    int minLen = qMin(editExpected.length(), extracted.length());
                    for (int i = 0; i < minLen; ++i) {
                        if (editExpected[i] != extracted[i])
                            out << "  diff at pos " << i << ": expected U+" << QString::number(editExpected[i].unicode(), 16)
                                << " got U+" << QString::number(extracted[i].unicode(), 16) << "\n";
                    }
                    if (editExpected.length() != extracted.length()) {
                        for (int i = minLen; i < editExpected.length(); ++i)
                            out << "  extra expected char at pos " << i << ": U+" << QString::number(editExpected[i].unicode(), 16) << "\n";
                        for (int i = minLen; i < extracted.length(); ++i)
                            out << "  extra got char at pos " << i << ": U+" << QString::number(extracted[i].unicode(), 16) << "\n";
                    }
                    FPDFText_ClosePage(textPage);
                    FPDF_ClosePage(reopenPage); FPDF_CloseDocument(reopenDoc);
                    FPDF_CloseDocument(editDoc); FPDF_CloseDocument(doc); PdfDocument::libRelease();
                    return 1;
                }
                FPDFText_ClosePage(textPage);
                FPDF_ClosePage(reopenPage);
                FPDF_CloseDocument(reopenDoc);
            }

            FPDF_CloseDocument(editDoc);
        }

        // Bloat check: add 5 more notes, save, verify < 700 KB
        {
            for (int i = 0; i < 5; ++i) {
                mgr.createInlineNote(0, QRectF(50 + i * 10, 150 + i * 30, 400, 30), original, "tester", false, QColor(255,0,0), 12.0f);
            }
            QString bloatPath = outDir + QStringLiteral("/vnfont_bloat.pdf");
            {
                QFile f(bloatPath);
                if (!f.open(QIODevice::WriteOnly)) {
                    out << "VNFONT: FAIL cannot open bloat output\n"; out.flush();
                    FPDF_CloseDocument(doc); PdfDocument::libRelease(); return 1;
                }
                TRFileWriter fw;
                fw.file = &f;
                fw.base.version = 1;
                fw.base.WriteBlock = TRFileWriter::WriteBlock;
                QMutexLocker lock(&s_pdfiumMutex);
                if (!FPDF_SaveAsCopy(doc, &fw.base, 0)) {
                    out << "VNFONT: FAIL bloat save\n"; out.flush();
                    FPDF_CloseDocument(doc); PdfDocument::libRelease(); return 1;
                }
            }
            qint64 bloatSize = QFileInfo(bloatPath).size();
            if (bloatSize >= 700 * 1024) {
                out << "VNFONT: FAIL bloat=" << bloatSize << "\n";
                out.flush();
                FPDF_CloseDocument(doc); PdfDocument::libRelease();
                return 1;
            }
            out << "VNFONT: PASS bloat=" << bloatSize << "\n";
        }

        FPDF_CloseDocument(doc);
        PdfDocument::libRelease();
        return 0;
    }

    // usage: --find-test <input.pdf> <query>
    // Headless text search test: reports match count, first 5 match details,
    // and rotation handling verification.
    if (argc >= 4 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--find-test")) {
        QTextStream out(stdout);
        QString inputPath = QString::fromLocal8Bit(argv[2]);
        QString query = QString::fromLocal8Bit(argv[3]);

        PdfDocument::libAddRef();

        int totalMatches = 0;
        int rotatedPagesFound = 0;
        QList<QString> rotationReports;

        {
            PdfDocument doc;
            if (!doc.open(inputPath)) {
                out << "FIND_TEST_FAIL cannot open " << inputPath << "\n";
                out.flush();
                PdfDocument::libRelease();
                return 1;
            }

            int pageCount = doc.pageCount();
            out << "File: " << inputPath << "\n";
            out << "Query: " << query << "\n";
            out << "Pages: " << pageCount << "\n";

            for (int pi = 0; pi < pageCount; ++pi) {
                QMutexLocker lock(&s_pdfiumMutex);
                FPDF_PAGE page = FPDF_LoadPage(doc.raw(), pi);
                if (!page) continue;

                int rot = FPDFPage_GetRotation(page);
                double dispW = FPDF_GetPageWidth(page);
                double dispH = FPDF_GetPageHeight(page);
                const QPointF box = pdfBoxOrigin(page);

                FPDF_TEXTPAGE textPage = FPDFText_LoadPage(page);
                if (!textPage) { FPDF_ClosePage(page); continue; }

                unsigned long flags = 0;
                FPDF_SCHHANDLE search = FPDFText_FindStart(
                    textPage, reinterpret_cast<FPDF_WIDESTRING>(query.utf16()), flags, 0);

                int pageMatches = 0;
                while (FPDFText_FindNext(search)) {
                    int charIdx = FPDFText_GetSchResultIndex(search);
                    int charCount = FPDFText_GetSchCount(search);

                    double left = 1e9, topPdf = -1e9, right = -1e9, bottomPdf = 1e9;
                    for (int c = charIdx; c < charIdx + charCount; ++c) {
                        double cl, ct, cr, cb;
                        FPDFText_GetCharBox(textPage, c, &cl, &cr, &cb, &ct);
                        left     = qMin(left, cl);
                        right    = qMax(right, cr);
                        topPdf   = qMax(topPdf, ct);
                        bottomPdf = qMin(bottomPdf, cb);
                    }

                    QRectF pdfRect(left, bottomPdf, right - left, topPdf - bottomPdf);
                    QRectF dispRect = pdfRectToDisp(pdfRect, dispW, dispH, rot, box.x(), box.y());

                    int snippetStart = qMax(0, charIdx - 20);
                    int snippetLen = charCount + 40;
                    std::vector<unsigned short> buf(snippetLen + 1, 0);
                    FPDFText_GetText(textPage, snippetStart, snippetLen, buf.data());
                    QString snippet = QString::fromUtf16(buf.data()).trimmed();

                    ++pageMatches;
                    ++totalMatches;

                    if (totalMatches <= 5)
                        out << "Match " << totalMatches
                            << ": page=" << (pi + 1)
                            << " rectPdf=(" << pdfRect.x() << "," << pdfRect.y()
                            << " " << pdfRect.width() << "x" << pdfRect.height() << ")"
                            << " rectDisp=(" << dispRect.x() << "," << dispRect.y()
                            << " " << dispRect.width() << "x" << dispRect.height() << ")"
                            << " snippet=\"" << snippet.left(50) << "\"\n";
                }

                if (rot != 0 && pageMatches > 0) {
                    ++rotatedPagesFound;
                    rotationReports << QString("  Page %1 rot=%2: %3 match(es)")
                                          .arg(pi + 1).arg(rot).arg(pageMatches);
                }

                FPDFText_FindClose(search);
                FPDFText_ClosePage(textPage);
                FPDF_ClosePage(page);
            }

            out << "Total matches: " << totalMatches << "\n";
            if (rotatedPagesFound > 0) {
                out << "Rotated pages with matches (" << rotatedPagesFound << "):\n";
                for (const auto& r : rotationReports)
                    out << r << "\n";
            } else {
                out << "No rotated pages with matches found.\n";
            }
            out << "FIND_TEST_OK\n";
            out.flush();
        }

        PdfDocument::libRelease();

        // Write report file
        QFile report(QDir::currentPath() + "/find_test_report.txt");
        if (report.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream rs(&report);
            rs << "TorReader PDF Find Test Report\n";
            rs << "==============================\n";
            rs << "Input: " << inputPath << "\n";
            rs << "Query: " << query << "\n";
            rs << "Total matches: " << totalMatches << "\n";
            if (rotatedPagesFound > 0) {
                rs << "Rotated pages with matches: " << rotatedPagesFound << "\n";
                for (const auto& r : rotationReports)
                    rs << r << "\n";
            }
            rs << "Status: PASS\n";
            report.close();
            out << "Report written to find_test_report.txt\n";
        } else {
            out << "FIND_TEST_WARN could not write report file\n";
        }
        out.flush();
        return totalMatches > 0 ? 0 : 2; // 0=found, 2=no matches (not a failure)
    }

    // usage: --links-test <input.pdf>
    // Probe (SPEC_PDF_LINKS muc 3): dem link moi trang + hit-test o tam tung
    // rect (ca trang xoay lan khong xoay). Dinh kem dong quy doi nguoc de
    // chung minh hit-test dung.
    if (argc >= 3 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--links-test")) {
        QTextStream out(stdout);
        QString inputPath = QString::fromLocal8Bit(argv[2]);

        PdfDocument::libAddRef();
        int pass = 0, fail = 0;
        {
            PdfDocument doc;
            if (!doc.open(inputPath)) {
                out << "LINKS_TEST_FAIL cannot open " << inputPath << "\n";
                out.flush();
                PdfDocument::libRelease();
                return 1;
            }
            int pageCount = doc.pageCount();
            out << "File: " << inputPath << "\n";
            out << "Pages: " << pageCount << "\n";

            for (int pi = 0; pi < pageCount; ++pi) {
                const QVector<PdfLink> links = PdfLinks::forPage(doc.raw(), pi);
                const PdfLinks::PageInfo info = PdfLinks::pageInfo(doc.raw(), pi);
                out << "[links] page=" << (pi + 1)
                    << " count=" << links.size()
                    << " rot=" << info.rot
                    << " disp=" << info.dispW << "x" << info.dispH
                    << " box=(" << info.boxX << "," << info.boxY << ")\n";
                for (int li = 0; li < links.size(); ++li) {
                    const PdfLink& L = links[li];
                    const QString kind = !L.uri.isEmpty() ? "uri"
                        : (L.destPage >= 0 ? "goto" : "unsupported");
                    const QString target = !L.uri.isEmpty() ? L.uri
                        : (L.destPage >= 0
                           ? QString("page%1(x=%2,y=%3)")
                               .arg(L.destPage + 1).arg(L.destX).arg(L.destY)
                           : QString());
                    const QRectF r = L.rectPdf.normalized();
                    out << "[links] page=" << (pi + 1)
                        << " rect=(" << r.left() << "," << r.bottom()
                        << "," << r.width() << "x" << r.height() << ")"
                        << " kind=" << kind << " target=" << target << "\n";

                    // Hit-test: rect -> disp -> tam -> quy doi nguoc ve page.
                    // Lua y: link co the CHONG nhau (rect nho nam trong rect lon)
                    // — linkAt tra link DONG NHAT thoat ra truoc. Vi vay chi can
                    // hitIdx>=0 la hop le; exact=true chung to tai tam rect tra ve
                    // DUNG link do (khong chong — tai lieu test dan cho nay).
                    const QRectF dispR = pdfRectToDisp(r, info.dispW, info.dispH,
                                                       info.rot, info.boxX, info.boxY);
                    const QPointF dispCenter = dispR.center();
                    const QPointF back = PdfLinks::dispToPdf(dispCenter, info);
                    const int hitIdx = PdfLinks::linkAt(links, dispCenter, info);
                    const bool exact = (hitIdx == li);
                    out << "[links]   hit-test center=(" << dispCenter.x() << ","
                        << dispCenter.y() << ") backToPdf=(" << back.x() << ","
                        << back.y() << ") inside=" << (r.contains(back) ? "true" : "false")
                        << " linkAt=" << (hitIdx >= 0 ? "true" : "false")
                        << " exact=" << (exact ? "true" : "false") << "\n";
                    if (hitIdx >= 0 && r.contains(back)) ++pass; else ++fail;
                }
            }
            out << "[links] PASS=" << pass << " FAIL=" << fail << "\n";
            out << (fail == 0 ? "LINKS_TEST_OK\n" : "LINKS_TEST_FAIL\n");
            out.flush();
        }
        PdfDocument::libRelease();
        return fail == 0 ? 0 : 1;
    }

    // usage: --markup-lifecycle <input.pdf>
    // Headless lifecycle test for every markup tool: create → undo → redo → delete.
    // Verifies TRUID-based undo never corrupts non-TRUID annots (Fix 3).
    if (argc >= 2 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--markup-lifecycle")) {
        QString inputPath = (argc >= 3) ? QString::fromLocal8Bit(argv[2]) : QString();
        QTextStream out(stdout);
        QFile reportFile(QDir::currentPath() + QStringLiteral("/markup_lifecycle_report.txt"));
        QTextStream rep(&reportFile);
        auto report = [&](const QString& label, bool pass) {
            out << (pass ? "PASS" : "FAIL") << " " << label << "\n";
            if (reportFile.isOpen()) rep << (pass ? "PASS" : "FAIL") << " " << label << "\n";
        };

        PdfDocument::libAddRef();
        FPDF_DOCUMENT doc = nullptr;
        if (!inputPath.isEmpty()) {
            doc = FPDF_LoadDocument(inputPath.toUtf8().constData(), nullptr);
        }
        if (!doc) {
            doc = FPDF_CreateNewDocument();
            FPDF_PAGE np = FPDFPage_New(doc, 0, 612, 792);
            FPDFPage_GenerateContent(np);
            FPDF_ClosePage(np);
        }
        int pageCount = FPDF_GetPageCount(doc);
        if (pageCount < 1) {
            out << "FAIL no pages\n"; out.flush(); PdfDocument::libRelease(); return 1;
        }
        if (reportFile.open(QIODevice::WriteOnly | QIODevice::Text)) rep.setEncoding(QStringConverter::Utf8);
        out << "Markup Lifecycle Test — pages=" << pageCount << "\n"; out.flush();

        auto countAnnots = [&](int pi) -> int {
            QMutexLocker lock(&s_pdfiumMutex);
            FPDF_PAGE p = FPDF_LoadPage(doc, pi);
            int n = p ? FPDFPage_GetAnnotCount(p) : -1;
            if (p) FPDF_ClosePage(p);
            return n;
        };
        auto countObjs = [&](int pi) -> int {
            QMutexLocker lock(&s_pdfiumMutex);
            FPDF_PAGE p = FPDF_LoadPage(doc, pi);
            int n = p ? FPDFPage_CountObjects(p) : -1;
            if (p) FPDF_ClosePage(p);
            return n;
        };

        AnnotStyle style;
        style.strokeColor = Qt::red;
        style.strokeWidth = 2.0f;
        style.opacity = 1.0f;
        QPointF pA(100, 100), pB(300, 200);

        // ── Test 1-9: each tool ──
        struct TC { const char* name; AnnotTool tool; bool isNote; bool isText; };
        TC tools[] = {
            {"Line",      AnnotTool::Line,      false, false},
            {"Arrow",     AnnotTool::Arrow,     false, false},
            {"Rect",      AnnotTool::Rectangle, false, false},
            {"Ellipse",   AnnotTool::Ellipse,   false, false},
            {"Cloud",     AnnotTool::Cloud,     false, false},
            {"Freehand",  AnnotTool::Freehand,  false, false},
            {"Highlight", AnnotTool::Highlight, false, false},
            {"Note",      AnnotTool::TextComment, true, false},
            {"FreeText",  AnnotTool::FreeText,  false, true},
        };
        int pi = 0;
        for (const auto& tc : tools) {
            AnnotationManager mgr; mgr.setDocument(doc, QString());
            AnnotationLayer layer; layer.setDocument(doc); layer.setAnnotationManager(&mgr);
            int a0 = countAnnots(pi), o0 = countObjs(pi);
            // Create
            if (tc.isNote) {
                mgr.createPopupNote(pi, pA, QStringLiteral("%1 test").arg(tc.name), QStringLiteral("tester"));
            } else if (tc.isText) {
                mgr.createInlineNote(pi, QRectF(pA, QSizeF(160, 24)),
                    QStringLiteral("%1 test").arg(tc.name), QStringLiteral("tester"), false, Qt::black);
            } else {
                QVector<QPointF> pts;
                if (tc.tool == AnnotTool::Freehand) {
                    for (int k = 0; k < 10; ++k)
                        pts.append(QPointF(pA.x() + k * 20, pA.y() + (k % 3) * 15));
                }
                layer.commitAnnotation(pi, tc.tool, style, pA, pB, pts);
            }
            int a1 = countAnnots(pi);
            bool okCreate = (a1 == a0 + 1);
            report(QString("%1-create").arg(tc.name), okCreate);
            if (!okCreate) continue; // abort this tool if create failed

            // Undo (simulate doUndo: find by uid, removeAnnot)
            QString uid = mgr.lastCreatedUid().isEmpty() ? layer.lastCreatedUid() : mgr.lastCreatedUid();
            if (!uid.isEmpty()) {
                int idx = mgr.findAnnotIndexByUid(pi, uid);
                bool okUndo = (idx >= 0) && mgr.removeAnnot(pi, idx);
                int a2 = countAnnots(pi);
                report(QString("%1-undo").arg(tc.name), okUndo && a2 == a0);
            } else {
                report(QString("%1-undo").arg(tc.name), false);
            }

            // Redo (recreate)
            if (tc.isNote) {
                mgr.createPopupNote(pi, pA, QStringLiteral("%1 test redo").arg(tc.name), QStringLiteral("tester"));
            } else if (tc.isText) {
                mgr.createInlineNote(pi, QRectF(pA, QSizeF(160, 24)),
                    QStringLiteral("%1 test redo").arg(tc.name), QStringLiteral("tester"), false, Qt::black);
            } else {
                QVector<QPointF> pts;
                if (tc.tool == AnnotTool::Freehand) {
                    for (int k = 0; k < 10; ++k)
                        pts.append(QPointF(pA.x() + k * 20, pA.y() + (k % 3) * 15));
                }
                layer.commitAnnotation(pi, tc.tool, style, pA, pB, pts);
            }
            int a3 = countAnnots(pi);
            report(QString("%1-redo").arg(tc.name), a3 == a1);

            // Delete (simulate: remove by uid)
            QString uid2 = mgr.lastCreatedUid().isEmpty() ? layer.lastCreatedUid() : mgr.lastCreatedUid();
            if (!uid2.isEmpty()) {
                int idx2 = mgr.findAnnotIndexByUid(pi, uid2);
                bool okDel = (idx2 >= 0) && mgr.removeAnnot(pi, idx2);
                int a4 = countAnnots(pi);
                report(QString("%1-delete").arg(tc.name), okDel && a4 == a0);
            } else {
                report(QString("%1-delete").arg(tc.name), false);
            }
        }
        // ── Test 10: TRUID protection (non-TRUID annots survive undo) ──
        {
            AnnotationManager mgr; mgr.setDocument(doc, QString());
            // Create 2 non-TRUID annots (simulate file-original / Widget)
            FPDF_PAGE tp = FPDF_LoadPage(doc, pi);
            if (tp) {
                for (int k = 0; k < 2; ++k) {
                    FPDF_ANNOTATION wa = FPDFPage_CreateAnnot(tp, FPDF_ANNOT_SQUARE);
                    if (wa) {
                        FS_RECTF wr{ static_cast<float>(50 + k*60), 600.0f, static_cast<float>(100 + k*60), 560.0f };
                        FPDFAnnot_SetRect(wa, &wr);
                        FPDFPage_CloseAnnot(wa);
                    }
                }
                FPDFPage_GenerateContent(tp);
                FPDF_ClosePage(tp);
            }
            int aBase = countAnnots(pi);
            report(QString("pretend-original-annots-created"), aBase >= 2);

            // Create a new TRUID annot
            mgr.createPopupNote(pi, QPointF(100, 400), QStringLiteral("new annot"), QStringLiteral("tester"));
            QString uid = mgr.lastCreatedUid();
            int aAfter1 = countAnnots(pi);
            report(QString("truid-create-count-plus1"), aAfter1 == aBase + 1);

            // Undo the new annot by TRUID — original 2 must survive
            int idx = mgr.findAnnotIndexByUid(pi, uid);
            bool okUndo = (idx >= 0) && mgr.removeAnnot(pi, idx);
            int aAfterUndo = countAnnots(pi);
            report(QString("truid-undo-removes-only-one"), okUndo && aAfterUndo == aBase);
            report(QString("truid-original-annots-survive"), aAfterUndo >= 2);
        }
        out << "\n--- markup_lifecycle_report.txt ---\n";
        if (reportFile.isOpen()) {
            rep << "\n--- END ---\n"; rep.flush(); reportFile.close();
            QFile rf2(QDir::currentPath() + QStringLiteral("/markup_lifecycle_report.txt"));
            if (rf2.open(QIODevice::ReadOnly | QIODevice::Text))
                out << QString::fromUtf8(rf2.readAll());
        }
        out << "MARKUP_LIFECYCLE_DONE\n";
        out.flush();
        FPDF_CloseDocument(doc);
        PdfDocument::libRelease();
        return 0;
    }

    // usage: --movebench <input.pdf> [pageIndex]
    // Headless benchmark: measure GenerateContent, create note, move note, render times
    if (argc >= 3 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--movebench")) {
        QString inputPath = QString::fromLocal8Bit(argv[2]);
        int pageIndex = (argc >= 4) ? QString::fromLocal8Bit(argv[3]).toInt() : 0;
        QTextStream out(stdout);

        PdfDocument::libAddRef();
        FPDF_DOCUMENT doc = FPDF_LoadDocument(inputPath.toUtf8().constData(), nullptr);
        if (!doc) {
            out << "MOVEBENCH: FAIL cannot open " << inputPath << "\n"; out.flush();
            PdfDocument::libRelease(); return 1;
        }
        int pageCount = FPDF_GetPageCount(doc);
        if (pageIndex < 0 || pageIndex >= pageCount) {
            out << "MOVEBENCH: FAIL pageIndex " << pageIndex << " out of range (pages=" << pageCount << ")\n"; out.flush();
            FPDF_CloseDocument(doc); PdfDocument::libRelease(); return 1;
        }

        // Page info
        double pageW = 0, pageH = 0;
        int pageRot = 0, pageObjCount = 0;
        {
            QMutexLocker lock(&s_pdfiumMutex);
            FPDF_PAGE p = FPDF_LoadPage(doc, pageIndex);
            if (p) {
                pageW = FPDF_GetPageWidth(p);
                pageH = FPDF_GetPageHeight(p);
                pageRot = FPDFPage_GetRotation(p);
                pageObjCount = FPDFPage_CountObjects(p);
                FPDF_ClosePage(p);
            }
        }
        out << "MOVEBENCH page=" << pageIndex << " size=" << pageW << "x" << pageH
            << " rot=" << pageRot << " objects=" << pageObjCount << "\n"; out.flush();

        AnnotationManager mgr;
        mgr.setDocument(doc, inputPath);

        // Step A: single GenerateContent, 3 times
        qint64 genTimes[3];
        qint64 genTotal = 0;
        for (int i = 0; i < 3; ++i) {
            QElapsedTimer t; t.start();
            mgr.generateContentForPage(pageIndex);
            genTimes[i] = t.elapsed();
            genTotal += genTimes[i];
        }
        qint64 genAvg = genTotal / 3;
        out << "MOVEBENCH gen_content_ms: " << genTimes[0] << " " << genTimes[1] << " " << genTimes[2]
            << " avg=" << genAvg << "\n"; out.flush();

        // Step B: create inline note
        QElapsedTimer t; t.start();
        mgr.createInlineNote(pageIndex, QRectF(50, 100, 300, 30),
            QStringLiteral("Move bench"), QStringLiteral("bench"),
            false, QColor(255, 0, 0), 12.0f);
        qint64 createMs = t.elapsed();
        out << "MOVEBENCH create_note_ms: " << createMs << "\n"; out.flush();

        // Step C: move note, 5 times
        int annotCount = 0;
        {
            QMutexLocker lock(&s_pdfiumMutex);
            FPDF_PAGE p = FPDF_LoadPage(doc, pageIndex);
            if (p) { annotCount = FPDFPage_GetAnnotCount(p); FPDF_ClosePage(p); }
        }
        int idx = annotCount - 1;
        qint64 moveTimes[5];
        qint64 moveTotal = 0;
        for (int i = 0; i < 5; ++i) {
            QElapsedTimer t2; t2.start();
            mgr.moveAnnot(pageIndex, idx, 20.0, 15.0);
            moveTimes[i] = t2.elapsed();
            moveTotal += moveTimes[i];
            {
                QMutexLocker lock(&s_pdfiumMutex);
                FPDF_PAGE p = FPDF_LoadPage(doc, pageIndex);
                if (p) { annotCount = FPDFPage_GetAnnotCount(p); FPDF_ClosePage(p); }
            }
            idx = annotCount - 1;
        }
        qint64 moveAvg = moveTotal / 5;
        out << "MOVEBENCH move_note_ms: " << moveTimes[0] << " " << moveTimes[1] << " " << moveTimes[2]
            << " " << moveTimes[3] << " " << moveTimes[4] << " avg=" << moveAvg << "\n"; out.flush();

        // Step D: render page at 1.5x, 2 times
        qint64 renderTimes[2];
        {
            QMutexLocker lock(&s_pdfiumMutex);
            FPDF_PAGE p = FPDF_LoadPage(doc, pageIndex);
            if (p) {
                int w = qMax(1, static_cast<int>(pageW * 1.5));
                int h = qMax(1, static_cast<int>(pageH * 1.5));
                for (int i = 0; i < 2; ++i) {
                    QImage image(w, h, QImage::Format_ARGB32);
                    image.fill(Qt::white);
                    FPDF_BITMAP bmp = FPDFBitmap_CreateEx(w, h, FPDFBitmap_BGRA,
                                                          image.bits(), image.bytesPerLine());
                    QElapsedTimer t3; t3.start();
                    FPDF_RenderPageBitmap(bmp, p, 0, 0, w, h, 0, FPDF_ANNOT);
                    renderTimes[i] = t3.elapsed();
                    FPDFBitmap_Destroy(bmp);
                }
                FPDF_ClosePage(p);
            } else {
                renderTimes[0] = renderTimes[1] = -1;
            }
        }
        out << "MOVEBENCH render_page_ms: " << renderTimes[0] << " " << renderTimes[1] << "\n"; out.flush();

        double genShare = (moveAvg > 0) ? (4.0 * genAvg / moveAvg * 100.0) : 0.0;
        out << "MOVEBENCH SUMMARY objects=" << pageObjCount
            << " gen_content_avg=" << genAvg
            << " move_avg=" << moveAvg
            << " render=" << renderTimes[0]
            << " gen_share=" << QString::number(genShare, 'f', 1) << "%\n"; out.flush();

        FPDF_CloseDocument(doc);
        PdfDocument::libRelease();
        return 0;
    }

    // usage: --pinlrubench <input.pdf> [pageA] [pageB]
    // Headless bench for the pin LRU: pin A, pin B, pin A, pin B. The 3rd/4th pins
    // must be LRU hits (hit=1, ms=0) instead of reloading from disk (hit=0).
    if (argc >= 3 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--pinlrubench")) {
        QString inputPath = QString::fromLocal8Bit(argv[2]);
        int pageA = (argc >= 4) ? QString::fromLocal8Bit(argv[3]).toInt() : 4;
        int pageB = (argc >= 5) ? QString::fromLocal8Bit(argv[4]).toInt() : pageA + 1;
        QTextStream out(stdout);

        PdfDocument::libAddRef();
        FPDF_DOCUMENT doc = FPDF_LoadDocument(inputPath.toUtf8().constData(), nullptr);
        if (!doc) {
            out << "PINLRUBENCH: FAIL cannot open " << inputPath << "\n"; out.flush();
            PdfDocument::libRelease(); return 1;
        }
        int pageCount = FPDF_GetPageCount(doc);
        if (pageA < 0 || pageB < 0 || pageA >= pageCount || pageB >= pageCount) {
            out << "PINLRUBENCH: FAIL pages out of range pages=" << pageCount << "\n"; out.flush();
            FPDF_CloseDocument(doc); PdfDocument::libRelease(); return 1;
        }

        AnnotationManager mgr;
        mgr.setDocument(doc, inputPath);
        out << "PINLRUBENCH file=" << inputPath << " pages=" << pageCount
            << " pinA=" << pageA << " pinB=" << pageB << "\n"; out.flush();

        for (int p : {pageA, pageB, pageA, pageB}) mgr.pinPage(p);

        mgr.releaseSharedPage();
        FPDF_CloseDocument(doc);
        PdfDocument::libRelease();
        return 0;
    }

    // usage: --pinlruflush <input.pdf> [pageA] [pageB]
    // Prove the pin LRU is flushed when the document is swapped (SPEC item 4):
    // pin 5 pages on doc1 (fills the LRU and evicts one), swap to a fresh doc2,
    // re-pin pageA/pageB — must be fresh loads (hit=0), no stale FPDF_PAGE reuse,
    // no crash. setDocument() is the same path the GUI uses on close/reopen.
    if (argc >= 3 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--pinlruflush")) {
        QString inputPath = QString::fromLocal8Bit(argv[2]);
        int pageA = (argc >= 4) ? QString::fromLocal8Bit(argv[3]).toInt() : 4;
        int pageB = (argc >= 5) ? QString::fromLocal8Bit(argv[4]).toInt() : pageA + 1;
        QTextStream out(stdout);

        PdfDocument::libAddRef();
        FPDF_DOCUMENT doc1 = FPDF_LoadDocument(inputPath.toUtf8().constData(), nullptr);
        if (!doc1) {
            out << "PINLRUFLUSH: FAIL cannot open " << inputPath << "\n"; out.flush();
            PdfDocument::libRelease(); return 1;
        }
        int pageCount = FPDF_GetPageCount(doc1);
        if (pageA < 0 || pageB < 0 || pageA >= pageCount || pageB >= pageCount) {
            out << "PINLRUFLUSH: FAIL pages out of range pages=" << pageCount << "\n"; out.flush();
            FPDF_CloseDocument(doc1); PdfDocument::libRelease(); return 1;
        }

        out << "PINLRUFLUSH file=" << inputPath << " pages=" << pageCount << "\n"; out.flush();
        AnnotationManager mgr;
        mgr.setDocument(doc1, inputPath);
        out << "PINLRUFLUSH: pin 5 pages on doc1 (fill LRU + evict one)\n"; out.flush();
        for (int p : {0, 1, 2, 3, 4}) mgr.pinPage(p);

        FPDF_DOCUMENT doc2 = FPDF_LoadDocument(inputPath.toUtf8().constData(), nullptr);
        out << "PINLRUFLUSH: setDocument(doc2) — LRU must be flushed\n"; out.flush();
        mgr.setDocument(doc2, inputPath);

        out << "PINLRUFLUSH: re-pin pageA/pageB on doc2 (must be hit=0, fresh load)\n"; out.flush();
        for (int p : {pageA, pageB, pageA, pageB}) mgr.pinPage(p);

        mgr.releaseSharedPage();
        FPDF_CloseDocument(doc2);
        FPDF_CloseDocument(doc1);
        PdfDocument::libRelease();
        out << "PINLRUFLUSH: OK\n"; out.flush();
        return 0;
    }

    // usage: --savebench <input.pdf>
    // Headless save benchmark: measure each step of the save pipeline.
    if (argc >= 3 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--savebench")) {
        QTextStream out(stdout);
        PdfDocument::libAddRef();

        QString inputPath = QString::fromLocal8Bit(argv[2]);
        QFileInfo fi(inputPath);
        if (!fi.exists()) {
            out << "SAVEBENCH: FAIL cannot open " << inputPath << "\n"; out.flush();
            PdfDocument::libRelease(); return 1;
        }
        QString workPath = inputPath + QStringLiteral(".savebench.pdf");
        QFile::remove(workPath);
        if (!QFile::copy(inputPath, workPath)) {
            out << "SAVEBENCH: FAIL cannot copy to " << workPath << "\n"; out.flush();
            PdfDocument::libRelease(); return 1;
        }

        PdfDocument doc;
        if (!doc.open(workPath)) {
            out << "SAVEBENCH: FAIL open document\n"; out.flush();
            QFile::remove(workPath); PdfDocument::libRelease(); return 1;
        }
        int pageCount = doc.pageCount();
        qint64 fileSize = QFileInfo(workPath).size();
        out << "SAVEBENCH pages=" << pageCount << " size=" << fileSize << "\n"; out.flush();

        AnnotationManager mgr;
        mgr.setDocument(doc.raw(), workPath);

        int nNotes = qMin(5, pageCount);
        QSet<int> dirtyPages;
        for (int i = 0; i < nNotes; ++i) {
            mgr.createInlineNote(i, QRectF(50, 100 + i * 30, 300, 30),
                QStringLiteral("Save bench note %1").arg(i + 1),
                QStringLiteral("bench"), false, QColor(255, 0, 0), 12.0f);
            dirtyPages.insert(i);
        }

        qint64 A = 0, B = 0, C = 0, D1 = 0, D2 = 0, D3 = 0;

        {
            QElapsedTimer t; t.start();
            for (int pg : dirtyPages)
                mgr.generateContentForPage(pg);
            A = t.elapsed();
            out << "SAVEBENCH gen_content_ms: " << A << " (pages=" << dirtyPages.size() << ")\n"; out.flush();
        }

        QString tmpPath = workPath + QStringLiteral(".savebench_tmp.pdf");
        {
            mgr.setDocument(doc.raw(), tmpPath);
            QElapsedTimer t; t.start();
            mgr.saveDocument();
            B = t.elapsed();
            qint64 tmpSize = QFileInfo(tmpPath).size();
            out << "SAVEBENCH save_document_ms: " << B << " tmp_size=" << tmpSize << "\n"; out.flush();
        }

        doc.close();
        {
            extern bool replaceFileAtomically(const QString& srcTmp, const QString& dest, QString* errOut);
            QString err;
            QElapsedTimer t; t.start();
            if (!replaceFileAtomically(tmpPath, workPath, &err)) {
                C = t.elapsed();
                out << "SAVEBENCH: FAIL replaceFileAtomically: " << err << "\n"; out.flush();
                QFile::remove(tmpPath); QFile::remove(workPath); PdfDocument::libRelease(); return 1;
            }
            C = t.elapsed();
            out << "SAVEBENCH replace_file_ms: " << C << "\n"; out.flush();
        }

        {
            QElapsedTimer t; t.start();
            TileCacheFile::hashFile(workPath);
            D1 = t.elapsed();
            out << "SAVEBENCH hash_file_ms: " << D1 << "\n"; out.flush();
        }
        {
            PdfDocument d2;
            QElapsedTimer t; t.start();
            if (!d2.open(workPath)) {
                out << "SAVEBENCH: FAIL reopen document\n"; out.flush();
                QFile::remove(workPath); PdfDocument::libRelease(); return 1;
            }
            D2 = t.elapsed();
            out << "SAVEBENCH reopen_doc_ms: " << D2 << "\n"; out.flush();
        }
        {
            ThumbnailRenderPool pool;
            QElapsedTimer t; t.start();
            if (!pool.open(workPath)) {
                out << "SAVEBENCH: FAIL ThumbnailRenderPool open\n"; out.flush();
                QFile::remove(workPath); PdfDocument::libRelease(); return 1;
            }
            pool.close();
            D3 = t.elapsed();
            out << "SAVEBENCH thumbpool_open_ms: " << D3 << "\n"; out.flush();
        }

        qint64 total = A + B + C + D1 + D2 + D3;
        out << "SAVEBENCH TOTAL_ms: " << total
            << "  (gen=" << A << " save=" << B << " replace=" << C
            << " hash=" << D1 << " reopen=" << D2 << " thumbpool=" << D3 << ")\n"; out.flush();

        QFile::remove(workPath);
        QFile::remove(tmpPath);
        PdfDocument::libRelease();
        return 0;
    }

    // usage: --dedup-test <input.pdf>
    // Chung minh dedupStreams KHONG doi noi dung hien thi cua bat ky trang nao.
    if (argc >= 3 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--dedup-test")) {
        QTextStream out(stdout);
        const QString inPath = QString::fromLocal8Bit(argv[2]);
        PdfDocument::libAddRef();

        auto hashPages = [](const QString& p, QList<QByteArray>& outHashes, QString& err) -> bool {
            QMutexLocker lock(&s_pdfiumMutex);
            FPDF_DOCUMENT d = FPDF_LoadDocument(p.toUtf8().constData(), nullptr);
            if (!d) { err = "cannot open " + p; return false; }
            const int n = FPDF_GetPageCount(d);
            for (int i = 0; i < n; ++i) {
                FPDF_PAGE pg = FPDF_LoadPage(d, i);
                if (!pg) { FPDF_CloseDocument(d); err = QString("cannot load page %1").arg(i); return false; }
                const int w = qMax(1, qRound(FPDF_GetPageWidth(pg)));
                const int h = qMax(1, qRound(FPDF_GetPageHeight(pg)));
                FPDF_BITMAP bmp = FPDFBitmap_Create(w, h, 1);
                FPDFBitmap_FillRect(bmp, 0, 0, w, h, 0xFFFFFFFF);
                FPDF_RenderPageBitmap(bmp, pg, 0, 0, w, h, 0, FPDF_ANNOT);
                const int stride = FPDFBitmap_GetStride(bmp);
                QCryptographicHash hh(QCryptographicHash::Sha256);
                trHashAdd(hh, static_cast<const char*>(FPDFBitmap_GetBuffer(bmp)),
                          static_cast<qsizetype>(stride) * h);
                outHashes.append(hh.result());
                FPDFBitmap_Destroy(bmp);
                FPDF_ClosePage(pg);
            }
            FPDF_CloseDocument(d);
            return true;
        };

        QList<QByteArray> before, after;
        QString err;
        if (!hashPages(inPath, before, err)) {
            out << "DEDUP: FAIL " << err << "\n"; out.flush();
            PdfDocument::libRelease(); return 1;
        }
        const qint64 sizeBefore = QFileInfo(inPath).size();

        const QString workPath = inPath + ".dedup.pdf";
        QFile::remove(workPath);
        if (!QFile::copy(inPath, workPath)) {
            out << "DEDUP: FAIL cannot copy to " << workPath << "\n"; out.flush();
            PdfDocument::libRelease(); return 1;
        }

        PdfEditor editor;
        if (!editor.dedupStreams(workPath)) {
            out << "DEDUP: FAIL dedupStreams returned false: " << editor.lastError() << "\n"; out.flush();
            PdfDocument::libRelease(); return 1;
        }
        const qint64 sizeAfter = QFileInfo(workPath).size();

        if (!hashPages(workPath, after, err)) {
            out << "DEDUP: FAIL " << err << "\n"; out.flush();
            PdfDocument::libRelease(); return 1;
        }

        if (before.size() != after.size()) {
            out << "DEDUP: FAIL pagecount " << before.size() << " vs " << after.size() << "\n";
            out.flush(); PdfDocument::libRelease(); return 1;
        }
        for (int i = 0; i < before.size(); ++i) {
            if (before[i] != after[i]) {
                out << "DEDUP: FAIL page " << i << " render-differs\n";
                out.flush(); PdfDocument::libRelease(); return 1;
            }
        }

        const double pct = sizeBefore > 0 ? (100.0 * (sizeBefore - sizeAfter) / sizeBefore) : 0.0;
        out << "DEDUP: PASS pages=" << before.size()
            << " before=" << sizeBefore << " after=" << sizeAfter
            << " saved=" << QString::number(pct, 'f', 1) << "%\n";
        out.flush();
        PdfDocument::libRelease();
        return 0;
    }

    // usage: --rendernote-test <input.pdf> <pageIndex>
    // Headless diagnosis: verify that a newly added inline note (FreeText)
    // actually changes the rendered bitmap.
    if (argc >= 4 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--rendernote-test")) {
        QString inputPath = QString::fromLocal8Bit(argv[2]);
        int pageIndex = QString::fromLocal8Bit(argv[3]).toInt();
        QTextStream out(stdout);

        PdfDocument::libAddRef();

        FPDF_DOCUMENT doc = nullptr;
        {
            QMutexLocker lock(&s_pdfiumMutex);
            doc = FPDF_LoadDocument(inputPath.toUtf8().constData(), nullptr);
        }
        if (!doc) {
            out << "RENDERNOTE: FAIL cannot open " << inputPath << "\n";
            out.flush(); PdfDocument::libRelease(); return 1;
        }
        int pageCount = FPDF_GetPageCount(doc);
        if (pageIndex < 0 || pageIndex >= pageCount) {
            out << "RENDERNOTE: FAIL pageIndex " << pageIndex << " out of range (pages=" << pageCount << ")\n";
            out.flush();
            { QMutexLocker lock(&s_pdfiumMutex); FPDF_CloseDocument(doc); }
            PdfDocument::libRelease(); return 1;
        }

        double pageW = 0, pageH = 0;
        {
            QMutexLocker lock(&s_pdfiumMutex);
            FPDF_PAGE tp = FPDF_LoadPage(doc, pageIndex);
            if (tp) { pageW = FPDF_GetPageWidth(tp); pageH = FPDF_GetPageHeight(tp); FPDF_ClosePage(tp); }
        }
        int imgW = qMax(1, (int)pageW);
        int imgH = qMax(1, (int)pageH);
        out << "RENDERNOTE: page=" << pageIndex << " size=" << pageW << "x" << pageH
            << " bitmap=" << imgW << "x" << imgH << "\n";
        out.flush();

        auto renderAndHash = [&]() -> QByteArray {
            QMutexLocker lock(&s_pdfiumMutex);
            FPDF_PAGE p = FPDF_LoadPage(doc, pageIndex);
            if (!p) return {};
            QImage image(imgW, imgH, QImage::Format_ARGB32);
            image.fill(Qt::white);
            FPDF_BITMAP bmp = FPDFBitmap_CreateEx(imgW, imgH, FPDFBitmap_BGRA,
                                                  image.bits(), image.bytesPerLine());
            FPDF_RenderPageBitmap(bmp, p, 0, 0, imgW, imgH, 0, FPDF_ANNOT);
            FPDFBitmap_Destroy(bmp);
            FPDF_ClosePage(p);
            QCryptographicHash hh(QCryptographicHash::Sha256);
            trHashAdd(hh, (const char*)image.bits(), (qsizetype)image.bytesPerLine() * imgH);
            return hh.result();
        };

        QByteArray hashTruoc = renderAndHash();
        if (hashTruoc.isEmpty()) {
            out << "RENDERNOTE: FAIL render before\n"; out.flush();
            { QMutexLocker lock(&s_pdfiumMutex); FPDF_CloseDocument(doc); }
            PdfDocument::libRelease(); return 1;
        }

        int objBefore = 0;
        {
            QMutexLocker lock(&s_pdfiumMutex);
            FPDF_PAGE p = FPDF_LoadPage(doc, pageIndex);
            if (p) { objBefore = FPDFPage_CountObjects(p); FPDF_ClosePage(p); }
        }

        {
            AnnotationManager mgr;
            mgr.setDocument(doc, inputPath);
            bool ok = mgr.createInlineNote(pageIndex, QRectF(50,100,300,30),
                                           QStringLiteral("RENDERTEST"), QStringLiteral("t"),
                                           false, QColor(255,0,0), 24.0f);
            out << "RENDERNOTE: createInlineNote returned=" << (ok ? "true" : "false") << "\n";
            out.flush();
        }

        int objAfter = 0;
        {
            QMutexLocker lock(&s_pdfiumMutex);
            FPDF_PAGE p = FPDF_LoadPage(doc, pageIndex);
            if (p) { objAfter = FPDFPage_CountObjects(p); FPDF_ClosePage(p); }
        }

        QByteArray hashSau = renderAndHash();
        if (hashSau.isEmpty()) {
            out << "RENDERNOTE: FAIL render after\n"; out.flush();
            { QMutexLocker lock(&s_pdfiumMutex); FPDF_CloseDocument(doc); }
            PdfDocument::libRelease(); return 1;
        }

        bool match = (hashTruoc == hashSau);
        out << "RENDERNOTE: hashTruoc=" << hashTruoc.toHex() << "\n";
        out << "RENDERNOTE: hashSau=" << hashSau.toHex()
            << " diff=" << (match ? "false" : "true") << "\n";
        out << "RENDERNOTE: objCount before=" << objBefore << " after=" << objAfter << "\n";
        out.flush();

        if (match) {
            out << "RENDERNOTE: FAIL trang KHONG doi sau khi them note (chu khong vao noi dung trang)\n";
            out << "RENDERNOTE: DIAG — goi GenerateContent thu cong...\n";
            {
                AnnotationManager mgr;
                mgr.setDocument(doc, inputPath);
                mgr.generateContentForPage(pageIndex);
            }
            QByteArray hashGen = renderAndHash();
            if (!hashGen.isEmpty()) {
                bool genMatch = (hashTruoc == hashGen);
                out << "RENDERNOTE: DIAG hash sau GenerateContent=" << hashGen.toHex()
                    << " diff=" << (genMatch ? "false" : "true") << "\n";
                out.flush();
            }
            { QMutexLocker lock(&s_pdfiumMutex); FPDF_CloseDocument(doc); }
            PdfDocument::libRelease();
            return 1;
        }

        out << "RENDERNOTE: PASS chu da xuat hien tren trang\n";
        out.flush();
        { QMutexLocker lock(&s_pdfiumMutex); FPDF_CloseDocument(doc); }
        PdfDocument::libRelease();
        return 0;
    }

    // usage: --annotvis-test <pdf> <pageIndex>
    // Trang co FreeText HIEN thi overlayCapable phai = 1 (foreign layer handles them)
    if (argc >= 4 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--annotvis-test")) {
        QString inputPath = QString::fromLocal8Bit(argv[2]);
        int pageIndex = QString::fromLocal8Bit(argv[3]).toInt();
        QTextStream out(stdout);

        PdfDocument::libAddRef();

        PdfDocument doc;
        if (!doc.open(inputPath)) {
            out << "ANNOTVIS: FAIL cannot open " << inputPath << "\n";
            out.flush(); PdfDocument::libRelease(); return 1;
        }

        AnnotationManager mgr;
        mgr.setDocument(doc.raw(), inputPath);
        bool capable = true;
        mgr.loadPageVisuals(pageIndex, &capable);

        int k = 0;
        {
            QMutexLocker lock(&s_pdfiumMutex);
            FPDF_PAGE page = FPDF_LoadPage(doc.raw(), pageIndex);
            if (!page) {
                out << "ANNOTVIS: FAIL cannot load page " << pageIndex << "\n";
                out.flush(); PdfDocument::libRelease(); return 1;
            }
            int n = FPDFPage_GetAnnotCount(page);
            for (int i = 0; i < n; ++i) {
                FPDF_ANNOTATION annot = FPDFPage_GetAnnot(page, i);
                if (!annot) continue;
                int flags = FPDFAnnot_GetFlags(annot);
                if (flags & FPDF_ANNOT_FLAG_HIDDEN) { FPDFPage_CloseAnnot(annot); continue; }
                int subtype = FPDFAnnot_GetSubtype(annot);
                if (subtype == FPDF_ANNOT_FREETEXT || subtype == FPDF_ANNOT_TEXT)
                    ++k;
                FPDFPage_CloseAnnot(annot);
            }
            FPDF_ClosePage(page);
        }

        out << "ANNOTVIS page=" << pageIndex << " freetext_hien=" << k << " capable=" << (capable ? 1 : 0) << "\n";
        out.flush();

        if (k >= 1 && !capable) {
            out << "ANNOTVIS: FAIL co " << k << " FreeText hien ma capable=0 (foreign layer dang le xu ly)\n";
            out.flush(); PdfDocument::libRelease(); return 1;
        }
        if (k >= 1)
            out << "ANNOTVIS: NOTE foreign layer handles " << k << " visible non-TorReader annotations\n";

        if (k >= 1) {
            double pageW = 0, pageH = 0;
            {
                QMutexLocker lock(&s_pdfiumMutex);
                FPDF_PAGE p = FPDF_LoadPage(doc.raw(), pageIndex);
                if (p) { pageW = FPDF_GetPageWidth(p); pageH = FPDF_GetPageHeight(p); FPDF_ClosePage(p); }
            }
            int w = qMax(1, (int)pageW);
            int h = qMax(1, (int)pageH);

            auto renderAndHash = [&](int flags) -> QByteArray {
                QMutexLocker lock(&s_pdfiumMutex);
                FPDF_PAGE p = FPDF_LoadPage(doc.raw(), pageIndex);
                if (!p) return {};
                QImage image(w, h, QImage::Format_ARGB32);
                image.fill(Qt::white);
                FPDF_BITMAP bmp = FPDFBitmap_CreateEx(w, h, FPDFBitmap_BGRA,
                                                      image.bits(), image.bytesPerLine());
                FPDF_RenderPageBitmap(bmp, p, 0, 0, w, h, 0, flags);
                FPDFBitmap_Destroy(bmp);
                FPDF_ClosePage(p);
                QCryptographicHash hh(QCryptographicHash::Sha256);
                trHashAdd(hh, (const char*)image.bits(), (qsizetype)image.bytesPerLine() * h);
                return hh.result();
            };

            QElapsedTimer t0; t0.start();
            QByteArray h0 = renderAndHash(0);
            qint64 ms0 = t0.elapsed();
            QElapsedTimer t1; t1.start();
            QByteArray h1 = renderAndHash(FPDF_ANNOT);
            qint64 ms1 = t1.elapsed();
            if (h0.isEmpty() || h1.isEmpty()) {
                out << "ANNOTVIS: FAIL render failed\n";
                out.flush(); PdfDocument::libRelease(); return 1;
            }
            if (h0 == h1) {
                out << "ANNOTVIS: FAIL render giong nhau, annotation khong co noi dung hien thi\n";
                out.flush(); PdfDocument::libRelease(); return 1;
            }

            out << "ANNOTVIS render_ms: khong_annot=" << ms0 << " co_annot=" << ms1 << " chenh=" << (ms1 - ms0) << "\n";
            out.flush();
        }

        out << "ANNOTVIS: PASS\n";
        out.flush();
        PdfDocument::libRelease();
        return 0;
    }

#endif

    // usage: --foreignlayer-test <pdf> <pageIndex> <out.png>
    // Headless: verify buildForeignAnnotLayer — foreign anootation layer extraction
    // and TRUID flag restore.
    if (argc >= 5 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--foreignlayer-test")) {
        QTextStream out(stdout);
        QString pdfPath = QString::fromLocal8Bit(argv[2]);
        int pageIndex = QString::fromLocal8Bit(argv[3]).toInt();
        QString outPng = QString::fromLocal8Bit(argv[4]);

        PdfDocument::libAddRef();
        FPDF_DOCUMENT doc = FPDF_LoadDocument(pdfPath.toUtf8().constData(), nullptr);
        if (!doc) {
            out << "FOREIGNLAYER: FAIL cannot open " << pdfPath << "\n"; out.flush();
            PdfDocument::libRelease(); return 1;
        }

        int pageCount = FPDF_GetPageCount(doc);
        if (pageIndex < 0 || pageIndex >= pageCount) {
            out << "FOREIGNLAYER: FAIL pageIndex " << pageIndex << " out of range (pages=" << pageCount << ")\n";
            out.flush(); FPDF_CloseDocument(doc); PdfDocument::libRelease(); return 1;
        }

        double pageW = 0, pageH = 0;
        {
            QMutexLocker lock(&s_pdfiumMutex);
            FPDF_PAGE p = FPDF_LoadPage(doc, pageIndex);
            if (p) { pageW = FPDF_GetPageWidth(p); pageH = FPDF_GetPageHeight(p); FPDF_ClosePage(p); }
        }

        int wPx = 1200;
        int hPx = qMax(1, static_cast<int>(1200.0 * pageH / qMax(1.0, pageW)));

        // Dem so annot truoc khi goi ham (check truoc khi goi)
        int k = 0;
        int truidHiddenBefore = 0;
        {
            QMutexLocker lock(&s_pdfiumMutex);
            FPDF_PAGE p = FPDF_LoadPage(doc, pageIndex);
            if (p) {
                int n = FPDFPage_GetAnnotCount(p);
                for (int i = 0; i < n; ++i) {
                    FPDF_ANNOTATION a = FPDFPage_GetAnnot(p, i);
                    if (!a) continue;
                    int flags = FPDFAnnot_GetFlags(a);
                    bool hidden = (flags & FPDF_ANNOT_FLAG_HIDDEN) != 0;
                    bool isForeign = !hidden && FPDFAnnot_HasKey(a, "TRUID") == 0;
                    if (isForeign) ++k;
                    if (FPDFAnnot_HasKey(a, "TRUID") && hidden) ++truidHiddenBefore;
                    FPDFPage_CloseAnnot(a);
                }
                FPDF_ClosePage(p);
            }
        }
        out << "FOREIGNLAYER: page=" << pageIndex << " foreign annots=" << k
            << " truid-hidden-before=" << truidHiddenBefore << "\n"; out.flush();

        {
            AnnotationManager mgr;
            mgr.setDocument(doc, pdfPath);
            QImage layer = mgr.buildForeignAnnotLayer(pageIndex, wPx, hPx);

            // Khang dinh 1:
            if (k >= 1 && layer.isNull()) {
                out << "FOREIGNLAYER: FAIL co " << k << " annot ngoai ma lop rong\n";
                out.flush(); FPDF_CloseDocument(doc); PdfDocument::libRelease(); return 1;
            }
            if (k == 0 && !layer.isNull()) {
                out << "FOREIGNLAYER: FAIL khong co annot ngoai ma lop khong rong\n";
                out.flush(); FPDF_CloseDocument(doc); PdfDocument::libRelease(); return 1;
            }

            // Khang dinh 2: co Hidden da duoc tra lai
            int truidHiddenAfter = 0;
            {
                QMutexLocker lock(&s_pdfiumMutex);
                FPDF_PAGE p = FPDF_LoadPage(doc, pageIndex);
                if (p) {
                    int n = FPDFPage_GetAnnotCount(p);
                    for (int i = 0; i < n; ++i) {
                        FPDF_ANNOTATION a = FPDFPage_GetAnnot(p, i);
                        if (!a) continue;
                        int flags = FPDFAnnot_GetFlags(a);
                        if (FPDFAnnot_HasKey(a, "TRUID") && (flags & FPDF_ANNOT_FLAG_HIDDEN))
                            ++truidHiddenAfter;
                        FPDFPage_CloseAnnot(a);
                    }
                    FPDF_ClosePage(p);
                }
            }
            if (truidHiddenBefore != truidHiddenAfter) {
                out << "FOREIGNLAYER: FAIL co Hidden khong duoc tra lai (truoc="
                    << truidHiddenBefore << " sau=" << truidHiddenAfter << ")\n";
                out.flush(); FPDF_CloseDocument(doc); PdfDocument::libRelease(); return 1;
            }

            if (!layer.isNull())
                layer.save(outPng);

            out << "FOREIGNLAYER: PASS page=" << pageIndex << " annot_ngoai=" << k
                << " layer=" << layer.width() << "x" << layer.height() << "\n";
            out.flush();
        }

        FPDF_CloseDocument(doc);
        PdfDocument::libRelease();
        return 0;
    }

    // ── Render probe (headless measurement: resolution-bound vs content-bound) ──
    // usage: TorReader.exe --render-probe <pdf_path> <page_number_1based>
    if (argc >= 4 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--render-probe")) {
        QString pdfPath = QString::fromLocal8Bit(argv[2]);
        int pageNum = QString::fromLocal8Bit(argv[3]).toInt();
        if (pageNum < 1) { fprintf(stderr, "RENDER_PROBE_FAIL page number must be >= 1\n"); return 1; }

        PdfDocument::libAddRef();
        QTextStream out(stdout);
        {
            PdfDocument doc;
            if (!doc.open(pdfPath)) {
                fprintf(stderr, "RENDER_PROBE_FAIL cannot open %s\n", pdfPath.toUtf8().constData());
                PdfDocument::libRelease();
                return 1;
            }

            FPDF_PAGE page = nullptr;
            qint64 loadPageMs = 0;
            {
                QMutexLocker lock(&s_pdfiumMutex);
                QElapsedTimer timer;
                timer.start();
                page = FPDF_LoadPage(doc.raw(), pageNum - 1);
                loadPageMs = timer.elapsed();
            }
            if (!page) {
                fprintf(stderr, "RENDER_PROBE_FAIL cannot load page %d\n", pageNum);
                PdfDocument::libRelease();
                return 1;
            }

            double pageW = 0, pageH = 0;
            {
                QMutexLocker lock(&s_pdfiumMutex);
                pageW = FPDF_GetPageWidth(page);
                pageH = FPDF_GetPageHeight(page);
            }

            out << "=== RENDER PROBE ===\n";
            out << "File: " << pdfPath << "\n";
            out << "Page: " << pageNum << "\n";
            out << "Page size (pts): " << QString::number(pageW, 'f', 1)
                << " x " << QString::number(pageH, 'f', 1) << "\n";
            out << "LoadPage time: " << loadPageMs << " ms\n";
            out << "Probe flags: FPDF_ANNOT | FPDF_RENDER_LIMITEDIMAGECACHE\n";

            const int resolutions[] = {200, 400, 800, 1600, 3000, 4000};
            const int nRes = 6;
            const qint64 TIMEOUT_MS = 60000;

            struct ResResult {
                int longEdge;
                int w, h;
                qint64 renderMs = -1;
                bool timeout = false;
            };
            QVector<ResResult> results;
            results.reserve(nRes);

            for (int ri = 0; ri < nRes; ++ri) {
                int longEdge = resolutions[ri];
                int w, h;
                if (pageW >= pageH) {
                    w = longEdge;
                    h = qMax(1, (int)(pageH / pageW * longEdge));
                } else {
                    h = longEdge;
                    w = qMax(1, (int)(pageW / pageH * longEdge));
                }

                QImage image(w, h, QImage::Format_ARGB32);
                image.fill(Qt::white);

                ResResult rr;
                rr.longEdge = longEdge;
                rr.w = w;
                rr.h = h;

                QElapsedTimer timer;
                timer.start();
                {
                    QMutexLocker lock(&s_pdfiumMutex);
                    FPDF_BITMAP bmp = FPDFBitmap_CreateEx(w, h, FPDFBitmap_BGRA,
                                                           image.bits(), image.bytesPerLine());
                    FPDF_RenderPageBitmap(bmp, page, 0, 0, w, h, 0,
                                          FPDF_ANNOT | FPDF_RENDER_LIMITEDIMAGECACHE);
                    FPDFBitmap_Destroy(bmp);
                }
                qint64 elapsed = timer.elapsed();
                rr.renderMs = elapsed;

                double mb = w * h * 4.0 / (1024.0 * 1024.0);
                if (elapsed >= TIMEOUT_MS) {
                    rr.timeout = true;
                    out << "  " << longEdge << "px: " << w << "x" << h
                        << " bitmap=" << QString::number(mb, 'f', 1) << " MB"
                        << " render=" << elapsed << " ms TIMEOUT (>=60s, skipping larger)\n";
                    results.append(rr);
                    break;
                }
                out << "  " << longEdge << "px: " << w << "x" << h
                    << " bitmap=" << QString::number(mb, 'f', 1) << " MB"
                    << " render=" << elapsed << " ms\n";
                results.append(rr);
            }

            // Second render at 800px (reuse FPDF_PAGE) to check PDFium internal cache
            {
                int longEdge = 800;
                int w, h;
                if (pageW >= pageH) {
                    w = longEdge;
                    h = qMax(1, (int)(pageH / pageW * longEdge));
                } else {
                    h = longEdge;
                    w = qMax(1, (int)(pageW / pageH * longEdge));
                }
                QImage image(w, h, QImage::Format_ARGB32);
                image.fill(Qt::white);
                QElapsedTimer timer;
                timer.start();
                {
                    QMutexLocker lock(&s_pdfiumMutex);
                    FPDF_BITMAP bmp = FPDFBitmap_CreateEx(w, h, FPDFBitmap_BGRA,
                                                           image.bits(), image.bytesPerLine());
                    FPDF_RenderPageBitmap(bmp, page, 0, 0, w, h, 0,
                                          FPDF_ANNOT | FPDF_RENDER_LIMITEDIMAGECACHE);
                    FPDFBitmap_Destroy(bmp);
                }
                qint64 secondMs = timer.elapsed();
                out << "Second render at 800px: " << secondMs << " ms (reusing same FPDF_PAGE)\n";
            }

            // Determine COST type
            QVector<ResResult> valid;
            for (const auto& r : results)
                if (!r.timeout) valid.append(r);

            if (valid.size() >= 2) {
                qint64 minTime = valid.first().renderMs;
                qint64 maxTime = valid.first().renderMs;
                for (const auto& r : valid) {
                    if (r.renderMs < minTime) minTime = r.renderMs;
                    if (r.renderMs > maxTime) maxTime = r.renderMs;
                }
                double ratio = (double)maxTime / qMax((qint64)1, minTime);

                if (ratio < 2.0)
                    out << "COST=CONTENT-BOUND (ha do phan giai KHONG giup)\n";
                else
                    out << "COST=RESOLUTION-BOUND (ha do phan giai CO giup)\n";
            } else {
                out << "COST=CONTENT-BOUND (only 1 level before timeout)\n";
            }

            out << "RENDER_PROBE_OK\n";
            out.flush();

            { QMutexLocker lock(&s_pdfiumMutex); FPDF_ClosePage(page); }
        }
        PdfDocument::libRelease();
        return 0;
    }

    // ── Vector probe (headless measurement for GPU renderer architecture) ──────
    // usage: TorReader.exe --vector-probe <pdf_path> <page_number_1based>
    if (argc >= 4 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--vector-probe")) {
        QString pdfPath = QString::fromLocal8Bit(argv[2]);
        int pageNum = QString::fromLocal8Bit(argv[3]).toInt();
        if (pageNum < 1) { fprintf(stderr, "VECTOR_PROBE_FAIL page number must be >= 1\n"); return 1; }

#ifdef _WIN32
        PROCESS_MEMORY_COUNTERS pmcBefore{};
        PROCESS_MEMORY_COUNTERS pmcAfter{};
        GetProcessMemoryInfo(GetCurrentProcess(), &pmcBefore, sizeof(pmcBefore));
#endif

        PdfDocument::libAddRef();
        {
            PdfDocument doc;
            if (!doc.open(pdfPath)) {
                fprintf(stderr, "VECTOR_PROBE_FAIL cannot open %s\n", pdfPath.toUtf8().constData());
                PdfDocument::libRelease();
                return 1;
            }

            FPDF_PAGE page = nullptr;
            qint64 loadPageUs = 0;
            {
                QMutexLocker lock(&s_pdfiumMutex);
                QElapsedTimer timer;
                timer.start();
                page = FPDF_LoadPage(doc.raw(), pageNum - 1);
                loadPageUs = timer.nsecsElapsed() / 1000;
            }
            if (!page) {
                fprintf(stderr, "VECTOR_PROBE_FAIL cannot load page %d\n", pageNum);
                PdfDocument::libRelease();
                return 1;
            }

            struct ProbeCounts {
                int paths = 0, texts = 0, images = 0, shadings = 0, forms = 0, unknown = 0;
                int totalSegments = 0;
                int moveTo = 0, lineTo = 0, bezierTo = 0;
            };
            struct ProbeCounter {
                ProbeCounts c;
                void countObject(FPDF_PAGEOBJECT obj) {
                    int type = FPDFPageObj_GetType(obj);
                    switch (type) {
                    case FPDF_PAGEOBJ_PATH: {
                        ++c.paths;
                        int segs = FPDFPath_CountSegments(obj);
                        if (segs <= 0) break;
                        c.totalSegments += segs;
                        for (int s = 0; s < segs; ++s) {
                            FPDF_PATHSEGMENT seg = FPDFPath_GetPathSegment(obj, s);
                            if (!seg) continue;
                            switch (FPDFPathSegment_GetType(seg)) {
                            case FPDF_SEGMENT_MOVETO:  ++c.moveTo; break;
                            case FPDF_SEGMENT_LINETO:  ++c.lineTo; break;
                            case FPDF_SEGMENT_BEZIERTO: ++c.bezierTo; break;
                            }
                        }
                        break;
                    }
                    case FPDF_PAGEOBJ_TEXT:    ++c.texts; break;
                    case FPDF_PAGEOBJ_IMAGE:   ++c.images; break;
                    case FPDF_PAGEOBJ_SHADING: ++c.shadings; break;
                    case FPDF_PAGEOBJ_FORM: {
                        ++c.forms;
                        int sub = FPDFFormObj_CountObjects(obj);
                        for (unsigned long j = 0; j < (unsigned long)sub; ++j) {
                            FPDF_PAGEOBJECT subObj = FPDFFormObj_GetObject(obj, j);
                            if (subObj) countObject(subObj);
                        }
                        break;
                    }
                    default: ++c.unknown; break;
                    }
                }
                void countPage(FPDF_PAGE page) {
                    int n = FPDFPage_CountObjects(page);
                    for (int i = 0; i < n; ++i) {
                        FPDF_PAGEOBJECT obj = FPDFPage_GetObject(page, i);
                        if (obj) countObject(obj);
                    }
                }
            };

            // ── State grouping types ──
            struct PathStateRec {
                bool fillOk = false; unsigned int fr=0,fg=0,fb=0,fa=0;
                bool strokeOk = false; unsigned int sr=0,sg=0,sb=0,sa=0;
                float sw = 0;
                int fillMode = 0;
                bool hasStroke = false;
                QVector<float> dash;
            };

            ProbeCounter counter;
            qint64 traverseUs = 0;
            {
                QMutexLocker lock(&s_pdfiumMutex);
                QElapsedTimer timer;
                timer.start();
                counter.countPage(page);
                traverseUs = timer.nsecsElapsed() / 1000;
            }

            const auto& c = counter.c;

            // ── State grouping & draw mode analysis (second pass) ──
            auto makeStateKey = [](const PathStateRec& s) -> QString {
                QStringList parts;
                parts << QString::number(s.fillOk) << QString::number(s.fr)
                      << QString::number(s.fg) << QString::number(s.fb) << QString::number(s.fa)
                      << QString::number(s.strokeOk)
                      << QString::number(s.sr) << QString::number(s.sg) << QString::number(s.sb) << QString::number(s.sa)
                      << QString::number(double(s.sw), 'f', 6)
                      << QString::number(s.fillMode) << QString::number(s.hasStroke)
                      << QString::number(s.dash.size());
                for (float d : s.dash)
                    parts << QString::number(double(d), 'f', 6);
                return parts.join(QChar('|'));
            };
            auto describeState = [](const PathStateRec& s) -> QString {
                QString d;
                if (s.fillOk)
                    d += QStringLiteral(" fill=rgba(%1,%2,%3,%4)").arg(s.fr).arg(s.fg).arg(s.fb).arg(s.fa);
                else
                    d += QStringLiteral(" fill=none");
                if (s.strokeOk)
                    d += QStringLiteral(" stroke=rgba(%1,%2,%3,%4)").arg(s.sr).arg(s.sg).arg(s.sb).arg(s.sa);
                else
                    d += QStringLiteral(" stroke=none");
                d += QStringLiteral(" sw=") + QString::number(double(s.sw), 'f', 3);
                d += QStringLiteral(" drawMode=") + QString::number(s.fillMode) + (s.hasStroke ? QStringLiteral("+stroke") : QStringLiteral());
                if (!s.dash.isEmpty()) {
                    d += QStringLiteral(" dash=[");
                    for (int i = 0; i < s.dash.size(); ++i) {
                        if (i) d += QChar(',');
                        d += QString::number(double(s.dash[i]), 'f', 3);
                    }
                    d += QChar(']');
                }
                return d.trimmed();
            };
            struct GroupInfo { PathStateRec state; int count = 0; };
            QHash<QString, GroupInfo> stateGroups;
            int nStrokeOnly = 0, nFillOnly = 0, nBoth = 0, nDrawNone = 0;
            int clipPathCount = 0;
            bool clipBasicApiWorks = false;
            bool clipSegmentApiWorks = false;
            typedef void* (*GetClipPathFn)(FPDF_PAGEOBJECT);
            GetClipPathFn pGetClipPath = nullptr;
            typedef int (*CountClipPathsFn)(FPDF_CLIPPATH);
            CountClipPathsFn pCountClipPaths = nullptr;
            typedef int (*CountClipPathSegmentsFn)(FPDF_CLIPPATH, int);
            CountClipPathSegmentsFn pCountClipPathSegments = nullptr;
            typedef FPDF_PATHSEGMENT (*GetClipPathSegmentFn)(FPDF_CLIPPATH, int, int);
            GetClipPathSegmentFn pGetClipPathSegment = nullptr;
            QStringList missingApis;
            {
                QLibrary pdfiumLib(QStringLiteral("pdfium"));
                pGetClipPath = reinterpret_cast<GetClipPathFn>(pdfiumLib.resolve("FPDFPageObj_GetClipPath"));
                pCountClipPaths = reinterpret_cast<CountClipPathsFn>(pdfiumLib.resolve("FPDFClipPath_CountPaths"));
                pCountClipPathSegments = reinterpret_cast<CountClipPathSegmentsFn>(pdfiumLib.resolve("FPDFClipPath_CountPathSegments"));
                pGetClipPathSegment = reinterpret_cast<GetClipPathSegmentFn>(pdfiumLib.resolve("FPDFClipPath_GetPathSegment"));
                clipBasicApiWorks = (pGetClipPath && pCountClipPaths);
                clipSegmentApiWorks = clipBasicApiWorks && pCountClipPathSegments && pGetClipPathSegment;
                if (!clipBasicApiWorks) {
                    if (!pGetClipPath) missingApis << QStringLiteral("FPDFPageObj_GetClipPath");
                    if (!pCountClipPaths) missingApis << QStringLiteral("FPDFClipPath_CountPaths");
                } else if (!clipSegmentApiWorks) {
                    if (!pCountClipPathSegments) missingApis << QStringLiteral("FPDFClipPath_CountPathSegments");
                    if (!pGetClipPathSegment) missingApis << QStringLiteral("FPDFClipPath_GetPathSegment");
                }
            }

            struct ClipContentInfo {
                int count = 0;
                float xmin=0, ymin=0, xmax=0, ymax=0;
                bool hasBBox = false;
            };
            QHash<QString, ClipContentInfo> clipContentGroups;
            bool clipKeyTruncated = false;
            qint64 stateUs = 0;
            {
                QMutexLocker lock(&s_pdfiumMutex);
                QElapsedTimer timer2;
                timer2.start();
                std::function<void(FPDF_PAGEOBJECT)> collectState;
                collectState = [&](FPDF_PAGEOBJECT obj) {
                    int t = FPDFPageObj_GetType(obj);
                    if (t == FPDF_PAGEOBJ_FORM) {
                        int nSub = FPDFFormObj_CountObjects(obj);
                        for (unsigned long j = 0; j < (unsigned long)nSub; ++j) {
                            FPDF_PAGEOBJECT sub = FPDFFormObj_GetObject(obj, j);
                            if (sub) collectState(sub);
                        }
                        return;
                    }
                    if (t != FPDF_PAGEOBJ_PATH) return;
                    PathStateRec s;
                    s.fillOk = FPDFPageObj_GetFillColor(obj, &s.fr, &s.fg, &s.fb, &s.fa);
                    s.strokeOk = FPDFPageObj_GetStrokeColor(obj, &s.sr, &s.sg, &s.sb, &s.sa);
                    if (!FPDFPageObj_GetStrokeWidth(obj, &s.sw)) s.sw = 0;
                    int dc = FPDFPageObj_GetDashCount(obj);
                    if (dc > 0) { s.dash.resize(dc); FPDFPageObj_GetDashArray(obj, s.dash.data(), dc); }
                    FPDF_BOOL strokeFlag = 0;
                    if (!FPDFPath_GetDrawMode(obj, &s.fillMode, &strokeFlag)) { s.fillMode = 0; strokeFlag = 0; }
                    s.hasStroke = (strokeFlag != 0);
                    if (s.fillOk && s.hasStroke) ++nBoth;
                    else if (s.fillOk) ++nFillOnly;
                    else if (s.hasStroke) ++nStrokeOnly;
                    else ++nDrawNone;
                    QString key = makeStateKey(s);
                    auto it = stateGroups.find(key);
                    if (it == stateGroups.end()) { GroupInfo gi; gi.state = s; gi.count = 1; stateGroups.insert(key, gi); }
                    else it->count += 1;
                    if (clipSegmentApiWorks) {
                        FPDF_CLIPPATH clip = reinterpret_cast<FPDF_CLIPPATH>(pGetClipPath(obj));
                        if (clip) {
                            ++clipPathCount;
                            QStringList parts;
                            float xmin=1e30f, ymin=1e30f, xmax=-1e30f, ymax=-1e30f;
                            bool hasAnyPoint = false;
                            int nPaths = pCountClipPaths(clip);
                            if (nPaths >= 0) {
                                parts << QString::number(nPaths);
                                int totalSegsUsed = 0;
                                const int MAX_SEG = 64;
                                for (int pi = 0; pi < nPaths && totalSegsUsed < MAX_SEG; ++pi) {
                                    int nSegs = pCountClipPathSegments(clip, pi);
                                    if (nSegs < 0) { parts << QString::number(-1); continue; }
                                    int segsToTake = qMin(nSegs, MAX_SEG - totalSegsUsed);
                                    if (segsToTake < nSegs) clipKeyTruncated = true;
                                    parts << QString::number(nSegs);
                                    for (int si = 0; si < segsToTake; ++si) {
                                        FPDF_PATHSEGMENT seg = pGetClipPathSegment(clip, pi, si);
                                        if (!seg) continue;
                                        int segType = FPDFPathSegment_GetType(seg);
                                        parts << QString::number(segType);
                                        float x=0, y=0;
                                        if (FPDFPathSegment_GetPoint(seg, &x, &y)) {
                                            parts << QString::number(double(x), 'f', 2) + QChar(',') + QString::number(double(y), 'f', 2);
                                            if (x < xmin) xmin = x; if (y < ymin) ymin = y;
                                            if (x > xmax) xmax = x; if (y > ymax) ymax = y;
                                            hasAnyPoint = true;
                                        }
                                        ++totalSegsUsed;
                                    }
                                }
                            }
                            QString ck = parts.join(QChar('|'));
                            auto it = clipContentGroups.find(ck);
                            if (it == clipContentGroups.end()) {
                                ClipContentInfo ci;
                                ci.count = 1;
                                ci.xmin = xmin; ci.ymin = ymin; ci.xmax = xmax; ci.ymax = ymax;
                                ci.hasBBox = hasAnyPoint;
                                clipContentGroups.insert(ck, ci);
                            } else {
                                it->count += 1;
                            }
                        }
                    } else if (clipBasicApiWorks) {
                        void* clip = pGetClipPath(obj);
                        if (clip) ++clipPathCount;
                    }
                };
                int totalPageObj = FPDFPage_CountObjects(page);
                for (int i = 0; i < totalPageObj; ++i) {
                    FPDF_PAGEOBJECT obj = FPDFPage_GetObject(page, i);
                    if (obj) collectState(obj);
                }
                stateUs = timer2.nsecsElapsed() / 1000;
            }
            // Sort groups by count descending
            QVector<QPair<QString, GroupInfo>> sortedGroups;
            sortedGroups.reserve(stateGroups.size());
            for (auto it = stateGroups.begin(); it != stateGroups.end(); ++it)
                sortedGroups.append({it.key(), it.value()});
            std::sort(sortedGroups.begin(), sortedGroups.end(),
                [](const QPair<QString, GroupInfo>& a, const QPair<QString, GroupInfo>& b) {
                    return a.second.count > b.second.count;
                });
            int nSinglePathGroups = 0;
            for (auto& g : sortedGroups)
                if (g.second.count == 1) ++nSinglePathGroups;
            int top10Sum = 0;
            int topN = qMin(10, static_cast<int>(sortedGroups.size()));
            for (int i = 0; i < topN; ++i)
                top10Sum += sortedGroups[i].second.count;

#ifdef _WIN32
            GetProcessMemoryInfo(GetCurrentProcess(), &pmcAfter, sizeof(pmcAfter));
#endif

            QTextStream out(stdout);
            out << "=== VECTOR PROBE ===\n";
            out << "File: " << pdfPath << "\n";
            out << "Page: " << pageNum << "\n";
            out << "LoadPage time: " << QString::number(loadPageUs / 1000.0, 'f', 2) << " ms\n";
            out << "Traverse time: " << QString::number(traverseUs / 1000.0, 'f', 2) << " ms\n";
            out << "Objects by type:\n";
            out << "  PATH:    " << c.paths << "\n";
            out << "  TEXT:    " << c.texts << "\n";
            out << "  IMAGE:   " << c.images << "\n";
            out << "  SHADING: " << c.shadings << "\n";
            out << "  FORM:    " << c.forms << "\n";
            out << "  UNKNOWN: " << c.unknown << "\n";
            out << "Total:    " << (c.paths + c.texts + c.images + c.shadings + c.forms + c.unknown) << "\n";
            out << "Path segments: " << c.totalSegments << "\n";
            out << "  moveto:  " << c.moveTo << "\n";
            out << "  lineto:  " << c.lineTo << "\n";
            out << "  bezierto:" << c.bezierTo << "\n";
            double vboMB = c.totalSegments * 2.0 * 2.0 * 4.0 / (1024.0 * 1024.0);
            out << "VBO estimate (segments*2verts*2floats): " << QString::number(vboMB, 'f', 3) << " MB\n";
#ifdef _WIN32
            long memBefore = (long)(pmcBefore.WorkingSetSize / (1024 * 1024));
            long memAfter  = (long)(pmcAfter.WorkingSetSize / (1024 * 1024));
            out << "Working set before: " << memBefore << " MB\n";
            out << "Working set after:  " << memAfter << " MB\n";
            out << "Working set delta:  " << (memAfter - memBefore) << " MB\n";
#endif
            out << "State analysis time: " << QString::number(stateUs / 1000.0, 'f', 2) << " ms\n";
            out << "State groups:\n";
            out << "  Total distinct state groups: " << stateGroups.size() << "\n";
            out << "  Top " << topN << " groups cover " << top10Sum << " paths ("
                << QString::number(100.0 * top10Sum / qMax(c.paths, 1), 'f', 1) << "% of "
                << c.paths << " paths)\n";
            out << "  Groups with exactly 1 path: " << nSinglePathGroups << "\n";
            out << "Top 10 state groups:\n";
            for (int i = 0; i < topN; ++i) {
                const auto& g = sortedGroups[i];
                out << "  #" << (i+1) << ": count=" << g.second.count
                    << " =>" << describeState(g.second.state) << "\n";
            }
            out << "Draw mode classification:\n";
            out << "  fill only:  " << nFillOnly << "\n";
            out << "  stroke only: " << nStrokeOnly << "\n";
            out << "  fill+stroke: " << nBoth << "\n";
            out << "  none:        " << nDrawNone << "\n";
            out << "Clip analysis:\n";
            if (clipSegmentApiWorks) {
                out << "  Paths with clip: " << clipPathCount << "\n";
                out << "  Distinct clip groups (by content): " << clipContentGroups.size() << "\n";
                if (clipKeyTruncated)
                    out << "  NOTE: clip key uses 64-segment max sample (some clips truncated)\n";
                QVector<QPair<QString, ClipContentInfo>> sortedClips;
                sortedClips.reserve(clipContentGroups.size());
                for (auto it = clipContentGroups.begin(); it != clipContentGroups.end(); ++it)
                    sortedClips.append({it.key(), it.value()});
                std::sort(sortedClips.begin(), sortedClips.end(),
                    [](const QPair<QString, ClipContentInfo>& a, const QPair<QString, ClipContentInfo>& b) {
                        return a.second.count > b.second.count;
                    });
                int clipTopN = qMin(5, static_cast<int>(sortedClips.size()));
                out << "  Top " << clipTopN << " most common clips:\n";
                for (int i = 0; i < clipTopN; ++i) {
                    const auto& g = sortedClips[i];
                    out << "    #" << (i+1) << ": count=" << g.second.count;
                    if (g.second.hasBBox)
                        out << " bbox=(" << QString::number(double(g.second.xmin), 'f', 1)
                            << "," << QString::number(double(g.second.ymin), 'f', 1)
                            << "," << QString::number(double(g.second.xmax), 'f', 1)
                            << "," << QString::number(double(g.second.ymax), 'f', 1) << ")";
                    else
                        out << " bbox=N/A";
                    out << "\n";
                }
            } else if (clipBasicApiWorks) {
                out << "  Paths with clip: " << clipPathCount << "\n";
                out << "  Distinct clip groups: CANNOT MEASURE — missing APIs: "
                    << missingApis.join(QStringLiteral(", ")) << "\n";
            } else {
                out << "  API NOT AVAILABLE: FPDFPageObj_GetClipPath / FPDFClipPath_CountPaths\n";
            }
            if (!missingApis.isEmpty()) {
                out << "Missing APIs summary:\n";
                for (const QString& ma : missingApis)
                    out << "  WARNING: " << ma << " — stats skipped\n";
            }
            out << "VECTOR_PROBE_OK\n";
            out.flush();

            { QMutexLocker lock(&s_pdfiumMutex); FPDF_ClosePage(page); }
        }
        PdfDocument::libRelease();
        return 0;
    }

    // ── VectorGL Phase 1: extract PDF paths → OpenGL offscreen → PNG ────────
    // usage: --vectorgl <pdf_path> <page_1based> <out.png> [width_px]
    if (argc >= 5 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--vectorgl")) {
        QString pdfPath = QString::fromLocal8Bit(argv[2]);
        int pageNum = QString::fromLocal8Bit(argv[3]).toInt();
        QString outPng = QString::fromLocal8Bit(argv[4]);
        int targetW = (argc >= 6) ? QString::fromLocal8Bit(argv[5]).toInt() : 2000;
        if (pageNum < 1 || targetW < 10) {
            fprintf(stderr, "VECTORGL_FAIL invalid args (page>=1, width>=10)\n");
            return 1;
        }

        QTextStream out(stdout);
        QElapsedTimer tTotal;
        tTotal.start();

        // ── 1. Load page ──
        PdfDocument::libAddRef();
        PdfDocument doc;
        if (!doc.open(pdfPath)) {
            out << "VECTORGL_FAIL cannot open " << pdfPath << "\n"; out.flush();
            PdfDocument::libRelease(); return 1;
        }
        FPDF_PAGE page = nullptr;
        qint64 loadPageMs = 0;
        {
            QElapsedTimer t;
            t.start();
            QMutexLocker lock(&s_pdfiumMutex);
            page = FPDF_LoadPage(doc.raw(), pageNum - 1);
            loadPageMs = t.elapsed();
        }
        if (!page) {
            out << "VECTORGL_FAIL cannot load page " << pageNum << "\n"; out.flush();
            PdfDocument::libRelease(); return 1;
        }
        double pageW = FPDF_GetPageWidth(page);
        double pageH = FPDF_GetPageHeight(page);
        int wPx = targetW;
        int hPx = qMax(1, static_cast<int>(pageH / pageW * wPx));
        out << "LoadPage      : " << loadPageMs << " ms  (page " << pageNum
            << " size=" << QString::number(pageW,'f',1) << "x" << QString::number(pageH,'f',1)
            << " px=" << wPx << "x" << hPx << ")\n";

        // ── 2. Extract path geometry + §2.1 statistics ──
        struct Vtx { float x, y; uint8_t r, g, b, a; };
        QVector<Vtx> vertices;
        int pathCount = 0, segCount = 0, lineSegCount = 0;
        int subpathCount = 0, outOfRangeCount = 0;

        // §2.1 statistics
        int fillCount = 0, strokeCount = 0, bothCount = 0;
        QSet<quint32> uniqueStrokeColors, uniqueFillColors;
        QVector<float> allStrokeWidths;

        // §2.3 width-binned VBOs: 5 bins → ≤0.5, ≤1, ≤2, ≤4, >4
        const float widthLimits[] = {0.5f, 1.0f, 2.0f, 4.0f};
        QVector<Vtx> widthBins[5];
        QVector<Vtx> fillVerts;
        QVector<int> fillSubPathStarts; // start index in fillVerts for each subpath

        auto flattenCubic = [&](float x0, float y0, float cx1, float cy1,
                                float cx2, float cy2, float x3, float y3,
                                uint8_t cr, uint8_t cg, uint8_t cb, uint8_t ca) {
            const int N = 4;
            float prevX = x0, prevY = y0;
            for (int i = 1; i <= N; ++i) {
                float t = float(i) / N;
                float u = 1.0f - t;
                float u2 = u * u, u3 = u2 * u;
                float t2 = t * t, t3 = t2 * t;
                float px = u3*x0 + 3*u2*t*cx1 + 3*u*t2*cx2 + t3*x3;
                float py = u3*y0 + 3*u2*t*cy1 + 3*u*t2*cy2 + t3*y3;
                vertices.append({prevX, prevY, cr, cg, cb, ca});
                vertices.append({px, py, cr, cg, cb, ca});
                prevX = px; prevY = py;
            }
            lineSegCount += N;
        };

        qint64 traverseMs = 0;
        {
            QElapsedTimer t;
            t.start();
            QMutexLocker lock(&s_pdfiumMutex);

            int nObj = FPDFPage_CountObjects(page);
            for (int oi = 0; oi < nObj; ++oi) {
                FPDF_PAGEOBJECT obj = FPDFPage_GetObject(page, oi);
                if (!obj || FPDFPageObj_GetType(obj) != FPDF_PAGEOBJ_PATH) continue;
                ++pathCount;

                FS_MATRIX mat{};
                FPDFPageObj_GetMatrix(obj, &mat);

                int nSeg = FPDFPath_CountSegments(obj);
                segCount += nSeg;

                // §2.1: classify path
                int fillMode = 0;
                FPDF_BOOL strokeBool = 0;
                FPDFPath_GetDrawMode(obj, &fillMode, &strokeBool);
                bool hasFill = (fillMode != FPDF_FILLMODE_NONE);
                float sw = 0;
                FPDFPageObj_GetStrokeWidth(obj, &sw);
                bool hasStroke = (strokeBool != 0) && sw > 0;
                unsigned int sr2 = 0, sg2 = 0, sb2 = 0, sa2 = 0;
                bool strokeOk = FPDFPageObj_GetStrokeColor(obj, &sr2, &sg2, &sb2, &sa2);
                if (!strokeOk) hasStroke = false;
                float sr = sr2 / 255.0f, sg = sg2 / 255.0f, sb = sb2 / 255.0f, sa = sa2 / 255.0f;
                unsigned int fr2 = 0, fg2 = 0, fb2 = 0, fa2 = 0;
                bool fillOk = FPDFPageObj_GetFillColor(obj, &fr2, &fg2, &fb2, &fa2);
                float fr = fr2 / 255.0f, fg = fg2 / 255.0f, fb = fb2 / 255.0f, fa = fa2 / 255.0f;

                if (hasFill) ++fillCount;
                if (hasStroke) {
                    ++strokeCount;
                    allStrokeWidths.append(sw);
                    if (strokeOk) {
                        uint32_t sc = (qMin(255, qMax(0, (int)(sr*255)))) |
                                      (qMin(255, qMax(0, (int)(sg*255))) << 8) |
                                      (qMin(255, qMax(0, (int)(sb*255))) << 16);
                        uniqueStrokeColors.insert(sc);
                    }
                }
                if (hasFill && hasStroke) ++bothCount;
                if (hasFill && fillOk) {
                    uint32_t fc = (qMin(255, qMax(0, (int)(fr*255)))) |
                                 (qMin(255, qMax(0, (int)(fg*255))) << 8) |
                                 (qMin(255, qMax(0, (int)(fb*255))) << 16);
                    uniqueFillColors.insert(fc);
                }

                // stroke color → per-vertex color
                uint8_t vr = 0, vg = 0, vb = 0, va = 255;
                if (hasStroke && strokeOk) {
                    vr = qMin(255, qMax(0, (int)(sr*255)));
                    vg = qMin(255, qMax(0, (int)(sg*255)));
                    vb = qMin(255, qMax(0, (int)(sb*255)));
                    va = qMin(255, qMax(0, (int)(sa*255)));
                }

                // width bin (0-4 for ≤0.5, ≤1, ≤2, ≤4, >4)
                int wBin = 4;
                if (hasStroke) {
                    for (int b = 0; b < 4; ++b)
                        if (sw <= widthLimits[b]) { wBin = b; break; }
                }

                float curX = 0, curY = 0;
                bool hasCur = false;
                float subX = 0, subY = 0;

                // fill-only: collect subpath points for triangle fan (§2.4)
                QVector<QPointF> fillPts;

                for (int si = 0; si < nSeg; ++si) {
                    FPDF_PATHSEGMENT seg = FPDFPath_GetPathSegment(obj, si);
                    if (!seg) continue;
                    int segType = FPDFPathSegment_GetType(seg);
                    float sx = 0, sy = 0;
                    FPDFPathSegment_GetPoint(seg, &sx, &sy);

                    float mx = mat.a * sx + mat.c * sy + mat.e;
                    float my = mat.b * sx + mat.d * sy + mat.f;

                    switch (segType) {
                    case FPDF_SEGMENT_MOVETO:
                        curX = mx; curY = my; hasCur = true;
                        subX = mx; subY = my;
                        ++subpathCount;
                        if (hasFill && !hasStroke) fillPts.clear();
                        break;
                    case FPDF_SEGMENT_LINETO:
                        if (hasCur) {
                            vertices.append({curX, curY, vr, vg, vb, va});
                            vertices.append({mx, my, vr, vg, vb, va});
                            if (hasStroke) widthBins[wBin].append({curX, curY, vr, vg, vb, va});
                            if (hasStroke) widthBins[wBin].append({mx, my, vr, vg, vb, va});
                            ++lineSegCount;
                        }
                        if (hasFill && !hasStroke) fillPts.append(QPointF(mx, my));
                        curX = mx; curY = my;
                        break;
                    case FPDF_SEGMENT_BEZIERTO: {
                        float cx1 = mx, cy1 = my;
                        float cx2 = 0, cy2 = 0;
                        float ex = 0, ey = 0;
                        if (si + 1 < nSeg) {
                            FPDF_PATHSEGMENT s2 = FPDFPath_GetPathSegment(obj, si + 1);
                            if (s2) {
                                float px = 0, py = 0;
                                FPDFPathSegment_GetPoint(s2, &px, &py);
                                cx2 = mat.a * px + mat.c * py + mat.e;
                                cy2 = mat.b * px + mat.d * py + mat.f;
                            }
                            ++si;
                        }
                        if (si + 1 < nSeg) {
                            FPDF_PATHSEGMENT s3 = FPDFPath_GetPathSegment(obj, si + 1);
                            if (s3) {
                                float px = 0, py = 0;
                                FPDFPathSegment_GetPoint(s3, &px, &py);
                                ex = mat.a * px + mat.c * py + mat.e;
                                ey = mat.b * px + mat.d * py + mat.f;
                            }
                            ++si;
                        }
                        if (hasCur) {
                            flattenCubic(curX, curY, cx1, cy1, cx2, cy2, ex, ey, vr, vg, vb, va);
                            if (hasStroke) {
                                float prevX2 = curX;
                                const int N2 = 4;
                                for (int i2 = 1; i2 <= N2; ++i2) {
                                    float t2 = float(i2)/N2; float u2 = 1-t2;
                                    float u2b = u2*u2, u3b = u2b*u2;
                                    float t2b = t2*t2, t3b = t2b*t2;
                                    float px2 = u3b*curX + 3*u2b*t2*cx1 + 3*u2*t2b*cx2 + t3b*ex;
                                    float py2 = u3b*curY + 3*u2b*t2*cy1 + 3*u2*t2b*cy2 + t3b*ey;
                                    widthBins[wBin].append({prevX2, py2, vr, vg, vb, va});
                                    widthBins[wBin].append({px2, py2, vr, vg, vb, va});
                                    prevX2 = px2;
                                }
                            }
                            if (hasFill && !hasStroke) {
                                const int N2 = 4;
                                for (int i2 = 1; i2 <= N2; ++i2) {
                                    float t2 = float(i2)/N2; float u2 = 1-t2;
                                    float u2b = u2*u2, u3b = u2b*u2;
                                    float t2b = t2*t2, t3b = t2b*t2;
                                    fillPts.append(QPointF(
                                        u3b*curX + 3*u2b*t2*cx1 + 3*u2*t2b*cx2 + t3b*ex,
                                        u3b*curY + 3*u2b*t2*cy1 + 3*u2*t2b*cy2 + t3b*ey));
                                }
                            }
                        }
                        curX = ex; curY = ey;
                        break;
                    }
                    }
                    if (hasCur && FPDFPathSegment_GetClose(seg)) {
                        vertices.append({curX, curY, vr, vg, vb, va});
                        vertices.append({subX, subY, vr, vg, vb, va});
                        if (hasStroke) {
                            widthBins[wBin].append({curX, curY, vr, vg, vb, va});
                            widthBins[wBin].append({subX, subY, vr, vg, vb, va});
                        }
                        ++lineSegCount;
                        hasCur = false;

                        // §2.4: fill-only path → triangle fan from first point
                        if (hasFill && !hasStroke && fillPts.size() >= 3) {
                            int startIdx = fillVerts.size();
                            fillSubPathStarts.append(startIdx);
                            for (const auto& fp : fillPts) {
                                float fnx = (fp.x() / pageW) * 2.0f - 1.0f;
                                float fny = (fp.y() / pageH) * 2.0f - 1.0f;
                                uint8_t fcr = qMin(255, qMax(0, (int)(fr*255)));
                                uint8_t fcg = qMin(255, qMax(0, (int)(fg*255)));
                                uint8_t fcb = qMin(255, qMax(0, (int)(fb*255)));
                                uint8_t fca = qMin(255, qMax(0, (int)(fa*255)));
                                fillVerts.append({fnx, fny, fcr, fcg, fcb, fca});
                            }
                        }
                        fillPts.clear();
                    }
                }
            }
            traverseMs = t.elapsed();
        }

        // §2.1: compute fill path stats for §2.4 decision
        int fillOnlyCount = fillCount - bothCount;
        int fillVertsCount = fillVerts.size();
        int fillVertTris = qMax(0, fillVertsCount - fillSubPathStarts.size() * 2); // GL_TRIANGLE_FAN: N-2 tris per subpath
        int uniqueSW = 0;
        { QSet<float> swSet; for (float w : allStrokeWidths) swSet.insert(w); uniqueSW = swSet.size(); }
        out << "\n=== §2.1 Statistics ===\n";
        out << "Fill paths      : " << fillCount << "\n";
        out << "Stroke paths    : " << strokeCount << "\n";
        out << "Both            : " << bothCount << "\n";
        out << "Fill-only       : " << fillOnlyCount << "\n";
        out << "Fill verts (tri): " << fillVertsCount << " (" << fillVertTris << " triangles)\n";
        out << "Mau net khac nhau  : " << uniqueStrokeColors.size() << "\n";
        out << "Mau to khac nhau   : " << uniqueFillColors.size() << "\n";
        out << "Do day net khac nhau: " << uniqueSW << "\n";
        {
            QMap<float,int> swHist;
            for (float w : allStrokeWidths) swHist[w]++;
            QList<QPair<float,int>> swList;
            for (auto it = swHist.constBegin(); it != swHist.constEnd(); ++it)
                swList.append(qMakePair(it.key(), it.value()));
            std::sort(swList.begin(), swList.end(), [](const QPair<float,int>& a, const QPair<float,int>& b){ return a.second > b.second; });
            int show = qMin(10, swList.size());
            for (int i = 0; i < show; ++i)
                out << "  " << QString::number(swList[i].first,'f',2) << " pt: " << swList[i].second << " paths\n";
        }
        out << "=== End §2.1 ===\n\n";

        bool doStencilFill = (fillOnlyCount <= 100000 && fillVertsCount > 0);
        if (!doStencilFill && fillVertsCount > 0) {
            out << "§2.4 SKIP: fill paths " << fillOnlyCount << " > 100,000 — stencil-then-cover not feasible\n";
            out << "  (vertices generated: " << fillVertsCount << ", triangles: " << fillVertTris << ")\n\n";
        }

        // Convert stroke vertices to NDC (§2.2 + §2.3: per-vertex color already in Vtx)
        for (int b = 0; b < 5; ++b) {
            for (auto& v : widthBins[b]) {
                v.x = (v.x / pageW) * 2.0f - 1.0f;
                v.y = (v.y / pageH) * 2.0f - 1.0f;
                if (v.x < -1.5f || v.x > 1.5f || v.y < -1.5f || v.y > 1.5f)
                    ++outOfRangeCount;
            }
        }
        // Also convert the legacy `vertices` array for total stats
        float totalVBOBytes = 0;
        for (int b = 0; b < 5; ++b) totalVBOBytes += widthBins[b].size() * sizeof(Vtx);
        float vboMB = totalVBOBytes / (1024.0f * 1024.0f);
        int totalVerts = 0;
        for (int b = 0; b < 5; ++b) totalVerts += widthBins[b].size();
        out << "Trich path    : " << traverseMs << " ms  (paths=" << pathCount
            << " segs=" << segCount << " subpaths=" << subpathCount
            << " lines=" << lineSegCount
            << " verts=" << totalVerts << " VBO=" << QString::number(vboMB,'f',3) << " MB"
            << " oor=" << outOfRangeCount << ")\n";

        // ── 3. OpenGL offscreen render ──
        qint64 uploadMs = 0, renderMs = 0, fillRenderMs = 0;
        bool glOk = false;
        QString glError;
        {
            QSurfaceFormat fmt;
            fmt.setVersion(3, 3);
            fmt.setProfile(QSurfaceFormat::CoreProfile);
            fmt.setDepthBufferSize(24);
            fmt.setStencilBufferSize(8);
            fmt.setSamples(0);

            QOffscreenSurface surf;
            surf.setFormat(fmt);
            surf.create();
            if (!surf.isValid()) {
                glError = QStringLiteral("QOffscreenSurface not valid");
            } else {
                QOpenGLContext ctx;
                ctx.setFormat(fmt);
                if (!ctx.create()) {
                    fmt.setProfile(QSurfaceFormat::CompatibilityProfile);
                    surf.setFormat(fmt);
                    surf.create();
                    ctx.setFormat(fmt);
                    if (!ctx.create()) {
                        glError = QStringLiteral("QOpenGLContext create failed (Core+Compat)");
                    }
                }
                if (glError.isEmpty() && !ctx.makeCurrent(&surf)) {
                    glError = QStringLiteral("makeCurrent failed");
                }
                if (glError.isEmpty()) {
                    QOpenGLFunctions* gl = ctx.functions();

                    // Print GL info for driver debugging
                    out << "GL_VERSION  : " << (const char*)gl->glGetString(GL_VERSION) << "\n";
                    out << "GL_RENDERER : " << (const char*)gl->glGetString(GL_RENDERER) << "\n";
                    out << "GL_VENDOR   : " << (const char*)gl->glGetString(GL_VENDOR) << "\n";

                    // VAO mandatory in Core Profile — Mesa tolerates VAO=0, NVIDIA/AMD/Intel reject draw calls without it
                    QOpenGLExtraFunctions* glx = ctx.extraFunctions();
                    if (glx) {
                        GLuint vao = 0;
                        glx->glGenVertexArrays(1, &vao);
                        glx->glBindVertexArray(vao);
                    } else {
                        glError = QStringLiteral("QOpenGLExtraFunctions unavailable (need GL 3.0+)");
                    }
                }
                if (glError.isEmpty()) {
                    QOpenGLFramebufferObject fbo(wPx, hPx,
                        QOpenGLFramebufferObject::CombinedDepthStencil);
                    fbo.bind();
                    out << "fbo.isValid() : " << fbo.isValid() << "\n";

                    QOpenGLFunctions* gl = ctx.functions();
                    gl->glViewport(0, 0, wPx, hPx);
                    gl->glClearColor(1.0f, 1.0f, 1.0f, 1.0f);
                    gl->glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
                    gl->glEnable(GL_LINE_SMOOTH);
                    gl->glHint(GL_LINE_SMOOTH_HINT, GL_NICEST);

                    // §2.5: measure GL_ALIASED_LINE_WIDTH_RANGE
                    float lwRange[2] = {1.0f, 1.0f};
                    gl->glGetFloatv(GL_ALIASED_LINE_WIDTH_RANGE, lwRange);
                    out << "GL_ALIASED_LINE_WIDTH_RANGE: " << lwRange[0] << " - " << lwRange[1] << "\n";
                    { GLenum err = gl->glGetError(); if (err != GL_NO_ERROR)
                        out << "GL_ERROR after init: 0x" << QString::number(err,16) << "\n"; }

                    // §2.4: stencil-then-cover for fill paths
                    qint64 fillStart = 0;
                    if (doStencilFill) {
                        fillStart = tTotal.elapsed();

                        // Shader for fill (same as stroke, just uses color from VBO)
                        const char* fillVsrc =
                            "#version 330 core\n"
                            "layout(location=0) in vec2 aPos;\n"
                            "layout(location=1) in vec4 aColor;\n"
                            "out vec4 vColor;\n"
                            "void main() { gl_Position = vec4(aPos, 0.0, 1.0); vColor = aColor; }\n";
                        const char* fillFsrc =
                            "#version 330 core\n"
                            "in vec4 vColor;\n"
                            "out vec4 FragColor;\n"
                            "void main() { FragColor = vColor; }\n";

                        QOpenGLShaderProgram fillProg;
                        if (fillProg.addShaderFromSourceCode(QOpenGLShader::Vertex, fillVsrc) &&
                            fillProg.addShaderFromSourceCode(QOpenGLShader::Fragment, fillFsrc) &&
                            fillProg.link()) {
                            fillProg.bind();
                            { GLenum err = gl->glGetError(); if (err != GL_NO_ERROR)
                                out << "GL_ERROR after fill shader: 0x" << QString::number(err,16) << "\n"; }

                            QOpenGLBuffer fillVBO(QOpenGLBuffer::VertexBuffer);
                            fillVBO.create();
                            fillVBO.bind();
                            fillVBO.allocate(fillVerts.constData(), fillVerts.size() * sizeof(Vtx));
                            fillProg.enableAttributeArray(0);
                            fillProg.setAttributeBuffer(0, GL_FLOAT, 0, 2, sizeof(Vtx));
                            fillProg.enableAttributeArray(1);
                            fillProg.setAttributeBuffer(1, GL_UNSIGNED_BYTE, 2 * sizeof(float), 4, sizeof(Vtx));

                            // Stencil setup for even-odd fill
                            gl->glEnable(GL_STENCIL_TEST);
                            gl->glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
                            gl->glStencilOp(GL_INVERT, GL_INVERT, GL_INVERT);
                            gl->glStencilFunc(GL_ALWAYS, 0, 0xFF);
                            gl->glStencilMask(0xFF);

                            // Draw triangle fans: each subpath = 1 GL_TRIANGLE_FAN
                            for (int si = 0; si < fillSubPathStarts.size(); ++si) {
                                int start = fillSubPathStarts[si];
                                int count = (si + 1 < fillSubPathStarts.size())
                                    ? fillSubPathStarts[si + 1] - start
                                    : fillVerts.size() - start;
                                if (count >= 3)
                                    gl->glDrawArrays(GL_TRIANGLE_FAN, start, count);
                            }

                            // Cover: draw bounding box with fill color where stencil != 0
                            gl->glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
                            gl->glStencilFunc(GL_NOTEQUAL, 0, 0xFF);
                            gl->glStencilMask(0x00);

                            // Use average fill color for cover (since we batched all fills)
                            float ar = 0, ag = 0, ab = 0;
                            if (fillVertTris > 0) {
                                for (const auto& v : fillVerts) { ar += v.r; ag += v.g; ab += v.b; }
                                ar /= (fillVerts.size() * 255.0f);
                                ag /= (fillVerts.size() * 255.0f);
                                ab /= (fillVerts.size() * 255.0f);
                            }
                            fillProg.setUniformValue("uColor", 0, 0, 0, 0); // unused when per-vertex

                            // Draw full-screen quad (stencil test clips to filled regions)
                            struct FillVtx2 { float x, y; uint8_t r,g,b,a; };
                            FillVtx2 cover[] = {{-1,-1,0,0,0,255},{1,-1,0,0,0,255},{-1,1,0,0,0,255},
                                                {1,-1,0,0,0,255},{1,1,0,0,0,255},{-1,1,0,0,0,255}};
                            // Use red for visibility: stencil-covered areas show fill
                            for (auto& cv : cover) { cv.r = (uint8_t)(ar*255); cv.g = (uint8_t)(ag*255); cv.b = (uint8_t)(ab*255); }
                            fillVBO.bind();
                            fillVBO.allocate(cover, sizeof(cover));
                            fillProg.setAttributeBuffer(0, GL_FLOAT, 0, 2, sizeof(FillVtx2));
                            fillProg.setAttributeBuffer(1, GL_UNSIGNED_BYTE, 2*sizeof(float), 4, sizeof(FillVtx2));
                            gl->glDrawArrays(GL_TRIANGLES, 0, 6);

                            gl->glDisable(GL_STENCIL_TEST);
                            gl->glStencilMask(0xFF);
                            gl->glClear(GL_STENCIL_BUFFER_BIT);

                            fillVBO.destroy();
                            fillProg.release();
                        }
                        fillRenderMs = tTotal.elapsed() - fillStart;
                    }

                    // §2.2 + §2.3: stroke shader with per-vertex color
                    const char* vsrc =
                        "#version 330 core\n"
                        "layout(location=0) in vec2 aPos;\n"
                        "layout(location=1) in vec4 aColor;\n"
                        "out vec4 vColor;\n"
                        "void main() { gl_Position = vec4(aPos, 0.0, 1.0); vColor = aColor; }\n";
                    const char* fsrc =
                        "#version 330 core\n"
                        "in vec4 vColor;\n"
                        "out vec4 FragColor;\n"
                        "void main() { FragColor = vColor; }\n";

                    QOpenGLShaderProgram prog;
                    if (!prog.addShaderFromSourceCode(QOpenGLShader::Vertex, vsrc) ||
                        !prog.addShaderFromSourceCode(QOpenGLShader::Fragment, fsrc) ||
                        !prog.link()) {
                        glError = QStringLiteral("shader link: ") + prog.log();
                    } else {
                        prog.bind();
                        { GLenum err = gl->glGetError(); if (err != GL_NO_ERROR)
                            out << "GL_ERROR after stroke shader: 0x" << QString::number(err,16) << "\n"; }

                        // §2.3: draw each width bin with its own glLineWidth
                        const float binWidths[] = {0.5f, 1.0f, 2.0f, 4.0f, 1.0f};
                        {
                            QElapsedTimer t;
                            t.start();

                            for (int b = 0; b < 5; ++b) {
                                if (widthBins[b].isEmpty()) continue;
                                float lw = binWidths[b];
                                if (lw < lwRange[0]) lw = lwRange[0];
                                if (lw > lwRange[1]) lw = lwRange[1];
                                gl->glLineWidth(lw);

                                QOpenGLBuffer vbo(QOpenGLBuffer::VertexBuffer);
                                vbo.create();
                                vbo.bind();
                                vbo.allocate(widthBins[b].constData(), widthBins[b].size() * sizeof(Vtx));

                                prog.enableAttributeArray(0);
                                prog.setAttributeBuffer(0, GL_FLOAT, 0, 2, sizeof(Vtx));
                                prog.enableAttributeArray(1);
                                prog.setAttributeBuffer(1, GL_UNSIGNED_BYTE, 2 * sizeof(float), 4, sizeof(Vtx));

                                if (widthBins[b].size() >= 2)
                                    gl->glDrawArrays(GL_LINES, 0, widthBins[b].size());

                                vbo.destroy();
                            }
                            gl->glFinish();
                            { GLenum err = gl->glGetError(); if (err != GL_NO_ERROR)
                                out << "GL_ERROR after render: 0x" << QString::number(err,16) << "\n"; }
                            renderMs = t.elapsed();
                        }

                        QImage img = fbo.toImage();
                        bool saved = img.save(outPng, "PNG");
                        glOk = saved;
                        if (!saved)
                            glError = QStringLiteral("QImage.save failed: ") + outPng;

                        prog.release();
                    }
                    fbo.release();
                }
            }
        }

        qint64 totalMs = tTotal.elapsed();
        out << "Upload VBO    : " << uploadMs << " ms\n";
        out << "Ve fill (stencil): " << fillRenderMs << " ms\n";
        out << "Ve GL (stroke): " << renderMs << " ms\n";
        out << "TONG          : " << totalMs << " ms\n";

        if (glOk)
            out << "VECTORGL_OK " << outPng << " (" << wPx << "x" << hPx << ")\n";
        else
            out << "VECTORGL_FAIL " << glError << "\n";
        out.flush();

        { QMutexLocker lock(&s_pdfiumMutex); FPDF_ClosePage(page); }
        PdfDocument::libRelease();
        return glOk ? 0 : 1;
    }

    // Ghost geometry helpers (defined in PdfGpuView.cpp, file scope — not static)
    QPointF trGhostBaseline(const QRectF& dispRect);
    qreal trGhostPixelSize(float fontSizePt, double zoom);

    // usage: --safedelete-test
    // Headless verify removeWorkingCopy safety gate — prevents deletion of user's original files.
    if (argc >= 2 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--safedelete-test")) {
        QTextStream out(stdout);
        extern bool removeWorkingCopy(const QString& path);
        qint64 now = QDateTime::currentMSecsSinceEpoch();
        QString tmpDir = QDir::temp().absolutePath();
        int failures = 0;
        auto CHECK = [&](const QString& label, bool ok) {
            if (!ok) { out << "FAIL " << label << "\n"; ++failures; }
        };
        // case 1: temp dir, .tortmp → true, file gone
        {
            QString p = tmpDir + "/safedel_" + QString::number(now) + ".tortmp";
            QFile f(p);
            f.open(QIODevice::WriteOnly); f.write("x"); f.close();
            bool ret = removeWorkingCopy(p);
            CHECK("case1-returned-true", ret);
            CHECK("case1-file-gone", !QFile::exists(p));
        }
        // case 2: temp dir, .pdf → false, file survives
        {
            QString p = tmpDir + "/safedel_" + QString::number(now) + ".pdf";
            QFile f(p);
            f.open(QIODevice::WriteOnly); f.write("x"); f.close();
            bool ret = removeWorkingCopy(p);
            CHECK("case2-returned-false", !ret);
            CHECK("case2-file-exists", QFile::exists(p));
            QFile::remove(p);
        }
        // case 3: cwd, .tortmp → false, file survives
        {
            QString p = "./safedel_fake_" + QString::number(now) + ".tortmp";
            QFile f(p);
            f.open(QIODevice::WriteOnly); f.write("x"); f.close();
            bool ret = removeWorkingCopy(p);
            CHECK("case3-returned-false", !ret);
            CHECK("case3-file-exists", QFile::exists(p));
            QFile::remove(p);
        }
        // case 4: empty path → false
        {
            bool ret = removeWorkingCopy("");
            CHECK("case4-empty-returned-false", !ret);
        }
        if (failures == 0)
            out << "SAFEDELETE: PASS\n";
        else
            out << "SAFEDELETE: FAIL failures=" << failures << "\n";
        out.flush();
        return failures == 0 ? 0 : 1;
    }

    // usage: --ghostgeom
    // Headless verify of ghost geometry helpers.
    if (argc >= 2 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--ghostgeom")) {
        QTextStream out(stdout);
        bool ok = true;
        auto check = [&](const QString& label, bool cond) {
            if (!cond) { out << "FAIL " << label << "\n"; ok = false; }
        };
        {
            QPointF bl = trGhostBaseline(QRectF(50, 100, 200, 20));
            check("baseline-x", qAbs(bl.x() - 54.0) < 1e-6);
            check("baseline-y", qAbs(bl.y() - 118.0) < 1e-6);
        }
        check("pixelsize-z1", qAbs(trGhostPixelSize(12.0f, 1.0) - 12.0) < 1e-6);
        check("pixelsize-z2", qAbs(trGhostPixelSize(12.0f, 2.5) - 30.0) < 1e-6);
        if (ok) out << "GHOSTGEOM: PASS\n";
        else    out << "GHOSTGEOM: FAIL\n";
        out.flush();
        return ok ? 0 : 1;
    }

    // usage: --safesave-test
    // Headless verify replaceFileAtomically — không bao giờ mất file gốc.
    if (argc >= 2 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--safesave-test")) {
        QTextStream out(stdout);
        extern bool replaceFileAtomically(const QString& srcTmp, const QString& dest, QString* errOut);
        qint64 now = QDateTime::currentMSecsSinceEpoch();
        QString workDir = QDir::temp().absolutePath() + QStringLiteral("/safesave_") + QString::number(now);
        QDir().mkpath(workDir);
        auto rd = [&](const QString& p) -> QString {
            QFile f(p); if (!f.open(QIODevice::ReadOnly)) return {};
            return QString::fromUtf8(f.readAll());
        };
        auto wr = [&](const QString& p, const QByteArray& data) {
            QFile f(p); f.open(QIODevice::WriteOnly); f.write(data); f.close();
        };
        auto clean = [&]() { QDir(workDir).removeRecursively(); };
        int failures = 0;
        auto CHECK = [&](const QString& label, bool ok) {
            if (!ok) { out << "FAIL " << label << "\n"; ++failures; }
        };
        QString err;
        // case 1: thay thế bình thường
        {
            QString src = workDir + QStringLiteral("/src1");
            QString dest = workDir + QStringLiteral("/dest1");
            wr(src, "NEWDATA"); wr(dest, "OLD");
            bool ret = replaceFileAtomically(src, dest, &err);
            CHECK("case1-returned-true", ret);
            CHECK("case1-content", rd(dest) == "NEWDATA");
            CHECK("case1-no-savetmp", !QFile::exists(dest + ".savetmp"));
            CHECK("case1-no-savebak", !QFile::exists(dest + ".savebak"));
        }
        // case 2: đích chưa tồn tại
        {
            QString src = workDir + QStringLiteral("/src2");
            QString dest = workDir + QStringLiteral("/dest2");
            wr(src, "NEWDATA");
            bool ret = replaceFileAtomically(src, dest, &err);
            CHECK("case2-returned-true", ret);
            CHECK("case2-content", rd(dest) == "NEWDATA");
        }
        // case 3: nguồn không tồn tại → dest phải y hệt
        {
            QString src = workDir + QStringLiteral("/nonexistent3");
            QString dest = workDir + QStringLiteral("/dest3");
            wr(dest, "OLD");
            bool ret = replaceFileAtomically(src, dest, &err);
            CHECK("case3-returned-false", !ret);
            CHECK("case3-content-preserved", rd(dest) == "OLD");
        }
        // case 4: nguồn rỗng (0 byte) → dest phải y hệt
        {
            QString src = workDir + QStringLiteral("/src4");
            QString dest = workDir + QStringLiteral("/dest4");
            wr(src, ""); wr(dest, "OLD");
            bool ret = replaceFileAtomically(src, dest, &err);
            CHECK("case4-returned-false", !ret);
            CHECK("case4-content-preserved", rd(dest) == "OLD");
        }
        // case 5: không sót rác
        {
            bool hasSavetmp = !QDir(workDir).entryList({"*.savetmp"}, QDir::Files).isEmpty();
            bool hasSavebak = !QDir(workDir).entryList({"*.savebak"}, QDir::Files).isEmpty();
            CHECK("case5-no-savetmp-leak", !hasSavetmp);
            CHECK("case5-no-savebak-leak", !hasSavebak);
        }
        clean();
        if (failures == 0)
            out << "SAFESAVE: PASS\n";
        else
            out << "SAFESAVE: FAIL failures=" << failures << "\n";
        out.flush();
        return failures == 0 ? 0 : 1;
    }

    // usage: --thumbbench <input.pdf> [maxPages]
    if (argc >= 3 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--thumbbench")) {
        QTextStream out(stdout);
        const QString inPath = QString::fromLocal8Bit(argv[2]);
        int maxPages = (argc >= 4) ? QString::fromLocal8Bit(argv[3]).toInt() : 60;
        PdfDocument::libAddRef();

        ThumbnailRenderPool pool;
        QElapsedTimer t; t.start();
        if (!pool.open(inPath)) {
            out << "THUMBBENCH: FAIL pool.open\n"; out.flush();
            PdfDocument::libRelease(); return 1;
        }
        const qint64 openMs = t.elapsed();
        const int total = qMin(maxPages, pool.pageCount());
        if (total <= 0) {
            out << "THUMBBENCH: FAIL pageCount=" << pool.pageCount() << "\n"; out.flush();
            pool.close(); PdfDocument::libRelease(); return 1;
        }

        int received = 0;
        qint64 firstMs = -1;
        QEventLoop loop;
        QObject::connect(&pool, &ThumbnailRenderPool::thumbnailReady, &loop,
            [&](int, QImage, quint64) {
                if (firstMs < 0) firstMs = t.elapsed();
                if (++received >= total) loop.quit();
            });
        QTimer guard;
        guard.setSingleShot(true);
        QObject::connect(&guard, &QTimer::timeout, &loop, &QEventLoop::quit);
        guard.start(120000);

        t.restart();
        for (int i = 0; i < total; ++i) pool.requestThumbnail(i, 2);
        loop.exec();
        const qint64 wallMs = t.elapsed();
        pool.close();
        PdfDocument::libRelease();

        out << "THUMBBENCH pages=" << total << " open_ms=" << openMs
            << " first_ms=" << firstMs << " wall_ms=" << wallMs
            << " received=" << received
            << " per_page_ms=" << QString::number(received ? double(wallMs)/received : 0.0, 'f', 1)
            << (received < total ? "  [THIEU - het gio]" : "")
            << "\n";
        out.flush();
        return received == total ? 0 : 1;
    }

    // usage: --thumbepoch-test <pdfA> <pdfB>
    // Deterministic harness: verify old-thumbnail epoch gating works without
    // race conditions. Requires two PDFs with different page counts.
    if (argc >= 4 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--thumbepoch-test")) {
        QTextStream out(stdout);
        QString pdfA = QString::fromLocal8Bit(argv[2]);
        QString pdfB = QString::fromLocal8Bit(argv[3]);

        // Step 1: open both documents
        PdfDocument docA, docB;
        if (!docA.open(pdfA)) { out << "THUMBEPOCH: FAIL cannot open " << pdfA << "\n"; out.flush(); return 1; }
        if (!docB.open(pdfB)) { out << "THUMBEPOCH: FAIL cannot open " << pdfB << "\n"; out.flush(); return 1; }
        if (docA.pageCount() == docB.pageCount()) {
            out << "THUMBEPOCH: FAIL hai file cung so trang (" << docA.pageCount() << ")\n";
            out.flush(); return 1;
        }

        // Step 2: open pool on pdfA, record epochA
        ThumbnailRenderPool poolA;
        if (!poolA.open(pdfA)) { out << "THUMBEPOCH: FAIL poolA.open\n"; out.flush(); return 1; }
        quint64 epochA = poolA.epoch();

        // Step 3: set up panel with docA
        PdfRenderer rendererA;
        rendererA.setDocument(&docA);
        ThumbnailPanel panel;
        panel.setDocument(&docA, &rendererA, &poolA, true);

        // Step 4: switch pool to pdfB, verify epoch advances
        poolA.close();
        if (!poolA.open(pdfB)) { out << "THUMBEPOCH: FAIL poolA.open(pdfB)\n"; out.flush(); return 1; }
        quint64 epochB = poolA.epoch();
        if (epochB == epochA) {
            out << "THUMBEPOCH: FAIL epoch khong tang sau open()\n";
            out.flush(); return 1;
        }

        // Step 5: point panel to docB
        PdfRenderer rendererB;
        rendererB.setDocument(&docB);
        panel.setDocument(&docB, &rendererB, &poolA, true);
        panel.debugResetCounters();

        // Step 6: a fake image sized according to actual page width
        const double pw0 = docB.pageSize(0).width();
        const int imgW = qMax(8, static_cast<int>(pw0 * 0.2));
        const int imgH = qMax(8, static_cast<int>(imgW * 1.4));
        QImage img(imgW, imgH, QImage::Format_RGB32);
        img.fill(Qt::white);

        // Step 7: Case A — old epoch image arrives late, must be dropped
        panel.onPageReady(0, img, epochA);
        if (panel.debugDroppedCount() != 1 || panel.debugAcceptedCount() != 0
            || panel.debugPendingCount() != 0 || panel.debugRejectedCount() != 0) {
            out << "THUMBEPOCH: FAIL anh cu VAN LOT (dropped="
                << panel.debugDroppedCount() << " accepted="
                << panel.debugAcceptedCount() << " pending="
                << panel.debugPendingCount() << " rejected="
                << panel.debugRejectedCount() << ")\n";
            out.flush(); return 1;
        }

        // Step 8: Case B — current epoch image, must be accepted or pending
        panel.debugResetCounters();
        panel.onPageReady(0, img, epochB);
        {
            const int acc = panel.debugAcceptedCount();
            const int drp = panel.debugDroppedCount();
            const int pen = panel.debugPendingCount();
            const int rej = panel.debugRejectedCount();
            if (drp != 0 || rej != 0 || (acc + pen) != 1) {
                out << "THUMBEPOCH: FAIL anh moi bi loai nham (accepted=" << acc
                    << " dropped=" << drp << " pending=" << pen
                    << " rejected=" << rej << ")\n";
                out.flush(); return 1;
            }
        }

        // Step 9: Case C — verify early-return fires when same doc is reloaded
        // (forceRebuild=false + same pointers → must NOT rebuild list).
        panel.setDocument(&docA, &rendererA, &poolA, true);
        poolA.close();
        if (!poolA.open(pdfA)) { out << "THUMBEPOCH: FAIL poolA.open(pdfA)\n"; out.flush(); return 1; }
        quint64 epochC = poolA.epoch();
        panel.debugResetCounters();
        panel.setDocument(&docA, &rendererA, &poolA, false);
        if (panel.debugEarlyReturnCount() != 1) {
            out << "THUMBEPOCH: FAIL ca C khong cham duoc nhanh thoat som (early="
                << panel.debugEarlyReturnCount() << ")\n";
            out.flush(); return 1;
        }
        {
            const double pwA = docA.pageSize(0).width();
            const int imgWC = qMax(8, static_cast<int>(pwA * 0.2));
            const int imgHC = qMax(8, static_cast<int>(imgWC * 1.4));
            QImage imgC(imgWC, imgHC, QImage::Format_RGB32);
            imgC.fill(Qt::white);
            panel.debugResetCounters();
            panel.onPageReady(0, imgC, epochC);
            const int acc = panel.debugAcceptedCount();
            const int drp = panel.debugDroppedCount();
            const int pen = panel.debugPendingCount();
            const int rej = panel.debugRejectedCount();
            if (drp != 0 || rej != 0 || (acc + pen) != 1) {
                out << "THUMBEPOCH: FAIL ca C (forceRebuild=false) accepted=" << acc
                    << " dropped=" << drp << " pending=" << pen
                    << " rejected=" << rej << " epochC=" << epochC << "\n";
                out.flush(); return 1;
            }
        }

        // Step 10: Case D — reload like Insert (forceRebuild=true), must also succeed
        quint64 epochD = 0;
        {
            ThumbnailRenderPool poolD;
            if (!poolD.open(pdfA)) { out << "THUMBEPOCH: FAIL poolD.open(pdfA)\n"; out.flush(); return 1; }
            epochD = poolD.epoch();
            const double pwA = docA.pageSize(0).width();
            const int imgWD = qMax(8, static_cast<int>(pwA * 0.2));
            const int imgHD = qMax(8, static_cast<int>(imgWD * 1.4));
            QImage imgD(imgWD, imgHD, QImage::Format_RGB32);
            imgD.fill(Qt::white);
            panel.setDocument(&docA, &rendererA, &poolD, true);
            panel.debugResetCounters();
            panel.onPageReady(0, imgD, epochD);
            const int acc = panel.debugAcceptedCount();
            const int drp = panel.debugDroppedCount();
            const int pen = panel.debugPendingCount();
            const int rej = panel.debugRejectedCount();
            if (drp != 0 || rej != 0 || (acc + pen) != 1) {
                out << "THUMBEPOCH: FAIL ca D (forceRebuild=true) accepted=" << acc
                    << " dropped=" << drp << " pending=" << pen
                    << " rejected=" << rej << " epochD=" << epochD << "\n";
                out.flush(); return 1;
            }
        }

        out << "THUMBEPOCH: PASS epochA=" << epochA << " epochB=" << epochB
            << " epochC=" << epochC << " epochD=" << epochD << "\n";
        out.flush();
        return 0;
    }

    // usage: --thumbreload-test <pdfA> <pdfB>
    // Real signal-path harness: pool -> worker -> panel through 3 phases (mo, insert, save).
    // Requires two PDFs with different page counts.
    if (argc >= 4 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--thumbreload-test")) {
        QTextStream out(stdout);
        QString pdfA = QString::fromLocal8Bit(argv[2]);
        QString pdfB = QString::fromLocal8Bit(argv[3]);

        PdfDocument docA, docB;
        if (!docA.open(pdfA)) { out << "THUMBRELOAD: FAIL cannot open " << pdfA << "\n"; out.flush(); return 1; }
        if (!docB.open(pdfB)) { out << "THUMBRELOAD: FAIL cannot open " << pdfB << "\n"; out.flush(); return 1; }
        if (docA.pageCount() == docB.pageCount()) {
            out << "THUMBRELOAD: FAIL hai file cung so trang (" << docA.pageCount() << ")\n";
            out.flush(); return 1;
        }

        PdfRenderer rendererA, rendererB;
        rendererA.setDocument(&docA);
        rendererB.setDocument(&docB);

        ThumbnailRenderPool pool;
        ThumbnailPanel panel;

        auto runPhase = [&](const char* label, ThumbnailRenderPool& p,
                            ThumbnailPanel& pnl, int n) -> bool {
            pnl.debugResetCounters();
            QEventLoop loop;
            int got = 0;
            auto conn = QObject::connect(&p, &ThumbnailRenderPool::thumbnailReady, &loop,
                [&](int, QImage, quint64) { if (++got >= n) loop.quit(); });
            QTimer guard; guard.setSingleShot(true);
            QObject::connect(&guard, &QTimer::timeout, &loop, &QEventLoop::quit);
            guard.start(30000);
            for (int i = 0; i < n; ++i) p.requestThumbnail(i, 2);
            loop.exec();
            QObject::disconnect(conn);
            loop.processEvents();
            const int acc = pnl.debugAcceptedCount();
            const int pen = pnl.debugPendingCount();
            const int drp = pnl.debugDroppedCount();
            const int rej = pnl.debugRejectedCount();
            out << "THUMBRELOAD [" << label << "] emitted=" << got
                << " accepted=" << acc << " pending=" << pen
                << " dropped=" << drp << " rejected=" << rej << "\n";
            out.flush();
            if (drp > 0) { out << "THUMBRELOAD: FAIL [" << label << "] co anh bi cong epoch loai\n"; return false; }
            if (acc + pen == 0) { out << "THUMBRELOAD: FAIL [" << label << "] panel khong nhan duoc anh nao\n"; return false; }
            return true;
        };

        if (!pool.open(pdfA)) { out << "THUMBRELOAD: FAIL pool.open(pdfA)\n"; out.flush(); return 1; }
        panel.setDocument(&docA, &rendererA, &pool, true);
        QCoreApplication::processEvents();
        if (!runPhase("mo", pool, panel, 5)) return 1;

        pool.close();
        docA.close();
        docB.open(pdfB);
        if (!pool.open(pdfB)) { out << "THUMBRELOAD: FAIL pool.open(pdfB) insert\n"; out.flush(); return 1; }
        panel.setDocument(&docB, &rendererB, &pool, true);
        QCoreApplication::processEvents();
        if (!runPhase("insert", pool, panel, 5)) return 1;

        pool.close();
        if (!pool.open(pdfB)) { out << "THUMBRELOAD: FAIL pool.open(pdfB) save\n"; out.flush(); return 1; }
        panel.setDocument(&docB, &rendererB, &pool, false);
        QCoreApplication::processEvents();
        if (!runPhase("save", pool, panel, 5)) return 1;

        out << "THUMBRELOAD: PASS\n";
        out.flush();
        return 0;
    }

    // usage: --guiprobe <input.pdf> <out.png>
    // Probe: kiem tra offscreen OpenGL (PdfGpuView) co render duoc trong Docker khong
    if (argc >= 4 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--guiprobe")) {
        QString inputPath = QString::fromLocal8Bit(argv[2]);
        QString outPng = QString::fromLocal8Bit(argv[3]);

        MainWindow w;
        w.resize(1400, 900);
        w.show();
        QCoreApplication::processEvents();

        w.openFile(inputPath);

        for (int i = 0; i < 60; ++i) {
            QCoreApplication::processEvents();
            QThread::msleep(50);
        }

        QPixmap pm = w.grab();
        QImage img = pm.toImage();
        if (img.isNull()) {
            fprintf(stderr, "GUIPROBE: FAIL grab returned null\n");
            return 1;
        }
        if (!img.save(outPng, "PNG")) {
            fprintf(stderr, "GUIPROBE: FAIL cannot save %s\n", outPng.toLocal8Bit().constData());
            return 1;
        }

        int total = img.width() * img.height();
        int nonBlack = 0;
        for (int y = 0; y < img.height(); ++y) {
            const QRgb* row = reinterpret_cast<const QRgb*>(img.constScanLine(y));
            for (int x = 0; x < img.width(); ++x) {
                QRgb px = row[x];
                if (qAlpha(px) > 0 && (qRed(px) > 10 || qGreen(px) > 10 || qBlue(px) > 10))
                    ++nonBlack;
            }
        }
        double pct = (total > 0) ? (100.0 * nonBlack / total) : 0.0;
        fprintf(stdout, "GUIPROBE size=%dx%d nonblack=%.2f%%\n", img.width(), img.height(), pct);
        if (pct < 5.0) {
            fprintf(stderr, "GUIPROBE: FAIL anh gan nhu den, offscreen GL khong render duoc\n");
            return 1;
        }
        fprintf(stdout, "GUIPROBE: PASS\n");
        return 0;
    }

    // usage: --uiprobe <input.pdf> <outdir> [waitMs=8000] [shotTab=0..5]
    // Probe giao dien (SPEC_PROBE_LOG_SNAPSHOT muc 4): mo cua so that (ke ca
    // theme), nap pdf, cho waitMs roi lam DUNG viec cua muc 3 nhung ghi ra
    // <outdir>/uiprobe.png + <outdir>/uiprobe.txt. Dung lai khuon --guiprobe.
    if (argc >= 4 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--uiprobe")) {
        const QString inputPath = QString::fromLocal8Bit(argv[2]);
        const QString outDir    = QString::fromLocal8Bit(argv[3]);
        int waitMs = (argc >= 5) ? QString::fromLocal8Bit(argv[4]).toInt() : 8000;
        if (waitMs <= 0) waitMs = 8000;   // mac dinh 8000, khong phai 3000
        int shotTab = -1;
        if (argc >= 6) {
            const int val = QString::fromLocal8Bit(argv[5]).toInt();
            if (val >= 0 && val <= 5)
                shotTab = val;
        }
        if (!QDir().mkpath(outDir)) {
            fprintf(stderr, "UIPROBE_FAIL khong tao duoc %s\n", outDir.toLocal8Bit().constData());
            return 1;
        }

        MainWindow w;
        w.resize(1400, 900);
        w.show();
        QCoreApplication::processEvents();

        w.openFile(inputPath);

        // Cho du waitMs: file CAD lon render tien-dan, chup som ra vung xem
        // TRANG TRON (bai hoc 08-14).
        const int loops = qMax(waitMs / 50, 1);
        for (int i = 0; i < loops; ++i) {
            QCoreApplication::processEvents();
            QThread::msleep(50);
        }

        // Khong goi probeSelectSidebarTab rieng — shotTab duoc truyen vao probeSnapshot
        // de sau khi dump OcrPanel xong, UI chuyen sang tab mong muon roi moi chup.

        const QString outPng = outDir + QLatin1String("/uiprobe.png");
        const QString outTxt = outDir + QLatin1String("/uiprobe.txt");
        QString err;
        if (!w.probeSnapshot(outPng, outTxt, &err, shotTab)) {
            fprintf(stderr, "UIPROBE_FAIL %s\n", err.toLocal8Bit().constData());
            return 1;
        }
        fprintf(stdout, "[uiprobe] shotTab=%d\n", shotTab);
        fprintf(stdout, "UIPROBE_OK %s\n", outPng.toLocal8Bit().constData());
        return 0;
    }

    // usage: --uiprobe-dialog <input.pdf> <outdir> <merge|about|sign|print> [waitMs=8000]
    // Probe hop thoai (SPEC_PROBE_DIALOG_FRAMES phan 1): mo DUNG hop thoai bang
    // cach kich hoat QAction tren toolbar roi chup CHINH hop thoai (cua so rieng)
    // ra <outdir>/dialog_<ten>.png + .txt. Noi dung chup + dong nam trong probeDialog.
    if (argc >= 5 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--uiprobe-dialog")) {
        const QString inputPath = QString::fromLocal8Bit(argv[2]);
        const QString outDir    = QString::fromLocal8Bit(argv[3]);
        const QString dlgName   = QString::fromLocal8Bit(argv[4]).toLower();
        int waitMs = (argc >= 6) ? QString::fromLocal8Bit(argv[5]).toInt() : 8000;
        if (waitMs <= 0) waitMs = 8000;
        if (dlgName != QLatin1String("merge") && dlgName != QLatin1String("about")
                && dlgName != QLatin1String("sign") && dlgName != QLatin1String("print")) {
            fprintf(stderr, "UIPROBE_DIALOG_FAIL ten hop thoai khong hop le (merge/about/sign/print)\n");
            return 1;
        }
        if (!QDir().mkpath(outDir)) {
            fprintf(stderr, "UIPROBE_DIALOG_FAIL khong tao duoc %s\n", outDir.toLocal8Bit().constData());
            return 1;
        }

        MainWindow w;
        w.resize(1400, 900);
        w.show();
        QCoreApplication::processEvents();
        w.openFile(inputPath);
        const int loops = qMax(waitMs / 50, 1);
        for (int i = 0; i < loops; ++i) {
            QCoreApplication::processEvents();
            QThread::msleep(50);
        }

        QString err;
        if (!w.probeDialog(dlgName, outDir, &err)) {
            fprintf(stderr, "UIPROBE_DIALOG_FAIL %s\n", err.toLocal8Bit().constData());
            return 1;
        }
        const QString outPng = outDir + QLatin1String("/dialog_") + dlgName + QLatin1String(".png");
        fprintf(stdout, "UIPROBE_DIALOG_OK %s\n", outPng.toLocal8Bit().constData());
        return 0;
    }

    // usage: --uiprobe-frames <input.pdf> <outdir> [intervalMs=600]
    // Chup nhieu khung de dung GIF (SPEC_PROBE_DIALOG_FRAMES phan 2): chay kich
    // ban trinh dien CO DINH, moi buoc ghi <outdir>/frame_XXX.png roi in so khung.
    if (argc >= 4 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--uiprobe-frames")) {
        const QString inputPath = QString::fromLocal8Bit(argv[2]);
        const QString outDir    = QString::fromLocal8Bit(argv[3]);
        int intervalMs = (argc >= 5) ? QString::fromLocal8Bit(argv[4]).toInt() : 600;
        if (intervalMs <= 0) intervalMs = 600;
        if (!QDir().mkpath(outDir)) {
            fprintf(stderr, "UIPROBE_FRAMES_FAIL khong tao duoc %s\n", outDir.toLocal8Bit().constData());
            return 1;
        }

        MainWindow w;
        w.resize(1400, 900);
        w.show();
        QCoreApplication::processEvents();
        w.openFile(inputPath);
        const int loops = qMax(8000 / 50, 1);   // cho nap tai lieu on dinh truoc khi chay kich ban
        for (int i = 0; i < loops; ++i) {
            QCoreApplication::processEvents();
            QThread::msleep(50);
        }

        const int count = w.probeFrames(outDir, intervalMs);
        fprintf(stdout, "UIPROBE_FRAMES_OK %d\n", count);
        return 0;
    }

    // usage: --viewprobe <input.pdf> <out.png> <continuous:0|1> <zoomPercent> <page1Based> [waitMs] [centerXpt] [centerYpt]
    // Probe: lai che do xem lien tuc + zoom + trang de nghiem thu annot tren ban ve CAD nang
    if (argc >= 7 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--viewprobe")) {
        QString inputPath = QString::fromLocal8Bit(argv[2]);
        QString outPng = QString::fromLocal8Bit(argv[3]);
        bool continuous = (QString::fromLocal8Bit(argv[4]) == QLatin1String("1"));
        bool zoomOk = false;
        double zoomPercent = QString::fromLocal8Bit(argv[5]).toDouble(&zoomOk);
        int page1Based = QString::fromLocal8Bit(argv[6]).toInt();
        int waitMs = (argc >= 8) ? QString::fromLocal8Bit(argv[7]).toInt() : 15000;
        const double kNaN = std::numeric_limits<double>::quiet_NaN();
        double centerXpt = kNaN, centerYpt = kNaN;
        if (argc >= 9) centerXpt = QString::fromLocal8Bit(argv[8]).toDouble();
        if (argc >= 10) centerYpt = QString::fromLocal8Bit(argv[9]).toDouble();
        if (!zoomOk) {
            fprintf(stderr, "VIEWPROBE: FAIL zoom khong hop le\n");
            return 1;
        }

        MainWindow w;
        w.resize(1600, 1000);
        w.show();
        QCoreApplication::processEvents();

        w.openFile(inputPath);

        // Chờ nạp xong y như --guiprobe
        for (int i = 0; i < 60; ++i) {
            QCoreApplication::processEvents();
            QThread::msleep(50);
        }

        w.probeSetView(continuous, zoomPercent, page1Based, centerXpt, centerYpt);
        if (!std::isnan(centerXpt) && !std::isnan(centerYpt))
            fprintf(stdout, "VIEWPROBE_CENTER %.1f %.1f\n", centerXpt, centerYpt);

        // CAD 92 trang render tien-dan rat cham: chay du waitMs, toi thieu 80 vong
        const int loops = qMax(waitMs / 50, 80);
        for (int i = 0; i < loops; ++i) {
            QCoreApplication::processEvents();
            QThread::msleep(50);
        }

        QPixmap pm = w.grab();
        QImage img = pm.toImage();
        if (img.isNull()) {
            fprintf(stderr, "VIEWPROBE: FAIL grab returned null\n");
            return 1;
        }
        if (!img.save(outPng, "PNG")) {
            fprintf(stderr, "VIEWPROBE: FAIL cannot save %s\n", outPng.toLocal8Bit().constData());
            return 1;
        }

        fprintf(stdout, "VIEWPROBE_OK %s\n", outPng.toLocal8Bit().constData());
        return 0;
    }

    // usage: --search-fold-test <pdf>
    // Nghiem thu tim kiem BO DAU (SPEC_ABOUT_PICK_SEARCH phan 3). Truy van
    // NHUNG CUNG trong ma nguon — KHONG lay tu argv (chu Viet qua argv hong
    // am tham vi main doc bang fromLocal8Bit). Neu <pdf> chua ton tai thi tao
    // PDF co chu tieng Viet that (NFC + NFD + khong dau) roi chay luon.
    if (argc >= 3 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--search-fold-test")) {
        QTextStream out(stdout);
        QString pdfPath = QString::fromLocal8Bit(argv[2]);
        PdfDocument::libAddRef();   // truoc khi goi bat ky API PDFium (tao PDF hay mo PDF)
        if (!QFileInfo::exists(pdfPath)) {
            // Tao PDF de nghi: go chu tieng Viet that bang FreeText annot.
            // Luu y: AnnotationManager va QPdfWriter deu NFC hoa chuoi nhap
            // vao, nen dong NFD trong PDF se ra dang NFC. Truong hop chu trong
            // PDF da o dang NFD duoc kiem bang foldForMatch() o phia duoi (cung
            // mot ham buildFoldMap dung cho ca chuoi trong trang).
            const QStringList lines = {
                QString::fromUtf8("MẶT"),                                   // NFC
                QString::fromUtf8("MẶT").normalized(QString::NormalizationForm_D), // NFD
                QString::fromUtf8("MAT"),                                  // khong dau
                QString::fromUtf8("MÁT"),                                  // dau khac
                QString::fromUtf8("đứng"),                                 // d co dau
            };
            FPDF_DOCUMENT tdoc = FPDF_CreateNewDocument();
            FPDF_PAGE tpage = FPDFPage_New(tdoc, 0, 612, 792);
            FPDFPage_GenerateContent(tpage);
            FPDF_ClosePage(tpage);
            AnnotationManager tmgr;
            tmgr.setDocument(tdoc, pdfPath);
            for (int i = 0; i < lines.size(); ++i)
                tmgr.createInlineNote(0, QRectF(40, 40 + i * 40, 520, 30), lines[i],
                                      QStringLiteral("foldtest"), false, QColor(Qt::black), 14.0f);
            {
                QFile f(pdfPath);
                f.open(QIODevice::WriteOnly);
                TRFileWriter fw;
                fw.file = &f;
                fw.base.version = 1;
                fw.base.WriteBlock = TRFileWriter::WriteBlock;
                QMutexLocker lock(&s_pdfiumMutex);
                FPDF_SaveAsCopy(tdoc, &fw.base, 0);
            }
            FPDF_CloseDocument(tdoc);
            out << "FOLD_TEST created test pdf: " << pdfPath << "\n";
            out.flush();
        }

        PdfDocument doc;
        if (!doc.open(pdfPath)) {
            out << "FOLD_TEST FAIL cannot open " << pdfPath << "\n";
            out.flush();
            PdfDocument::libRelease();
            return 1;
        }

        // In toan van trang 0 duoi dang hex codepoint de doi chieu dang NFD/NFC
        // co duoc giu nguyen trong PDF that hay khong.
        {
            QMutexLocker lock(&s_pdfiumMutex);
            FPDF_PAGE pg = FPDF_LoadPage(doc.raw(), 0);
            if (pg) {
                FPDF_TEXTPAGE tp = FPDFText_LoadPage(pg);
                if (tp) {
                    const int nc = FPDFText_CountChars(tp);
                    std::vector<unsigned short> b2(static_cast<size_t>(nc) + 1, 0);
                    FPDFText_GetText(tp, 0, nc, b2.data());
                    const QString t2 = QString::fromUtf16(b2.data());
                    QStringList cps;
                    for (const QChar& c : t2)
                        cps << QString::number(c.unicode(), 16);
                    out << "Page text codepoints: " << cps.join(QLatin1Char(' ')) << "\n";
                    out.flush();
                    FPDFText_ClosePage(tp);
                }
                FPDF_ClosePage(pg);
            }
        }

        // Ham chay 1 truy van that, cho searchComplete, tra danh sach ket qua.
        auto runSearch = [&](const QString& query, bool matchDiacritics)
                            -> QList<SearchResult> {
            TextSearch searcher;
            QList<SearchResult> hits;
            QEventLoop loop;
            bool done = false;
            QObject::connect(&searcher, &TextSearch::found, &loop,
                             [&](SearchResult r) { hits.append(r); });
            QObject::connect(&searcher, &TextSearch::searchComplete, &loop,
                             [&](int) { done = true; loop.quit(); });
            searcher.search(&doc, query, Qt::CaseInsensitive, matchDiacritics);
            QTimer::singleShot(60000, &loop, &QEventLoop::quit);
            loop.exec();
            if (!done)
                out << "FOLD_TEST WARN timeout query=" << query << "\n";
            return hits;
        };

        // Truy van nhung cung trong ma nguon (khong di qua argv).
        const QString NFC   = QString::fromUtf8("MẶT");
        const QString NFD   = NFC.normalized(QString::NormalizationForm_D);
        const QString PLAIN = QString::fromUtf8("MAT");
        const QString DUNG  = QString::fromUtf8("đứng");

        struct FoldRow { const char* label; QString query; bool exact; };
        const FoldRow rows[] = {
            { "MẶT  NFC   bo dau    ", NFC,   false },
            { "MẶT  NFD   bo dau    ", NFD,   false },
            { "MAT  —     bo dau    ", PLAIN, false },
            { "MẶT  NFC   chinh xac ", NFC,   true  },
            { "dung  NFC  bo dau    ", DUNG,  false },
        };

        int trioCount = -1;
        bool pass = true;

        // Kiem TRUC TIEP phep gap dang (khong phu thuoc PDF): NFC, NFD va 'd'
        // phai ra cung mot dang so khop.
        const QString m_nfc = QString::fromUtf8("MẶT");
        const QString m_nfd = m_nfc.normalized(QString::NormalizationForm_D);
        out << "fold check: foldForMatch(NFC)=" << TextSearch::foldForMatch(m_nfc)
            << " foldForMatch(NFD)=" << TextSearch::foldForMatch(m_nfd)
            << " foldForMatch(dung)=" << TextSearch::foldForMatch(DUNG)
            << " (ky vong mat/dung)\n";
        out.flush();
        if (TextSearch::foldForMatch(m_nfc) != QStringLiteral("mat")
            || TextSearch::foldForMatch(m_nfd) != QStringLiteral("mat")
            || TextSearch::foldForMatch(DUNG) != QStringLiteral("dung"))
            pass = false;

        out << "\nBang nghiem thu (PDF: " << pdfPath << ")\n";
        for (int idx = 0; idx < 5; ++idx) {
            const FoldRow& row = rows[idx];
            const QList<SearchResult> hits = runSearch(row.query, row.exact);
            const int n = hits.size();
            out << "  [" << idx << "] " << row.label << " -> " << n << " ket qua\n";
            if (idx < 3) {
                if (trioCount < 0) trioCount = n;
                else if (n != trioCount) pass = false;
            } else if (idx == 3) {
                if (n > trioCount) pass = false;
            } else {
                if (n <= 0) pass = false;   // "dung" phai co it nhat 1 ket qua
            }
            out.flush();
        }

        // Chi tiet: charIdx/charCount + rect cua vao ket qua dau (che do bo dau).
        out << "\nChi tiet che do bo dau (MẶT NFC): charIdx/charCount + rect0\n";
        const QList<SearchResult> foldHits = runSearch(NFC, false);
        for (int i = 0; i < qMin(4, foldHits.size()); ++i) {
            const SearchResult& r = foldHits[i];
            const QRectF rc = r.rects.isEmpty() ? QRectF() : r.rects.first();
            out << "  [" << i << "] page=" << (r.pageIndex + 1)
                << " charIdx=" << r.charIdx << " charCount=" << r.charCount
                << " rect0=" << rc.x() << "," << rc.y()
                << "," << rc.width() << "x" << rc.height()
                << " snippet=\"" << r.contextSnippet.left(30) << "\"\n";
        }
        out.flush();

        // Rect che do bo dau phai TRUNG rect che do chinh xac cho cung vi tri
        // (chung to anh xa chi so nguoc khong lech).
        out << "\nDoi chieu rect bo dau vs chinh xac (cung vi tri phai trung):\n";
        const QList<SearchResult> exactHits = runSearch(NFC, true);
        int matchRect = 0;
        for (const SearchResult& e : exactHits) {
            if (e.rects.isEmpty()) continue;
            bool found = false;
            for (const SearchResult& f : foldHits) {
                if (f.rects.isEmpty()) continue;
                if (f.pageIndex == e.pageIndex && f.rects.first() == e.rects.first()
                    && f.charIdx == e.charIdx && f.charCount == e.charCount) {
                    found = true;
                    break;
                }
            }
            out << "  exact rect0=" << e.rects.first().x() << "," << e.rects.first().y()
                << " charIdx=" << e.charIdx << " charCount=" << e.charCount
                << " -> " << (found ? "TRUNG" : "KHONG TRUNG") << "\n";
            if (!found) pass = false;
            ++matchRect;
        }
        out.flush();

        out << "\nFOLD_TEST " << (pass ? "PASS" : "FAIL") << "\n";
        out.flush();
        PdfDocument::libRelease();
        return pass ? 0 : 1;
    }

    // usage: --about-probe <outdir>
    // Chup AboutDialog ca 2 theme ra PNG de kiem hinh thuc: logo card dung mau
    // theme (khong con mang trang), chu License dung token, font he thong.
    // Kem in mau DIEM ANH THAT cua logo card va vung license de nghiem thu so.
    if (argc >= 3 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--about-probe")) {
        const QString outDir = QString::fromLocal8Bit(argv[2]);
        QDir().mkpath(outDir);
        for (bool dark : { true, false }) {
            const ThemeTokens& t = dark ? darkHC() : lightHC();
            qApp->setStyleSheet(
                QStringLiteral("QDialog { background:%1; color:%2; } "
                               "QLabel { background:transparent; } "
                               "QTextBrowser, QTextEdit { background:%1; color:%2; }")
                    .arg(t.bgAlt, t.fg));
            AboutDialog dlg(dark);
            dlg.show();
            for (int i = 0; i < 20; ++i) { QCoreApplication::processEvents(); QThread::msleep(20); }
            const QString tag = dark ? QStringLiteral("dark") : QStringLiteral("light");
            const QPixmap pm = dlg.grab();
            const bool ok = pm.isNull() ? false
                : pm.save(outDir + QStringLiteral("/about_%1.png").arg(tag), "PNG");
            // Mau diem anh that cua logo card (QLabel co pixmap) + vung license.
            auto pixAt = [](QWidget* w) -> QString {
                if (!w || w->width() <= 0 || w->height() <= 0) return QStringLiteral("n/a");
                const QImage img = w->grab().toImage();
                if (img.isNull()) return QStringLiteral("n/a");
                return img.pixelColor(img.width() / 2, 2).name();
            };
            QString logoPix = QStringLiteral("n/a"), licPix = QStringLiteral("n/a");
            for (QLabel* l : dlg.findChildren<QLabel*>())
                if (!l->pixmap(Qt::ReturnByValue).isNull()) { logoPix = pixAt(l); break; }
            for (QTextBrowser* tb : dlg.findChildren<QTextBrowser*>())
                { licPix = pixAt(tb); break; }
            qInfo().noquote() << QString("[aboutprobe] %1 saved=%2 logoPix=%3 licPix=%4")
                .arg(tag).arg(ok ? 1 : 0).arg(logoPix, licPix);
        }
        qApp->setStyleSheet(QString());
        return 0;
    }

    // usage: --searchnav-test <input.pdf> <query> <continuous:0|1> <zoomPercent> <resultIdx1Based> [waitMs]
    // Probe (SPEC_SEARCH_NAV_R2): tim kiem THAT, roi "bam" ket qua thu
    // resultIdx1Based qua dung tin hieu searchResultSelected. Dinh kem dong
    // qInfo "[searchnav] ..." o MainWindow de nghiem thu bang so.
    if (argc >= 6 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--searchnav-test")) {
        QString inputPath = QString::fromLocal8Bit(argv[2]);
        QString query     = QString::fromLocal8Bit(argv[3]);
        bool continuous   = (QString::fromLocal8Bit(argv[4]) == QLatin1String("1"));
        bool zoomOk = false;
        double zoomPercent = QString::fromLocal8Bit(argv[5]).toDouble(&zoomOk);
        int resultIdx = QString::fromLocal8Bit(argv[6]).toInt();
        int waitMs = (argc >= 8) ? QString::fromLocal8Bit(argv[7]).toInt() : 15000;
        if (!zoomOk) {
            fprintf(stderr, "SEARCHNAV: FAIL zoom khong hop le\n");
            return 1;
        }

        MainWindow w;
        w.resize(1600, 1000);
        w.show();
        QCoreApplication::processEvents();

        w.openFile(inputPath);
        for (int i = 0; i < 60; ++i) {
            QCoreApplication::processEvents();
            QThread::msleep(50);
        }

        w.probeSearchNav(continuous, zoomPercent, query, resultIdx, waitMs);
        for (int i = 0; i < 20; ++i) {
            QCoreApplication::processEvents();
            QThread::msleep(50);
        }
        fprintf(stdout, "SEARCHNAV_OK continuous=%d zoom=%d result=%d\n",
                continuous ? 1 : 0, qRound(zoomPercent), resultIdx);
        return 0;
    }

    // usage: --searchstate-test <pdfA> <pdfB> <query> <continuous:0|1> <zoomPercent> [waitMs]
    // Probe (SPEC_SEARCH_STATE_R3): nghiem thu 3 loi trang thai tim kiem bang
    // log — tim o A roi doi tab A/B phai giu ket qua rieng, va [hl] moi trang
    // phai drawn>0 khi lọt vao vung nhin o che do lien tuc.
    if (argc >= 7 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--searchstate-test")) {
        QString pathA = QString::fromLocal8Bit(argv[2]);
        QString pathB = QString::fromLocal8Bit(argv[3]);
        QString query = QString::fromLocal8Bit(argv[4]);
        bool continuous = (QString::fromLocal8Bit(argv[5]) == QLatin1String("1"));
        bool zoomOk = false;
        double zoomPercent = QString::fromLocal8Bit(argv[6]).toDouble(&zoomOk);
        int waitMs = (argc >= 8) ? QString::fromLocal8Bit(argv[7]).toInt() : 15000;
        if (!zoomOk) {
            fprintf(stderr, "SEARCHSTATE: FAIL zoom khong hop le\n");
            return 1;
        }

        MainWindow w;
        w.resize(1600, 1000);
        w.show();
        QCoreApplication::processEvents();

        w.probeSearchState(pathA, pathB, query, continuous, zoomPercent, waitMs);
        for (int i = 0; i < 20; ++i) {
            QCoreApplication::processEvents();
            QThread::msleep(50);
        }
        fprintf(stdout, "SEARCHSTATE_DONE continuous=%d zoom=%d\n",
                continuous ? 1 : 0, qRound(zoomPercent));
        return 0;
    }

    // usage: --ocr-layer-test <outdir>
    // Kiem tra TANG CHU VO HINH khong can Tesseract (chay duoc tren Linux):
    // chen cac tu gia (OcrWord) vao mot trang moi, roi kiem:
    //   1) FPDFText_CountChars sau khi chen > 0
    //   2) OcrTextLayer::pageDone ba 0 lan dau, 1 lan sau (khong chen 2 lan)
    //   3) FPDFText_GetText doc nguoc lai dung chu da chen (ke ca tieng Viet)
    //   4) Box point sau khi doc nguoc khop vi tri da chen (sai so nho)
    //   5) sha256 file PDF tren dia khong doi sau khi chen (lop chi trong RAM)
    if (argc >= 3 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--ocr-layer-test")) {
        QTextStream out(stdout);
        const QString outDir = QString::fromLocal8Bit(argv[2]);
        QDir().mkpath(outDir);
        PdfDocument::libAddRef();

        const QString pdfPath = outDir + QLatin1String("/layer_test.pdf");
        FPDF_DOCUMENT doc = FPDF_CreateNewDocument();
        if (!doc) { out << "LAYER FAIL create doc\n"; out.flush(); PdfDocument::libRelease(); return 1; }
        {
            QMutexLocker lock(&s_pdfiumMutex);
            FPDF_PAGE np = FPDFPage_New(doc, 0, 300, 200);
            if (!np) { out << "LAYER FAIL create page\n"; out.flush(); FPDF_CloseDocument(doc); PdfDocument::libRelease(); return 1; }
            FPDFPage_GenerateContent(np);
            FPDF_ClosePage(np);
        }
        { QFile f(pdfPath);
          if (!f.open(QIODevice::WriteOnly)) { out << "LAYER FAIL write\n"; out.flush(); FPDF_CloseDocument(doc); PdfDocument::libRelease(); return 1; }
          TRFileWriter fw; fw.file = &f; fw.base.version = 1; fw.base.WriteBlock = TRFileWriter::WriteBlock;
          bool ok = false; { QMutexLocker lock(&s_pdfiumMutex); ok = FPDF_SaveAsCopy(doc, &fw.base, 0) != 0; }
          f.close();
          if (!ok) { out << "LAYER FAIL save base\n"; out.flush(); FPDF_CloseDocument(doc); PdfDocument::libRelease(); return 1; }
        }
        auto sha = [](const QString& p) -> QString {
            QFile f(p);
            if (!f.open(QIODevice::ReadOnly)) return QString();
            QCryptographicHash h(QCryptographicHash::Sha256);
            while (!f.atEnd()) h.addData(f.read(1024 * 1024));
            return QString::fromLatin1(h.result().toHex());
        };
        const QString beforeSha = sha(pdfPath);

        // Tu gia dat tai vi tri da biet: (x,y) point, cao 10pt
        QVector<OcrWord> words;
        auto mk = [&](const QString& t, double x, double y, float conf) {
            OcrWord w; w.text = t; w.boxPt = QRectF(x, y, t.size() * 5.0, 10.0); w.conf = conf; words << w;
        };
        mk(QStringLiteral("Hello"), 20, 150, 95.0f);
        mk(QStringLiteral("Hatch"), 90, 150, 94.0f);
        mk(QStringLiteral("Gạch"), 160, 150, 90.0f);   // tieng Viet

        out << "pageDone before = " << OcrTextLayer::pageDone(doc, 0) << "\n";
        const int inserted = OcrTextLayer::insertPage(doc, 0, words);
        out << "inserted = " << inserted << "\n";
        if (inserted != words.size()) { out << "LAYER FAIL insert count\n"; out.flush(); FPDF_CloseDocument(doc); PdfDocument::libRelease(); return 1; }
        out << "pageDone after = " << OcrTextLayer::pageDone(doc, 0) << "\n";
        if (!OcrTextLayer::pageDone(doc, 0)) { out << "LAYER FAIL pageDone\n"; out.flush(); FPDF_CloseDocument(doc); PdfDocument::libRelease(); return 1; }

        // Chen lan 2 phai bi tu choi (khong nhan doi)
        const int dup = OcrTextLayer::insertPage(doc, 0, words);
        out << "reinsert (expect 0) = " << dup << "\n";
        if (dup != 0) { out << "LAYER FAIL reinsert not blocked\n"; out.flush(); FPDF_CloseDocument(doc); PdfDocument::libRelease(); return 1; }

        // Doc nguoc text + box
        int nChars = 0; QString gotText;
        QVector<QRectF> gotBoxes;
        {
            QMutexLocker lock(&s_pdfiumMutex);
            FPDF_PAGE pg = FPDF_LoadPage(doc, 0);
            if (!pg) { out << "LAYER FAIL load page\n"; out.flush(); FPDF_CloseDocument(doc); PdfDocument::libRelease(); return 1; }
            FPDF_TEXTPAGE tp = FPDFText_LoadPage(pg);
            nChars = tp ? FPDFText_CountChars(tp) : -1;
            if (tp && nChars > 0) {
                std::vector<unsigned short> buf(nChars + 1, 0);
                FPDFText_GetText(tp, 0, nChars, buf.data());
                gotText = QString::fromUtf16(buf.data()).trimmed();
                for (int i = 0; i < nChars; ++i) {
                    double l = 0, t = 0, r = 0, b = 0;
                    if (FPDFText_GetCharBox(tp, i, &l, &r, &t, &b))
                        gotBoxes << QRectF(l, b, r - l, t - b);
                }
            }
            if (tp) FPDFText_ClosePage(tp);
            FPDF_ClosePage(pg);
        }
        out << "CountChars after = " << nChars << "\n";
        if (nChars <= 0) { out << "LAYER FAIL no chars\n"; out.flush(); FPDF_CloseDocument(doc); PdfDocument::libRelease(); return 1; }
        out << "extracted = [" << gotText << "]\n";
        if (!gotText.contains(QLatin1String("Hello")) || !gotText.contains(QLatin1String("Hatch"))
            || !gotText.contains(QStringLiteral("Gạch"))) {
            out << "LAYER FAIL text mismatch\n"; out.flush(); FPDF_CloseDocument(doc); PdfDocument::libRelease(); return 1;
        }
        // Kiem vi tri: tu "Hello" bat dau o x=20, y=150. Char box dau tien
        // phai gan x=20 va y=150..160.
        if (gotBoxes.size() >= 1) {
            const QRectF b0 = gotBoxes.first();
            out << "first char box = (" << QString::number(b0.left(), 'f', 1) << ","
                << QString::number(b0.top(), 'f', 1) << ")..("
                << QString::number(b0.right(), 'f', 1) << ","
                << QString::number(b0.bottom(), 'f', 1) << ")\n";
            const bool xOk = qAbs(b0.left() - 20.0) < 3.0;
            const bool yOk = b0.bottom() >= 148.0 && b0.bottom() <= 152.0;
            if (!xOk || !yOk) { out << "LAYER FAIL pos mismatch\n"; out.flush(); FPDF_CloseDocument(doc); PdfDocument::libRelease(); return 1; }
        }

        const QString afterSha = sha(pdfPath);
        out << "sha256 unchanged = " << (beforeSha == afterSha && !beforeSha.isEmpty() ? "YES" : "NO") << "\n";
        if (beforeSha != afterSha) { out << "LAYER FAIL file modified\n"; out.flush(); FPDF_CloseDocument(doc); PdfDocument::libRelease(); return 1; }

        // ── 7) Ghi ban sao CO chu vo hinh de nghiem thu chon chu (SPEC_TEXTSEL
        // ADOBE muc 4): doc nay chua lop OCR, FPDFText_* thay nhu chu thuong.
        const QString withTextPath = outDir + QLatin1String("/layer_with_text.pdf");
        { QFile f(withTextPath);
          if (f.open(QIODevice::WriteOnly)) {
              TRFileWriter fw; fw.file = &f; fw.base.version = 1; fw.base.WriteBlock = TRFileWriter::WriteBlock;
              { QMutexLocker lock(&s_pdfiumMutex); FPDF_SaveAsCopy(doc, &fw.base, 0); }
              f.close();
          }
        }
        out << "Saved OCR-layer copy (for textsel): " << withTextPath << "\n";

        FPDF_CloseDocument(doc);
        OcrTextLayer::forgetDocument(doc);
        out << "OCR_LAYER_OK\n";
        out.flush();
        PdfDocument::libRelease();
        return 0;
    }

    // usage: --ocr-pixdiff-test <outdir>
    // Pixel-diff cua lop chu vo hinh — loai bo loi "chu OCR hien de len anh scan".
    // Trang GIAU object (2005 tu, qua nguong 2000 cua VectorLayer::build) de nhanh
    // TEXT cua lop vector THUC SU chay. Text duoc chen theo kieu MO PHONG PDFium
    // Windows (bblanchon, khong ho tro SetTextRenderMode): chi fill alpha=0, render
    // mode van la FILL. Loi cu: lop vector bo qua alpha, GetRenderedBitmap ve chu
    // thanh muc den => to tile co muc. Ve chuan FPDF_RenderPageBitmap ton trong
    // alpha=0 => khong ra muc. Ca hai deu nen trang => diff = 0 (muc tieu).
    if (argc >= 3 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--ocr-pixdiff-test")) {
        QTextStream out(stdout);
        const QString outDir = QString::fromLocal8Bit(argv[2]);
        QDir().mkpath(outDir);
        PdfDocument::libAddRef();

        const double PW = 800.0, PH = 600.0;   // point
        FPDF_DOCUMENT doc = FPDF_CreateNewDocument();
        if (!doc) { out << "PIXDIFF FAIL create doc\n"; out.flush(); PdfDocument::libRelease(); return 1; }

        FPDF_FONT font = nullptr;
        {
            QMutexLocker lock(&s_pdfiumMutex);
            FPDF_PAGE np = FPDFPage_New(doc, 0, PW, PH);
            if (!np) { out << "PIXDIFF FAIL create page\n"; out.flush(); FPDF_CloseDocument(doc); PdfDocument::libRelease(); return 1; }
            FPDFPage_GenerateContent(np);
            FPDF_ClosePage(np);

            QFile f(QStringLiteral(":/fonts/DejaVuSans.ttf"));
            QByteArray data;
            if (f.open(QIODevice::ReadOnly)) data = f.readAll();
            if (data.isEmpty()) { out << "PIXDIFF FAIL font\n"; out.flush(); FPDF_CloseDocument(doc); PdfDocument::libRelease(); return 1; }
            font = FPDFText_LoadFont(doc, reinterpret_cast<const uint8_t*>(data.constData()),
                                     static_cast<uint32_t>(data.size()), FPDF_FONT_TRUETYPE, /*cid=*/1);
            if (!font) { out << "PIXDIFF FAIL load font\n"; out.flush(); FPDF_CloseDocument(doc); PdfDocument::libRelease(); return 1; }
        }

        // Chen 2005 tu: FILL mode + fill alpha=0 (mo phong PDFium Windows bblanchon
        // khong ho tro SetTextRenderMode — INVISIBLE khong dat duoc, chi con alpha).
        const int perRow = 130;
        const int nWords = 2005;
        {
            QMutexLocker lock(&s_pdfiumMutex);
            FPDF_PAGE page = FPDF_LoadPage(doc, 0);
            if (!page) { out << "PIXDIFF FAIL load page\n"; out.flush(); FPDF_CloseDocument(doc); PdfDocument::libRelease(); return 1; }
            for (int i = 0; i < nWords; ++i) {
                const int row = i / perRow, col = i % perRow;
                const double x = 20.0 + col * 6.0, y = 20.0 + row * 11.0;
                FPDF_PAGEOBJECT obj = FPDFPageObj_CreateTextObj(doc, font, 11.5f);
                if (!obj) continue;
                const QString text = QStringLiteral("OCRinvisible");
                if (!FPDFText_SetText(obj, reinterpret_cast<FPDF_WIDESTRING>(text.utf16()))) {
                    FPDFPageObj_Destroy(obj);
                    continue;
                }
                // CHI alpha=0, KHONG dat render mode (mo phong loi PDFium Windows)
                FPDFPageObj_SetFillColor(obj, 0, 0, 0, 0);
                FPDFPageObj_SetStrokeColor(obj, 0, 0, 0, 0);
                FS_MATRIX m{1.0f, 0.0f, 0.0f, 1.0f, float(x), float(y)};
                FPDFPageObj_SetMatrix(obj, &m);
                FPDFPage_InsertObject(page, obj);
            }
            FPDFPage_GenerateContent(page);
            FPDF_ClosePage(page);
        }
        out << "words inserted (fill alpha=0, FILL mode) = " << nWords << "\n";

        const double scale = 3.0;   // 3 px/pt — du de thay muc den
        const int wPx = qMax(1, (int)std::lround(PW * scale));
        const int hPx = qMax(1, (int)std::lround(PH * scale));

        VectorLayer vl;
        {
            QMutexLocker lock(&s_pdfiumMutex);
            const bool built = vl.build(doc, 0);
            if (!built) { out << "PIXDIFF FAIL vector build (nObj<=2000?)\n"; out.flush(); FPDF_CloseDocument(doc); PdfDocument::libRelease(); return 1; }
        }
        out << "textTiles after fix = " << vl.textTiles().size() << " (expect 0)\n";

        QImage tileImg(wPx, hPx, QImage::Format_ARGB32);
        tileImg.fill(Qt::white);
        {
            QPainter p(&tileImg);
            p.setRenderHint(QPainter::SmoothPixmapTransform, false);
            for (const TextTile& t : vl.textTiles()) {
                QImage a = t.img.convertToFormat(QImage::Format_ARGB32);
                const QRgb c = t.color;
                for (int y = 0; y < a.height(); ++y)
                    for (int x = 0; x < a.width(); ++x) {
                        const int al = qAlpha(a.pixel(x, y));
                        if (al == 0) continue;
                        a.setPixel(x, y, qRgba(qRed(c), qGreen(c), qBlue(c), al));
                    }
                QRectF target(t.rectPt.left() * scale, (PH - t.rectPt.top()) * scale,
                              t.rectPt.width() * scale, t.rectPt.height() * scale);
                p.drawImage(target, a);
            }
        }

        QImage refImg(wPx, hPx, QImage::Format_ARGB32);
        refImg.fill(Qt::white);
        {
            QMutexLocker lock(&s_pdfiumMutex);
            FPDF_PAGE pg = FPDF_LoadPage(doc, 0);
            if (!pg) { out << "PIXDIFF FAIL load page\n"; out.flush(); FPDF_CloseDocument(doc); PdfDocument::libRelease(); return 1; }
            FPDF_BITMAP bmp = FPDFBitmap_CreateEx(wPx, hPx, FPDFBitmap_BGRA,
                                                  refImg.bits(), refImg.bytesPerLine());
            if (bmp) {
                FPDFBitmap_FillRect(bmp, 0, 0, wPx, hPx, 0xFFFFFFFF);
                FPDF_RenderPageBitmap(bmp, pg, 0, 0, wPx, hPx, 0, 0);
                FPDFBitmap_Destroy(bmp);
            }
            FPDF_ClosePage(pg);
        }

        qint64 diff = 0, inkTile = 0, inkRef = 0;
        for (int y = 0; y < hPx; ++y) {
            const QRgb* rt = reinterpret_cast<const QRgb*>(tileImg.constScanLine(y));
            const QRgb* rr = reinterpret_cast<const QRgb*>(refImg.constScanLine(y));
            for (int x = 0; x < wPx; ++x) {
                if (rt[x] != 0xFFFFFFFFu) ++inkTile;
                if (rr[x] != 0xFFFFFFFFu) ++inkRef;
                if (rt[x] != rr[x]) ++diff;
            }
        }
        out << "pixel-diff invisible layer: inkTiles=" << inkTile
            << " inkRasterRef=" << inkRef << " diff=" << diff << "\n";

        FPDF_CloseDocument(doc);
        // textTiles != 0 la dau hieu LOI tren moi ban PDFium (object vo hinh van
        // duoc nop vao lop vector). ink/diff chi thay muc tren ban Windows
        // (bblanchon, GetRenderedBitmap ve chu de len anh scan).
        if (vl.textTiles().size() != 0 || diff != 0 || inkTile != 0 || inkRef != 0) {
            out << "PIXDIFF FAIL invisible text drawn by vector layer\n"; out.flush();
            PdfDocument::libRelease(); return 1;
        }
        out << "OCR_PIXDIFF_OK\n";
        out.flush();
        PdfDocument::libRelease();
        return 0;
    }

    // usage: --ocr-accept <source.pdf> <srcPageIndex> <workdir> [langs]
    // Nghiem thu OCR 3a tren Linux, bao cao bang so:
    //   1) Dung PDF chi-anh tu 1 trang cua source.pdf (rasterize roi boc lai)
    //   2) Dem FPDFText_CountChars TRUOC OCR (phai = 0)
    //   3) Chay OCR + chen lop chu vo hinh, in CountChars SAU, so tu, thoi gian
    //   4) In 3 tu: hinh chu nhat pixel (Tesseract) + hinh chu nhat point (PDF)
    //   5) Chay TextSearch tim mot tu OCR doc duoc, kiem chieu cao highlight deu
    //   6) sha256 file PDF truoc/sau (phai giong nhau — khong sua file)
    // [langs] (tu dong 5) la ma Tesseract mac dinh "vie+eng" — them de nghiem
    // thu cac goi ngon ngu moi (SPEC_OCR_LANGPACK phan nghiem thu muc 4).
    if (argc >= 4 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--ocr-accept")) {
        QTextStream out(stdout);
        const QString srcPath = QString::fromLocal8Bit(argv[2]);
        const int srcPage = QString::fromLocal8Bit(argv[3]).toInt();
        const QString workdir = QString::fromLocal8Bit(argv[4]);
        const QString langs = (argc >= 5) ? QString::fromLocal8Bit(argv[5])
                                          : QStringLiteral("vie+eng");
        QDir().mkpath(workdir);

        PdfDocument::libAddRef();

        // ── 1) Dung PDF chi-anh: rasterize srcPage cua source.pdf ───────────
        const QString pdfPath = workdir + QLatin1String("/ocr_test_image.pdf");
        const int dpi = 300;   // can nho tuyet doi cua engine (OCR chat luong cao)
        int effDpi = dpi;
        double srcW = 0, srcH = 0;
        {
            FPDF_DOCUMENT sdoc = nullptr;
            { QMutexLocker lock(&s_pdfiumMutex); sdoc = FPDF_LoadDocument(srcPath.toUtf8().constData(), nullptr); }
            if (!sdoc) { out << "OCR_ACCEPT FAIL cannot open " << srcPath << "\n"; out.flush(); PdfDocument::libRelease(); return 1; }
            FPDF_PAGE sp = nullptr;
            { QMutexLocker lock(&s_pdfiumMutex); sp = FPDF_LoadPage(sdoc, srcPage); }
            if (!sp) { out << "OCR_ACCEPT FAIL cannot load source page\n"; out.flush(); FPDF_CloseDocument(sdoc); PdfDocument::libRelease(); return 1; }
            { QMutexLocker lock(&s_pdfiumMutex);
              srcW = FPDF_GetPageWidth(sp);
              srcH = FPDF_GetPageHeight(sp);
            }
            // dpi theo kich thuoc trang — giong engine: canh dai ~5000px, tran 600.
            const double maxEdgePt = qMax(srcW, srcH);
            effDpi = (maxEdgePt > 0.0)
                ? qBound(300, (int)std::lround(5000.0 * 72.0 / maxEdgePt), 600)
                : dpi;
            const int wPx = qMax(1, (int)std::lround(srcW * effDpi / 72.0));
            const int hPx = qMax(1, (int)std::lround(srcH * effDpi / 72.0));
            std::vector<unsigned char> px((size_t)wPx * hPx, 255);
            FPDF_BITMAP bmp = nullptr;
            { QMutexLocker lock(&s_pdfiumMutex);
              bmp = FPDFBitmap_CreateEx(wPx, hPx, FPDFBitmap_Gray, px.data(), wPx);
              if (bmp) { FPDFBitmap_FillRect(bmp, 0, 0, wPx, hPx, 0xFFFFFFFF);
                         FPDF_RenderPageBitmap(bmp, sp, 0, 0, wPx, hPx, 0, 0); }
            }
            FPDF_ClosePage(sp);
            if (!bmp) { out << "OCR_ACCEPT FAIL render\n"; out.flush(); FPDF_CloseDocument(sdoc); PdfDocument::libRelease(); return 1; }
            FPDFBitmap_Destroy(bmp);
            FPDF_CloseDocument(sdoc);

            // Tao PDF chi-anh bang QPdfWriter (Qt6::Gui) — boi FPDFImageObj
            // trong PDFium ban nay khong nhan BGRA/Gray tu CreateEx (ra anh trang).
            // Dung QImage voi stride ro rang roi .copy() de loai bo bo dem.
            QImage gray(reinterpret_cast<const uchar*>(px.data()), wPx, hPx, wPx,
                        QImage::Format_Grayscale8);
            const QImage grayCopy = gray.copy();
            {
                QPdfWriter pdfw(pdfPath);
                pdfw.setPageLayout(QPageLayout(QPageSize(QSizeF(srcW, srcH), QPageSize::Point),
                                               QPageLayout::Portrait, QMarginsF()));
                // QPdfWriter lam viec theo device unit: o resolution=effDpi, trang
                // rong srcW*effDpi/72 device px. Ve hinh vao dung toan bo vung do.
                pdfw.setResolution(effDpi);
                QPainter p(&pdfw);
                p.drawImage(QRectF(0, 0, srcW * effDpi / 72.0, srcH * effDpi / 72.0), grayCopy);
                p.end();
            }
        }

        // ── 2) Mo PDF chi-anh: dem chu TRUOC OCR ────────────────────────────
        PdfDocument doc;
        if (!doc.open(pdfPath)) { out << "OCR_ACCEPT FAIL reopen\n"; out.flush(); PdfDocument::libRelease(); return 1; }
        out << "Image PDF: " << pdfPath << "\n";
        out << "Page size: " << QString::number(srcW, 'f', 1) << " x "
            << QString::number(srcH, 'f', 1) << " pt\n";
        out << "Raster dpi: " << effDpi << " (page long edge " << qMax(srcW, srcH)
            << "pt => target ~5000px)\n";

        auto countChars = [&](FPDF_DOCUMENT d, int p) -> int {
            QMutexLocker lock(&s_pdfiumMutex);
            FPDF_PAGE pg = FPDF_LoadPage(d, p);
            if (!pg) return -1;
            FPDF_TEXTPAGE tp = FPDFText_LoadPage(pg);
            int n = tp ? FPDFText_CountChars(tp) : -1;
            if (tp) FPDFText_ClosePage(tp);
            FPDF_ClosePage(pg);
            return n;
        };
        const int beforeChars = countChars(doc.raw(), 0);
        out << "FPDFText_CountChars BEFORE OCR = " << beforeChars << " (expect 0)\n";
        if (beforeChars != 0) { out << "OCR_ACCEPT FAIL image pdf has text\n"; out.flush(); PdfDocument::libRelease(); return 1; }

        // ── 3) OCR ───────────────────────────────────────────────────────────
        QString whyNot;
        if (!OcrEngine::available(&whyNot)) {
            out << "OCR_ACCEPT FAIL engine unavailable: " << whyNot << "\n"; out.flush(); PdfDocument::libRelease(); return 1;
        }
        QElapsedTimer timer; timer.start();
        auto words = OcrEngine::recognizePage(doc.raw(), 0, langs, dpi,
                                              []() { return false; });
        const qint64 ocrMs = timer.elapsed();
        out << "OCR time: " << ocrMs << " ms\n";
        out << "Words recognized: " << words.size() << "\n";
        if (words.isEmpty()) { out << "OCR_ACCEPT FAIL no words\n"; out.flush(); PdfDocument::libRelease(); return 1; }

        // ── 4) In 3 tu: box pixel (tu Tesseract) + box point (da quy doi) ────
        // De co box pixel can render lai o cung dpi — tra ve gia tri tu box point
        // quy nguoc de doi chieu. Box point da tinh: x = px*72/dpi, y = hPt - py*72/dpi.
        out << "Coordinate check (3 words, rot=0, origin=(0,0)):\n";
        for (int i = 0; i < qMin(3, words.size()); ++i) {
            const OcrWord& w = words[i];
            const double leftPx  = w.boxPt.left() * dpi / 72.0;
            const double topPx   = (srcH - w.boxPt.top()) * dpi / 72.0;
            const double rightPx = w.boxPt.right() * dpi / 72.0;
            const double botPx   = (srcH - w.boxPt.bottom()) * dpi / 72.0;
            out << "  \"" << w.text << "\" conf=" << QString::number(w.conf, 'f', 0)
                << "  pixel=(" << QString::number(leftPx, 'f', 0) << "," << QString::number(topPx, 'f', 0)
                << ")..(" << QString::number(rightPx, 'f', 0) << "," << QString::number(botPx, 'f', 0) << ")"
                << "  point=(" << QString::number(w.boxPt.left(), 'f', 1) << "," << QString::number(w.boxPt.top(), 'f', 1)
                << ")..(" << QString::number(w.boxPt.right(), 'f', 1) << "," << QString::number(w.boxPt.bottom(), 'f', 1)
                << ")\n";
        }

        // ── Chen lop chu vo hinh ─────────────────────────────────────────────
        const int inserted = OcrTextLayer::insertPage(doc.raw(), 0, words);
        out << "Invisible text objects inserted: " << inserted << "\n";
        if (inserted <= 0) { out << "OCR_ACCEPT FAIL insert\n"; out.flush(); PdfDocument::libRelease(); return 1; }

        // Trang da OCR — chen lan 2 phai tra ve 0 (khong tao chu trung lap)
        const int reinsert = OcrTextLayer::insertPage(doc.raw(), 0, words);
        out << "Re-insert same page (expect 0): " << reinsert << "\n";
        if (reinsert != 0) { out << "OCR_ACCEPT FAIL double insert\n"; out.flush(); PdfDocument::libRelease(); return 1; }

        const int afterChars = countChars(doc.raw(), 0);
        out << "FPDFText_CountChars AFTER OCR = " << afterChars << " (expect > 0)\n";
        if (afterChars <= 0) { out << "OCR_ACCEPT FAIL no chars after\n"; out.flush(); PdfDocument::libRelease(); return 1; }

        // ── 5) TextSearch tim mot tu + chieu cao highlight ───────────────────
        // Dung tu DAU TIEN (hoac tu dai nhat trong 5 tu dau) de tim tron tu,
        // khong tim ky tu don — chieu cao highlight moi co y nghia.
        QString query = words[0].text;
        for (int i = 1; i < qMin(5, words.size()); ++i)
            if (words[i].text.size() > query.size()) query = words[i].text;
        if (query.size() < 1) query = QStringLiteral("a");
        TextSearch searcher;
        QList<SearchResult> results;
        QEventLoop loop2;
        QObject::connect(&searcher, &TextSearch::found, &loop2, [&](SearchResult r) { results << r; });
        bool done = false;
        QObject::connect(&searcher, &TextSearch::searchComplete, &loop2, [&](int) { done = true; loop2.quit(); });
        searcher.search(&doc, query, Qt::CaseInsensitive);
        QTimer::singleShot(30000, &loop2, &QEventLoop::quit);
        loop2.exec();
        out << "TextSearch query=\"" << query << "\" results=" << results.size() << "\n";
        if (done && results.isEmpty()) { out << "OCR_ACCEPT FAIL search empty\n"; out.flush(); PdfDocument::libRelease(); return 1; }
        {
            // Chieu cao highlight: tap tat ca rect tim duoc, do min/max/mean
            QVector<double> heights;
            for (const SearchResult& r : results)
                for (const QRectF& rc : r.rects)
                    heights << rc.height();
            if (!heights.isEmpty()) {
                double sum = 0, mn = 1e9, mx = -1e9;
                for (double h : heights) { sum += h; mn = qMin(mn, h); mx = qMax(mx, h); }
                const double mean = sum / heights.size();
                out << "Highlight heights: count=" << heights.size()
                    << " min=" << QString::number(mn, 'f', 2)
                    << " max=" << QString::number(mx, 'f', 2)
                    << " mean=" << QString::number(mean, 'f', 2)
                    << " (max/min ratio=" << QString::number(mx / qMax(0.0001, mn), 'f', 2) << ")\n";
            }
        }

        // ── 6) sha256 truoc/sau ──────────────────────────────────────────────
        auto sha = [](const QString& p) -> QString {
            QFile f(p);
            if (!f.open(QIODevice::ReadOnly)) return QString();
            QCryptographicHash h(QCryptographicHash::Sha256);
            while (!f.atEnd()) h.addData(f.read(1024 * 1024));
            return QString::fromLatin1(h.result().toHex());
        };
        const QString beforeSha = sha(pdfPath);
        const QString afterSha  = sha(pdfPath);
        out << "sha256 unchanged: " << (beforeSha == afterSha && !beforeSha.isEmpty() ? "YES" : "NO")
            << " (" << beforeSha.left(16) << "...)\n";

        out << "OCR_ACCEPT_OK\n";
        out.flush();
        PdfDocument::libRelease();
        return (beforeChars == 0 && afterChars > 0 && !results.isEmpty() && beforeSha == afterSha) ? 0 : 1;
    }

    // usage: --textsel-test <pdf> <page1Based> <x1> <y1> <x2> <y2>
    // Nghiem thu chon chu theo CHI SO KY TU (SPEC_TEXTSEL_ADOBE muc NGHIEM THU):
    // (x1,y1)-(x2,y2) la TOA DO HIEN THI (Y-down, goc trai tren, da ap /Rotate
    // + CropBox), giai lai nhan-keo-nha. In anchor/focus char, so ky tu, so
    // rect (1 rect = 1 dong), chu chon duoc, va word/line range tai diem dau.
    if (argc >= 8 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--textsel-test")) {
        QTextStream out(stdout);
        const QString pdfPath = QString::fromLocal8Bit(argv[2]);
        const int page = QString::fromLocal8Bit(argv[3]).toInt() - 1;
        const double x1 = QString::fromLocal8Bit(argv[4]).toDouble();
        const double y1 = QString::fromLocal8Bit(argv[5]).toDouble();
        const double x2 = QString::fromLocal8Bit(argv[6]).toDouble();
        const double y2 = QString::fromLocal8Bit(argv[7]).toDouble();

        PdfDocument::libAddRef();
        PdfDocument doc;
        if (!doc.open(pdfPath)) {
            out << "[textsel] FAIL cannot open " << pdfPath << "\n"; out.flush();
            PdfDocument::libRelease(); return 1;
        }
        const TextSelection::PageInfo info = TextSelection::pageFor(doc.raw(), page);
        if (!info.tp) {
            out << "[textsel] FAIL no text page (page=" << (page + 1) << ")\n"; out.flush();
            TextSelection::closeDocument(doc.raw()); PdfDocument::libRelease(); return 1;
        }
        const QPointF p1 = TextSelection::dispToPagePt(info, QPointF(x1, y1));
        const QPointF p2 = TextSelection::dispToPagePt(info, QPointF(x2, y2));
        const double tolX = 5.0, tolY = 5.0;   // zoom ~1 cho harness
        int a = TextSelection::charIndexAt(info.tp, p1.x(), p1.y(), tolX, tolY);
        int f = TextSelection::charIndexAt(info.tp, p2.x(), p2.y(), tolX, tolY);
        out << "[textsel] page=" << (page + 1) << " rot=" << info.rot
            << " anchorChar=" << a << " focusChar=" << f;
        if (a >= 0 && f < 0) f = a;
        if (a < 0 && f >= 0) a = f;
        if (a < 0 || f < 0) {
            out << " count=0 rects=0\n";
            out << "[textsel] text=\"\"\n";
            out.flush();
            TextSelection::closeDocument(doc.raw()); PdfDocument::libRelease();
            return 0;
        }
        if (f < a) qSwap(a, f);
        const int count = f - a + 1;
        const QVector<QRectF> rects = TextSelection::rectsForRangeDisp(info, a, count);
        const QString text = TextSelection::textForRange(info.tp, a, count);
        out << " count=" << count << " rects=" << rects.size() << "\n";
        out << "[textsel] text=\"" << text << "\"\n";
        for (int i = 0; i < rects.size(); ++i) {
            out << "[textsel] rect[" << i << "]="
                << QString::number(rects[i].x(), 'f', 2) << ","
                << QString::number(rects[i].y(), 'f', 2) << ","
                << QString::number(rects[i].width(), 'f', 2) << ","
                << QString::number(rects[i].height(), 'f', 2) << "\n";
        }
        // word/line range tai diem anchor (nghiem thu muc 5: chon dung TROM TU).
        {
            int ws = 0, wc = 0, ls = 0, lc = 0;
            TextSelection::wordRange(info.tp, a, &ws, &wc);
            TextSelection::lineRange(info.tp, a, &ls, &lc);
            const QString wtext = TextSelection::textForRange(info.tp, ws, wc);
            out << "[textsel] word[" << a << "]=" << ws << "," << wc
                << ",\"" << wtext << "\"\n";
            out << "[textsel] line[" << a << "]=" << ls << "," << lc << "\n";
        }
        out.flush();
        TextSelection::closeDocument(doc.raw());
        PdfDocument::libRelease();
        return 0;
    }

    // usage: --foreignbench <input.pdf>
    // Measure 3 approaches to render foreign annotation layer, choose cheapest.
    if (argc >= 3 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--foreignbench")) {
        QTextStream out(stdout);
        const QString inPath = QString::fromLocal8Bit(argv[2]);
        PdfDocument::libAddRef();

        FPDF_DOCUMENT doc = nullptr;
        { QMutexLocker lock(&s_pdfiumMutex); doc = FPDF_LoadDocument(inPath.toUtf8().constData(), nullptr); }
        if (!doc) { out << "FOREIGNBENCH: FAIL cannot open " << inPath << "\n"; out.flush(); PdfDocument::libRelease(); return 1; }

        // 2.1 Scan up to 30 pages, pick the one with most foreign annots
        const int pageCount = FPDF_GetPageCount(doc);
        const int limit = qMin(pageCount, 30);
        int bestPage = -1, bestCount = 0;
        double bestW = 0, bestH = 0;
        for (int i = 0; i < limit; ++i) {
            QMutexLocker lock(&s_pdfiumMutex);
            FPDF_PAGE pg = FPDF_LoadPage(doc, i);
            if (!pg) continue;
            double w = FPDF_GetPageWidth(pg), h = FPDF_GetPageHeight(pg);
            int n = FPDFPage_GetAnnotCount(pg), foreign = 0;
            for (int j = 0; j < n; ++j) {
                FPDF_ANNOTATION a = FPDFPage_GetAnnot(pg, j);
                if (!a) continue;
                int fl = FPDFAnnot_GetFlags(a);
                if (!(fl & FPDF_ANNOT_FLAG_HIDDEN) && FPDFAnnot_HasKey(a, "TRUID") == 0
                    && FPDFAnnot_GetSubtype(a) != FPDF_ANNOT_POPUP)
                    ++foreign;
                FPDFPage_CloseAnnot(a);
            }
            FPDF_ClosePage(pg);
            if (foreign > bestCount) { bestCount = foreign; bestPage = i; bestW = w; bestH = h; }
        }
        if (bestPage < 0) { out << "KHONG CO ANNOT NGOAI\n"; out.flush(); FPDF_CloseDocument(doc); PdfDocument::libRelease(); return 1; }

        int wPx, hPx;
        if (bestW >= bestH) { wPx = 2000; hPx = qMax(1, (int)(2000 * bestH / bestW)); }
        else                { hPx = 2000; wPx = qMax(1, (int)(2000 * bestW / bestH)); }
        out << "page=" << bestPage << " foreignAnnots=" << bestCount
            << " pageSize=" << bestW << "x" << bestH << "\n"; out.flush();

        // Render lambdas (require mutex held outside)
        auto renderTo = [&](FPDF_PAGE pg, int flags) -> QImage {
            QImage img(wPx, hPx, QImage::Format_ARGB32);
            img.fill(Qt::white);
            FPDF_BITMAP bmp = FPDFBitmap_CreateEx(wPx, hPx, FPDFBitmap_BGRA, img.bits(), img.bytesPerLine());
            if (bmp) { FPDFBitmap_FillRect(bmp, 0, 0, wPx, hPx, 0xFFFFFFFF);
                       FPDF_RenderPageBitmap(bmp, pg, 0, 0, wPx, hPx, 0, flags); FPDFBitmap_Destroy(bmp); }
            return img;
        };
        auto hideOurs = [&](FPDF_PAGE pg) -> QVector<int> {
            QVector<int> hid; int n = FPDFPage_GetAnnotCount(pg);
            for (int i = 0; i < n; ++i) {
                FPDF_ANNOTATION a = FPDFPage_GetAnnot(pg, i);
                if (!a) continue;
                if (FPDFAnnot_HasKey(a, "TRUID")) {
                    int f = FPDFAnnot_GetFlags(a);
                    if (!(f & FPDF_ANNOT_FLAG_HIDDEN)) { FPDFAnnot_SetFlags(a, f | FPDF_ANNOT_FLAG_HIDDEN); hid.append(i); }
                }
                FPDFPage_CloseAnnot(a);
            }
            return hid;
        };
        auto restoreOurs = [&](FPDF_PAGE pg, const QVector<int>& idxs) {
            for (int i : idxs) {
                FPDF_ANNOTATION a = FPDFPage_GetAnnot(pg, i);
                if (!a) continue;
                FPDFAnnot_SetFlags(a, FPDFAnnot_GetFlags(a) & ~FPDF_ANNOT_FLAG_HIDDEN);
                FPDFPage_CloseAnnot(a);
            }
        };

        const int RUNS = 3;
        qint64 aTimes[RUNS], bTimes[RUNS], cAnnotTimes[RUNS], cPlainTimes[RUNS];
        QImage benchALayer, benchAPlain, benchBLayer, benchCFull;
        int aDiffPixels = 0, bNonTransparent = 0, bFormType = -1;
        bool bFormOk = false;

        for (int r = 0; r < RUNS; ++r) {
            // Method A: current approach — double render + diff
            {
                QMutexLocker lock(&s_pdfiumMutex);
                FPDF_PAGE page = FPDF_LoadPage(doc, bestPage);
                if (!page) { out << "FOREIGNBENCH: FAIL load page\n"; out.flush(); FPDF_CloseDocument(doc); PdfDocument::libRelease(); return 1; }
                QElapsedTimer t; t.start();
                auto hid = hideOurs(page);
                QImage plain = renderTo(page, 0);
                QImage annot = renderTo(page, FPDF_ANNOT);
                restoreOurs(page, hid);
                aTimes[r] = t.elapsed();
                FPDF_ClosePage(page);
                // Diff outside mutex
                QImage layer(wPx, hPx, QImage::Format_ARGB32);
                layer.fill(Qt::transparent);
                int dc = 0;
                for (int y = 0; y < hPx; ++y) {
                    const QRgb* pa = (const QRgb*)plain.constScanLine(y);
                    const QRgb* pb = (const QRgb*)annot.constScanLine(y);
                    QRgb* po = (QRgb*)layer.scanLine(y);
                    for (int x = 0; x < wPx; ++x) {
                        if ((pa[x] & 0x00FFFFFF) != (pb[x] & 0x00FFFFFF)) { po[x] = (pb[x] | 0xFF000000); ++dc; }
                    }
                }
                if (r == 0) { benchALayer = layer; benchAPlain = plain; aDiffPixels = dc; }
            }

            // Method B: FFLDraw on transparent bitmap (no page render)
            {
                QMutexLocker lock(&s_pdfiumMutex);
                FPDF_PAGE page = FPDF_LoadPage(doc, bestPage);
                if (!page) continue;
                QImage img(wPx, hPx, QImage::Format_ARGB32);
                img.fill(Qt::transparent);
                FPDF_BITMAP bmp = FPDFBitmap_CreateEx(wPx, hPx, FPDFBitmap_BGRA, img.bits(), img.bytesPerLine());
                if (bmp) FPDFBitmap_FillRect(bmp, 0, 0, wPx, hPx, 0x00000000);
                FPDF_FORMFILLINFO ffi; memset(&ffi, 0, sizeof(ffi)); ffi.version = 2;
                FPDF_FORMHANDLE form = FPDFDOC_InitFormFillEnvironment(doc, &ffi);
                if (r == 0) { bFormOk = (form != nullptr); bFormType = FPDF_GetFormType(doc); }
                QElapsedTimer t; t.start();
                if (form) {
                    FORM_OnAfterLoadPage(page, form);
                    FPDF_FFLDraw(form, bmp, page, 0, 0, wPx, hPx, 0, FPDF_ANNOT);
                    FORM_OnBeforeClosePage(page, form);
                    FPDFDOC_ExitFormFillEnvironment(form);
                }
                bTimes[r] = t.elapsed();
                FPDFBitmap_Destroy(bmp);
                FPDF_ClosePage(page);
                if (r == 0) {
                    benchBLayer = img; int ntp = 0;
                    for (int y = 0; y < hPx; ++y) {
                        const QRgb* row = (const QRgb*)img.constScanLine(y);
                        for (int x = 0; x < wPx; ++x) if (qAlpha(row[x]) != 0) ++ntp;
                    }
                    bNonTransparent = ntp;
                }
            }

            // Method C: single render with FPDF_ANNOT (TRUID hidden) + measure plain overhead
            {
                QMutexLocker lock(&s_pdfiumMutex);
                FPDF_PAGE page = FPDF_LoadPage(doc, bestPage);
                if (!page) continue;
                auto hid = hideOurs(page);
                QElapsedTimer t; t.start();
                QImage annot = renderTo(page, FPDF_ANNOT);
                qint64 t1 = t.elapsed();
                t.restart();
                QImage plain = renderTo(page, 0);
                cPlainTimes[r] = t.elapsed();
                cAnnotTimes[r] = t1;
                restoreOurs(page, hid);
                FPDF_ClosePage(page);
                if (r == 0) benchCFull = annot;
            }
        }

        // Averages
        double avgA=0, avgB=0, avgCAnnot=0, avgCPlain=0;
        for (int i = 0; i < RUNS; ++i) { avgA += aTimes[i]; avgB += bTimes[i]; avgCAnnot += cAnnotTimes[i]; avgCPlain += cPlainTimes[i]; }
        avgA /= RUNS; avgB /= RUNS; avgCAnnot /= RUNS; avgCPlain /= RUNS;
        double overheadPct = avgCPlain > 0 ? ((avgCAnnot - avgCPlain) / avgCPlain * 100.0) : 0.0;

        // Save PNGs
        benchAPlain.save("bench_A_plain.png");
        benchALayer.save("bench_A_layer.png");
        benchBLayer.save("bench_B_layer.png");
        benchCFull.save("bench_C_full.png");

        out << "A: ms=" << QString::number(avgA, 'f', 1) << " diffPixels=" << aDiffPixels << "\n";
        out << "B: ms=" << QString::number(avgB, 'f', 1) << " nonTransparentPixels=" << bNonTransparent
            << " formHandle=" << (bFormOk ? "ok" : "null") << " formType=" << bFormType << "\n";
        out << "C: ms_annot=" << QString::number(avgCAnnot, 'f', 1)
            << " ms_plain=" << QString::number(avgCPlain, 'f', 1)
            << " overhead=" << QString::number(overheadPct, 'f', 1) << "%\n";
        out << "KET LUAN:\n"
            << "  A (hien tai) = " << QString::number(avgA, 'f', 1) << "\n"
            << "  B (FFLDraw)  = " << QString::number(avgB, 'f', 1)
            << "  -> co ve duoc annot ngoai khong: " << (bNonTransparent > 1000 ? "CO" : "KHONG") << "\n"
            << "  C (1 render) = " << QString::number(avgCAnnot, 'f', 1)
            << "  -> dat them " << QString::number(overheadPct, 'f', 1) << "% so voi render tran\n";
        out.flush();
        FPDF_CloseDocument(doc);
        PdfDocument::libRelease();
        return 0;
    }

    // usage: --pageflip-bench <input.pdf> <p1_1based> <p2_1based> <lan>
    // Harness lat trang (SPEC_PERF_DESK_ABOUT phan 1): lat qua lai giua p1 va p2
    // `lan` lau qua DUNG onPageChanged, in thoi gian tung lan doi trang.
    if (argc >= 6 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--pageflip-bench")) {
        const QString inputPath = QString::fromLocal8Bit(argv[2]);
        const int p1 = QString::fromLocal8Bit(argv[3]).toInt() - 1;
        const int p2 = QString::fromLocal8Bit(argv[4]).toInt() - 1;
        const int loops = QString::fromLocal8Bit(argv[5]).toInt();
        MainWindow w;
        w.resize(1400, 900);
        w.show();
        QCoreApplication::processEvents();
        w.openFile(inputPath);
        w.probeFlipBench(p1, p2, loops);
        return 0;
    }

    // usage: --flagbench <input.pdf> <page_1based>
    // Benchmark: render one page with 5 flag combos, 3 runs each, report avg ms + save PNGs.
    if (argc >= 4 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--flagbench")) {
        QString inputPath = QString::fromLocal8Bit(argv[2]);
        int pageIdx = QString::fromLocal8Bit(argv[3]).toInt() - 1; // 1-based → 0-based
        QTextStream out(stdout);

        PdfDocument::libAddRef();

        FPDF_DOCUMENT doc = FPDF_LoadDocument(inputPath.toUtf8().constData(), nullptr);
        if (!doc) {
            out << "FLAGBENCH: FAIL cannot open " << inputPath << "\n"; out.flush();
            PdfDocument::libRelease(); return 1;
        }
        int pageCount = FPDF_GetPageCount(doc);
        if (pageIdx < 0 || pageIdx >= pageCount) {
            out << "FLAGBENCH: FAIL page " << (pageIdx + 1) << " out of range (pages=" << pageCount << ")\n"; out.flush();
            FPDF_CloseDocument(doc); PdfDocument::libRelease(); return 1;
        }

        // Load page ONCE, reuse for all renders
        QMutexLocker lock(&s_pdfiumMutex);
        FPDF_PAGE page = FPDF_LoadPage(doc, pageIdx);
        if (!page) {
            out << "FLAGBENCH: FAIL cannot load page " << (pageIdx + 1) << "\n"; out.flush();
            FPDF_CloseDocument(doc); PdfDocument::libRelease(); return 1;
        }
        lock.unlock();

        double pageW = FPDF_GetPageWidth(page);
        double pageH = FPDF_GetPageHeight(page);
        int W = 2000;
        int H = static_cast<int>(pageW > 0 ? (pageH / pageW * W) : 2000);
        if (H < 1) H = 1;

        // 5 flag combinations
        struct Combo { const char label; const char* name; int flags; };
        // ponytail: FPDF_RENDER_NO_NATIVETEXT = 0x800 (not in this pdfium header)
        const int NO_NATIVETEXT = 0x800;
        const Combo combos[] = {
            {'A', "FPDF_ANNOT|LIMITEDIMAGECACHE",              FPDF_ANNOT | FPDF_RENDER_LIMITEDIMAGECACHE},
            {'B', "A|NO_SMOOTHPATH",                           FPDF_ANNOT | FPDF_RENDER_LIMITEDIMAGECACHE | FPDF_RENDER_NO_SMOOTHPATH},
            {'C', "A|NO_SMOOTHPATH|NO_SMOOTHTEXT|NO_SMOOTHIMAGE", FPDF_ANNOT | FPDF_RENDER_LIMITEDIMAGECACHE | FPDF_RENDER_NO_SMOOTHPATH | FPDF_RENDER_NO_SMOOTHTEXT | FPDF_RENDER_NO_SMOOTHIMAGE},
            {'D', "FPDF_ANNOT (no LIMITEDIMAGECACHE)",         FPDF_ANNOT},
            {'E', "A|LIMITEDIMAGECACHE|NO_NATIVETEXT",         FPDF_ANNOT | FPDF_RENDER_LIMITEDIMAGECACHE | NO_NATIVETEXT},
        };

        double avgMs[5] = {};
        QString outDir = QDir::currentPath();

        for (int c = 0; c < 5; ++c) {
            qint64 total = 0;
            for (int r = 0; r < 3; ++r) {
                QImage img(W, H, QImage::Format_ARGB32);
                img.fill(Qt::white);
                QMutexLocker lk(&s_pdfiumMutex);
                FPDF_BITMAP bmp = FPDFBitmap_CreateEx(W, H, FPDFBitmap_BGRA,
                                                      img.bits(), img.bytesPerLine());
                QElapsedTimer t; t.start();
                FPDF_RenderPageBitmap(bmp, page, 0, 0, W, H, 0, combos[c].flags);
                qint64 ms = t.elapsed();
                FPDFBitmap_Destroy(bmp);
                lk.unlock();
                total += ms;
                if (r == 0) {
                    QString fn = outDir + QString("/flag_%1.png").arg(combos[c].label);
                    img.save(fn, "PNG");
                }
            }
            avgMs[c] = static_cast<double>(total) / 3.0;
            out << combos[c].label << ": ms=" << QString::number(avgMs[c], 'f', 1) << "\n";
        }

        // Find fastest
        int fastest = 0;
        for (int i = 1; i < 5; ++i)
            if (avgMs[i] < avgMs[fastest]) fastest = i;
        double pctSaved = avgMs[0] > 0 ? ((avgMs[0] - avgMs[fastest]) / avgMs[0] * 100.0) : 0.0;
        out << "KET LUAN: nhanh nhat=" << combos[fastest].label
            << " giam " << QString::number(pctSaved, 'f', 1) << "% so voi A\n";
        out.flush();

        FPDF_ClosePage(page);
        FPDF_CloseDocument(doc);
        PdfDocument::libRelease();
        return 0;
    }

    MainWindow window;
    window.setWindowTitle("TorReader PDF");
    window.resize(1280, 800);
    window.show();

    // Open file passed via command line (e.g. drag-to-exe)
    if (argc > 1)
        window.openFile(QString::fromLocal8Bit(argv[1]));
#endif

    return app.exec();
}
