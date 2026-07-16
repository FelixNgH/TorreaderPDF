#include <QApplication>
#include <QStyleFactory>
#include <QThreadPool>
#include <QThread>
#include <QFontDatabase>
#include <QFile>
#include <QTextStream>
#include <QDateTime>
#include <QMutex>
#include "ui/MainWindow.h"

static QFile   g_logFile;
static QMutex  g_logMutex;

static void logHandler(QtMsgType type, const QMessageLogContext&, const QString& msg) {
    if (!msg.startsWith("[")) return; // chỉ log message có prefix [ của chúng ta
    QMutexLocker lk(&g_logMutex);
    if (!g_logFile.isOpen()) return;
    const char* level = (type == QtWarningMsg) ? "WARN"
                      : (type == QtCriticalMsg) ? "CRIT"
                      : (type == QtFatalMsg)    ? "FATAL" : "DBG";
    QTextStream ts(&g_logFile);
    ts << QDateTime::currentDateTime().toString("hh:mm:ss.zzz")
       << " [" << level << "] " << msg << "\n";
    g_logFile.flush();
}

int main(int argc, char* argv[]) {
    g_logFile.setFileName("C:/temp/torreader_debug.txt");
    g_logFile.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate);
    qInstallMessageHandler(logHandler);
    QApplication::setHighDpiScaleFactorRoundingPolicy(
        Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);

    QApplication app(argc, argv);
    app.setApplicationName("TorReader PDF");
    app.setApplicationVersion("2.0.0");
    app.setOrganizationName("Loc Nguyen Huy");
    app.setOrganizationDomain("torreader.cloud");

    // Use all available cores for PDF rendering
    QThreadPool::globalInstance()->setMaxThreadCount(
        qMax(4, QThread::idealThreadCount()));

    // Use Fusion style for consistent cross-platform look
    app.setStyle(QStyleFactory::create("Fusion"));

    // Prefer open-source fonts; fall back gracefully to system fonts.
    // To bundle Noto Sans: add NotoSans-Regular.ttf to resources and call
    // QFontDatabase::addApplicationFont(":/fonts/NotoSans-Regular.ttf");
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

    MainWindow window;
    window.setWindowTitle("TorReader");
    window.resize(1280, 800);
    window.show();

    // Open file passed via command line (e.g. drag-to-exe)
    if (argc > 1)
        window.openFile(QString::fromLocal8Bit(argv[1]));

    return app.exec();
}
