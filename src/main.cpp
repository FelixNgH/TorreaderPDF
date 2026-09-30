#include <QApplication>
#include <QEvent>
#include <QMessageBox>
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
#include <memory>
#include <vector>
#ifndef TORREADER_NO_PDFIUM
#include "ui/MainWindow.h"
#include "ui/ContinuousView.h"
#include "ui/AboutDialog.h"
#include "ui/ThemeTokens.h"
#include "ui/ThumbnailPanel.h"
#include "ui/LoadingBadge.h"
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
#include "core/PageCache.h"
#include "core/PdfiumLock.h"
#include "core/Bisect.h"
#include "core/FastExit.h"
#include "annotations/AnnotationManager.h"
#include "annotations/AnnotationLayer.h"
#include "annotations/AnnotationTypes.h"
#include "core/ThumbnailRenderPool.h"
#include "core/TileCacheFile.h"
#include "core/VectorCacheFile.h"
#include <fpdfview.h>
#include <fpdf_edit.h>
#include <fpdf_save.h>
#include <fpdf_annot.h>
#include <fpdf_progressive.h>
#include <fpdf_text.h>
#include <fpdf_formfill.h>
#include <QPdfWriter>
#include <QPainter>
#include <QFont>
#include <QFontMetricsF>
#include <QPageLayout>
#include <QPageSize>
extern QMutex s_pdfiumMutex;
#endif
#ifdef _WIN32
#include <psapi.h>
#endif
#ifdef Q_OS_WIN
#include "core/VkPacketInputFilter.h"
// 0927 LƯỢT 11: TerminateProcess/GetCurrentProcess cho đường thoát nhanh.
#include <windows.h>
#include <heapapi.h>   // 🔴 LƯỢT 28: PROCESS_HEAP_SUMMARY/HeapSummary/GetProcessHeaps (LEAN_AND_MEAN khong nap heapapi)
#endif
#ifndef Q_OS_WIN
#include <unistd.h>   // 0927 LƯỢT 11: _exit() cho đường thoát nhanh
#endif
#include <QCoreApplication>
#include <QRandomGenerator>
#include <QEventLoop>
#include <QDir>
#include <QImage>
#include <QImageWriter>
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
#include <QPlainTextEdit>
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
    // 🔴 LƯỢT 34 (muc 1 — DO): gan nhan chu THE cho moi dong khi TORREADER_GUILOCK=1 de
    // tim TAN GO cho UI dung hinh: khoa = khoang trong lon nhat giua HAI dong [UI] lien tiep.
    static const bool kTagThread = !qEnvironmentVariableIsEmpty("TORREADER_GUILOCK");
    static const QThread* kMainThr = []{
        return QCoreApplication::instance() ? QCoreApplication::instance()->thread() : nullptr;
    }();
    const bool isMain = kTagThread && (QThread::currentThread() == kMainThr);
    {
        QMutexLocker lk(&g_logMutex);
        if (g_logFile.isOpen()) {
            QByteArray line = QDateTime::currentDateTime().toString("hh:mm:ss.zzz").toUtf8();
            line += " [";
            line += level;
            line += "] ";
            if (kTagThread) line += isMain ? "[UI] " : "[bg] ";
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

// ── 0927 LƯỢT 11 — THOÁT NHANH (vá tạm) ──────────────────────────────────────
// Crash 0x80000003 lúc thoát nằm trong ~MainWindow (UI → PageCache::closeEntry →
// FPDF_ClosePage), ~50% lượt với file CAD nặng. Gốc chưa rõ ⇒ vá tạm ở ĐƯỜNG THOÁT:
// gọi đúng MỘT chỗ sau mỗi `app.exec()` của ĐƯỜNG APP THẬT, huỷ tiến trình thay vì
// để chạy hàm huỷ. Chi tiết + lý do: REPORT_CRASH_EYACHO_0927.md §"LƯỢT 11".
//
// 🔴 HỢP ĐỒNG (không được phá):
//   1. Chỉ gọi SAU `app.exec()` — mọi việc người dùng cần nằm trong closeEvent và
//      nó đã chạy xong trước lúc exec() trả về (closeEvent tự báo qua trExitReady).
//   2. Không đụng đường ĐÓNG TAB (app vẫn chạy) — hàm này chỉ ở cuối vòng đời app.
//   3. Bật `--thoat-cham` (xem Bisect.h) = đường CŨ, để CEO tái hiện crash.
static void trThoatNhanh(int rc, MainWindow* w) {
    if (trThoatCham()) {
        qDebug().noquote() << QStringLiteral("[thoat] duongCu rc=%1 lyDo=thoatCham").arg(rc);
        return;   // đường cũ: chạy đủ hàm huỷ, crash 0x80000003 quay lại nếu có
    }
    if (!trExitReady()) {
        // Không đi qua closeEvent (qApp->quit() ở GateDialog, probe tự quit) ⇒
        // KHÔNG chắc việc bắt buộc đã xong ⇒ không giết, đi tiếp như cũ.
        qDebug().noquote() << QStringLiteral("[thoat] duongCu rc=%1 lyDo=closeEventChuaXong").arg(rc);
        return;
    }
    QSettings().sync();              // đẩy hết ghi QSettings còn treo (token, ngôn ngữ, ocr/root)
    const int left = w ? w->removeDraftsForExit() : 0;
    if (left > 0)
        qWarning().noquote() << QStringLiteral("[thoat] banNhapConLai=%1 (handle PDFium con mo)").arg(left);
    const qint64 sauDon = trExitDoneClock().isValid() ? trExitDoneClock().elapsed() : -1;
    qDebug().noquote() << QStringLiteral("[thoat] nhanh rc=%1 sauDonMs=%2").arg(rc).arg(sauDon);
    fflush(nullptr);                 // stdout + stderr (probe in ra đây) — TRƯỚC khi giết
    g_logFile.flush();
    g_logFile.close();
#ifdef Q_OS_WIN
    if (!::TerminateProcess(::GetCurrentProcess(), static_cast<UINT>(rc))) ::_exit(rc);
#else
    _exit(rc);
#endif
}
#endif

// 🔴 LƯỢT 28 (VIỆC 1a — heap KHỐI LỚN): 2 000 lan malloc/free kich co 64 KB–4 MB,
// SEED CO DINH (moi lan do cung mot day kich thuoc). LFH chi phuc vu khoi ≤16 KB;
// khoi lon di free-list cua process heap (pdfium.dll dung CRT/ucrt malloc =
// GetProcessHeap). hp_big_ns TANG THEO LAP ⇒ gia thuyet CEO dung: phan manh khoi
// lon (QImage cache + bitmap + path arrays churn) lam moi cap phat lon cham dan.
// Nhat: chinh day malloc cung phan manh them heap — bang nhau giua cac lan do thi
// so VAN so duoc voi nhau. Linux tra -1 (dung pfCount L25 — mang Linux bien dich).
static long long heapBigNs() {
#ifdef Q_OS_WIN
    QElapsedTimer t; t.start();
    unsigned x = 20260929u;
    for (int i = 0; i < 2000; ++i) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        const size_t sz = 65536 + (x % (4u * 1024u * 1024u - 65536u));
        void* p = malloc(sz);
        if (p) { *static_cast<volatile char*>(p) = 1; free(p); }
    }
    return t.nsecsElapsed();
#else
    return -1;
#endif
}
// Dem heap (GetProcessHeaps) + HeapWalk cua GetProcessHeap: so block FREE + tong
// bytes free (cap 200k entry — probe-only; LFH an block nho trong HeapWalk nen
// con lai chu yeu khoi lon; nen khac van cap phat song song ⇒ so xap xi, XU HU la
// du). Segment Heap khong ho tro HeapWalk ⇒ -1 la tin hieu manifest da noi (tren
// Win11 — may do Win10 19044, xem REPORT).
static void heapSummary(long long& nHeaps, long long& freeKB, long long& freeBlocks) {
    nHeaps = -1; freeKB = -1; freeBlocks = -1;
#ifdef Q_OS_WIN
    nHeaps = GetProcessHeaps(0, nullptr);
    PROCESS_HEAP_ENTRY e{};
    long long fb = 0, fk = 0, seen = 0;
    const HANDLE h = GetProcessHeap();
    while (seen < 2000000 && HeapWalk(h, &e)) {   // tran 2M entry — buoc chay co thoi han
        ++seen;
        if (!(e.wFlags & (PROCESS_HEAP_REGION | PROCESS_HEAP_UNCOMMITTED_RANGE |
                          PROCESS_HEAP_ENTRY_BUSY)) && e.cbData) {
            ++fb; fk += (long long)e.cbData / 1024;
        }
    }
    freeKB = fk; freeBlocks = fb;
#endif
}

// 🔴 LƯỢT 34 (muc 1 — DO): [slotms] tong quat tren moi lan phat su kien o luong GIAO DIEN.
// Bao cao MỌI handler cua UI thread chay > 100 ms (ke ca paintEvent, timer, slot cua signal
// queued) kem ten doi tuon nhan + loai su kien. Chi bat khi TORREADER_GUILOCK=1 (che do DO),
// mac dinh tat va khong them chi phi. Day la thu duy nhat bat duoc cho UI dung >100ms MA
// KHONG phai do doi khoa pdfium (ca [guilock]/[lockwait] deu ~0 trong cua so zoom 200%).
class TrProbeApp : public QApplication {
public:
    using QApplication::QApplication;
    static const char* evName(int t) {
        switch (t) {
        case QEvent::Timer: return "Timer";
        case QEvent::Paint: return "Paint";
        case QEvent::Show: return "Show";
        case QEvent::Resize: return "Resize";
        case QEvent::UpdateRequest: return "UpdateRequest";
        case QEvent::MouseMove: return "MouseMove";
        case QEvent::MouseButtonPress: return "MouseButtonPress";
        case QEvent::Wheel: return "Wheel";
        default: return "ev";
        }
    }
    bool notify(QObject* recv, QEvent* ev) override {
        static const bool audit = !qEnvironmentVariableIsEmpty("TORREADER_GUILOCK");
        if (!audit) return QApplication::notify(recv, ev);
        QElapsedTimer t; t.start();
        const bool r = QApplication::notify(recv, ev);
        const qint64 ms = t.elapsed();
        if (ms > 100)
            qDebug().noquote() << "[slotms] notify" << (recv ? recv->metaObject()->className() : "?")
                               << evName(int(ev->type())) << "ms=" << ms;
        return r;
    }
};

// 🔴 LƯỢT 36 (mục 0) — THOÁT PROBE SẠCH, chặn MỘT choke point:
// Mọi chế độ `--*-probe`/`--*-bench` `return` khỏi main trong khi task nền (render
// pool, closeJob) VẪN còn chạy ⇒ destructor tĩnh chạy giữa lúc worker đang giữ
// khoá ⇒ văng (bằng chứng 15:32: --welcome-probe Qt6Core ← TimedPdfiumLock ←
// ProgressiveRenderTask::run PdfRenderer.cpp:590). Nay main() là wrapper: đường
// probe kết thúc bằng flush + TerminateProcess (ĐÚNG đường thoát nhanh của
// trThoatNhanh) — không chạy bất kỳ hàm huỷ nào. Đường APP THẬT (cờ dưới) KHÔNG
// đổi: vẫn app.exec() → trThoatNhanh, --thoat-cham vẫn về đường huỷ cũ.
static bool g_trRealAppPath = false;

static int trMainImpl(int argc, char* argv[]) {
    // 0927 LƯỢT 12 — CỜ NGƯỢC `--pool-song-par` (thay `--pool-lock` của LƯỢT 9).
    // Mặc định KHÔNG có cờ ⇒ KHOÁ PDFium là mặc định vĩnh viễn (xem core/PdfiumLock.h
    // để biết vì sao: ASan heap-use-after-free trên ColorSpace toàn cục + TSan 229
    // tranh chấp dính pdfium ⇒ pdfium KHÔNG thread-safe kể cả khác FPDF_DOCUMENT).
    // Cờ này CHỈ để CEO tái hiện lại đường KHÔNG khoá đã biết là hỏng. KHÔNG bật ở
    // bản phát hành.
    {
        int nFlags = 0;
        const TrBisectFlag* flags = trBisectFlags(&nFlags);
        for (int i = 1; i < argc; ) {
            const QString a = QString::fromLocal8Bit(argv[i]);
            if (a == QLatin1String("--pool-lock")) {
                // Cờ cũ đã bị gỡ — cảnh báo thay vì im lặng để không ai tưởng còn hiệu lực.
                qWarning("[khoachung] --pool-lock da bi GO BO: khoa PDFium la mac dinh. "
                         "Dung --pool-song-par neu can tai hien loi.");
            } else if (a == QLatin1String("--pool-song-par")) {
                qWarning("[khoachung] BA CHAY CHE DO --pool-song-par: PDFium khong khoa "
                         "=> biet truoc se dung 0x80000003 / heap 0xc0000374.");
                qputenv("TORREADER_POOL_SONGPAR", "1");
                for (int j = i; j + 1 < argc; ++j) argv[j] = argv[j + 1];
                --argc;   // không bỏ argv[argc] — phần còn lại không đọc tới
                continue;
            } else {
                bool matched = false;
                for (int f = 0; f < nFlags; ++f) {
                    if (a != QLatin1String(flags[f].cli)) continue;
                    qputenv(flags[f].env, "1");
                    for (int j = i; j + 1 < argc; ++j) argv[j] = argv[j + 1];
                    --argc;
                    matched = true;
                    break;
                }
                if (!matched) ++i;
                continue;
            }
            for (int j = i; j + 1 < argc; ++j) argv[j] = argv[j + 1];
            --argc;   // --pool-lock: bỏ khỏi argv như các cờ khác (không để lọc 2 lần)
        }
    }

    QApplication::setHighDpiScaleFactorRoundingPolicy(
        Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);

    TrProbeApp app(argc, argv);
    app.setApplicationName("TorReader PDF");
    app.setApplicationVersion(FELIXPDF_VERSION);
    app.setOrganizationName("Loc Nguyen Huy");
    app.setOrganizationDomain("torreader.cloud");

#ifdef Q_OS_WIN
    // 0903: Go tieng Viet UniKey (VK_PACKET) mat ky tu >255 — xem core/VkPacketInputFilter.h
    static VkPacketInputFilter g_vkPacketFilter;
    app.installNativeEventFilter(&g_vkPacketFilter);
#endif

    // Do-luong: bat cac lan luong chinh bi chan > nguong. Chi bat khi co bien moi truong.
    if (!qEnvironmentVariableIsEmpty("TORREADER_STALL_WATCH")) {
        bool okThr = false;
        int thrMs = qEnvironmentVariable("TORREADER_STALL_WATCH").toInt(&okThr);
        if (!okThr || thrMs < 50) thrMs = 300;
        auto* stallTimer = new QTimer(&app);
        auto lastTick = std::make_shared<QElapsedTimer>();
        lastTick->start();
        QObject::connect(stallTimer, &QTimer::timeout, &app, [lastTick, thrMs]() {
            const qint64 gap = lastTick->restart();
            if (gap > thrMs)
                qDebug().noquote() << "[stall] main thread blocked ms=" << gap;
        });
        stallTimer->start(20);
        qDebug().noquote() << "[stall] watchdog ON threshold=" << thrMs << "ms";
    }

    // Ngay sau QApplication, truoc MainWindow: bat het log ra file.
    installTextLog();
    qDebug() << "[gate] app version =" << FELIXPDF_VERSION;

#ifndef TORREADER_NO_PDFIUM

    // 0927 LƯỢT 12: dòng XÁC NHẬN. Gọi hàm ở đây để nó đọc biến ĐÚNG MỘT LẦN ngay
    // lúc khởi động, trước khi luồng nền gọi tới. bat=1 là HÀNH VI ĐÚNG.
    qDebug().noquote() << "[poollock] bat=" << (trPoolLockOn() ? 1 : 0)
                       << (trPoolLockOn() ? "(mac dinh — MOI loi goi PDFium deu khoa)"
                                          : "(!! --pool-song-par — TAI HIEN LOI, khong phat hanh !!)");

    // 0927 LƯỢT 10: MỘT dòng `[bisect] <ten>=1` cho MỖI công tắc đang BẬT (tên = cờ
    // bỏ `--`). Không có dòng nào = mặc định (hành vi hiện tại) = lượt chạy vô nghĩa.
    {
        int nFlags = 0;
        const TrBisectFlag* flags = trBisectFlags(&nFlags);
        for (int f = 0; f < nFlags; ++f) {
            if (!qEnvironmentVariableIsEmpty(flags[f].env)) continue;
            qDebug().noquote() << "[bisect]" << flags[f].cli + 2 << "=1";   // bỏ "--"
        }
    }

    // 📐 NAC 1 (0921): doi chung pixel — sua FreeText NGOAI bang DUNG CHU CU roi
    // ghi /AP moi qua QPDF. usage: --ftngoai-ap <in.pdf> <page> <annotIndex> <out.pdf>
    if (argc >= 6 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--ftngoai-ap")) {
        QString inPath = QString::fromLocal8Bit(argv[2]);
        int pageIndex = QString::fromLocal8Bit(argv[3]).toInt();
        int annotIndex = QString::fromLocal8Bit(argv[4]).toInt();
        QString outPath = QString::fromLocal8Bit(argv[5]);
        QTextStream out(stdout);
        QFile::remove(outPath);
        PdfDocument::libAddRef();
        PdfDocument doc;
        if (!doc.open(inPath)) { out << "FTAP: FAIL open\n"; out.flush(); PdfDocument::libRelease(); return 1; }
        QString oldText;
        {
            QMutexLocker lock(&s_pdfiumMutex);
            FPDF_PAGE p = FPDF_LoadPage(doc.raw(), pageIndex);
            FPDF_ANNOTATION a = FPDFPage_GetAnnot(p, annotIndex);
            unsigned long n = FPDFAnnot_GetStringValue(a, "Contents", nullptr, 0);
            if (n > 2) {
                std::vector<char16_t> b(n/2+1, 0);
                FPDFAnnot_GetStringValue(a, "Contents", (FPDF_WCHAR*)b.data(), n);
                oldText = QString::fromUtf16(b.data());
            }
            FPDFPage_CloseAnnot(a); FPDF_ClosePage(p);
        }
        // argv[6] (tuy chon): chu MOI (UTF-8). Thieu ⇒ giu DUNG chu cu.
        const QString newText = (argc >= 7) ? QString::fromUtf8(argv[6]) : oldText;
        out << "FTAP: oldText=[" << oldText << "]\n";
        out << "FTAP: newText=[" << newText << "]\n"; out.flush();
        {
            AnnotationManager mgr;
            mgr.setDocument(doc.raw(), outPath);
            mgr.setAllowForeignFreeTextEdit(true);   // harness do — GUI KHONG bat
            bool ok = mgr.retextNote(pageIndex, annotIndex, newText);
            out << "FTAP: retextNote=" << (ok?"true":"false") << "\n";
            bool sv = mgr.saveDocument();
            out << "FTAP: save=" << (sv?"true":"false") << " err=" << mgr.lastError() << "\n";
        }
        out.flush();
        PdfDocument::libRelease();
        return 0;
    }

    // 🔴 P1 (0921): thu DUONG HOAN TAC. usage: --ftap-restore <in.pdf> <page> <idx> <out.pdf>
    // in.pdf phai la ban DA VA (co /TR_AP_ORIG). Dat lai /AP/N tu /TR_AP_ORIG roi xoa khoa.
    if (argc >= 6 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--ftap-restore")) {
        QString inPath = QString::fromLocal8Bit(argv[2]);
        int pageIndex = QString::fromLocal8Bit(argv[3]).toInt();
        int annotIndex = QString::fromLocal8Bit(argv[4]).toInt();
        QString outPath = QString::fromLocal8Bit(argv[5]);
        QTextStream out(stdout);
        QFile::remove(outPath);
        if (!QFile::copy(inPath, outPath)) { out << "FTAPR: FAIL copy\n"; out.flush(); return 1; }
        PdfDocument::libAddRef();
        PdfDocument doc;
        if (!doc.open(inPath)) { out << "FTAPR: FAIL open\n"; out.flush(); PdfDocument::libRelease(); return 1; }
        {
            AnnotationManager mgr;
            mgr.setDocument(doc.raw(), outPath);
            bool ok = mgr.restoreForeignAp(pageIndex, annotIndex);
            out << "FTAPR: restore=" << (ok?"true":"false") << " err=" << mgr.lastError() << "\n";
        }
        out.flush();
        PdfDocument::libRelease();
        return 0;
    }

    // 🔴 P0 (0921): thu chot so huu setAnnotStyle.
    // usage: --ftap-style <in.pdf> <page> <idx> <out.pdf> [--own]
    //   (khong --own) : annot NGOAI  → phai tra false, /AP khong doi mot byte.
    //   --own         : gan TRUID truoc → annot CUA TA → phai tra true (doi mau duoc).
    if (argc >= 6 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--ftap-style")) {
        QString inPath = QString::fromLocal8Bit(argv[2]);
        int pageIndex = QString::fromLocal8Bit(argv[3]).toInt();
        int annotIndex = QString::fromLocal8Bit(argv[4]).toInt();
        QString outPath = QString::fromLocal8Bit(argv[5]);
        const bool makeOwn = (argc >= 7 && QString::fromLocal8Bit(argv[6]) == QLatin1String("--own"));
        QTextStream out(stdout);
        QFile::remove(outPath);
        PdfDocument::libAddRef();
        PdfDocument doc;
        if (!doc.open(inPath)) { out << "FTAPS: FAIL open\n"; out.flush(); PdfDocument::libRelease(); return 1; }
        {
            AnnotationManager mgr;
            mgr.setDocument(doc.raw(), outPath);
            if (makeOwn) {
                mgr.setAllowTestStampUid(true);   // harness: mo phong annot CUA TA
                QString uid = mgr.generateUid();
                bool set = mgr.setAnnotUid(pageIndex, annotIndex, uid);
                out << "FTAPS: makeOwn setUid=" << (set?"true":"false") << " uid=" << uid << "\n";
            }
            bool ok = mgr.setAnnotStyle(pageIndex, annotIndex, QColor(0, 128, 255), 4.0f, false, 255);
            out << "FTAPS: setAnnotStyle=" << (ok?"true":"false") << "\n";
            bool sv = mgr.saveDocument();
            out << "FTAPS: save=" << (sv?"true":"false") << " err=" << mgr.lastError() << "\n";
        }
        out.flush();
        PdfDocument::libRelease();
        return 0;
    }

    // 🔴 P0 (0921): chot so huu setAnnotContents (o nhap comment o sidebar).
    // usage: --ftap-contents <in.pdf> <page> <idx> <out.pdf> [--own] [chu_moi]
    //   (khong --own): annot NGOAI → phai false, /Contents khong doi mot byte.
    //   --own        : gan TRUID truoc → annot CUA TA → phai true (van go duoc).
    if (argc >= 6 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--ftap-contents")) {
        QString inPath = QString::fromLocal8Bit(argv[2]);
        int pageIndex = QString::fromLocal8Bit(argv[3]).toInt();
        int annotIndex = QString::fromLocal8Bit(argv[4]).toInt();
        QString outPath = QString::fromLocal8Bit(argv[5]);
        const bool makeOwn = (argc >= 7 && QString::fromLocal8Bit(argv[6]) == QLatin1String("--own"));
        const QString newText = (argc >= 8) ? QString::fromUtf8(argv[7])
                                            : QStringLiteral("TORREADER edited content 0921");
        QTextStream out(stdout);
        QFile::remove(outPath);
        PdfDocument::libAddRef();
        PdfDocument doc;
        if (!doc.open(inPath)) { out << "FTAPC: FAIL open\n"; out.flush(); PdfDocument::libRelease(); return 1; }
        {
            AnnotationManager mgr;
            mgr.setDocument(doc.raw(), outPath);
            if (makeOwn) {
                mgr.setAllowTestStampUid(true);   // harness: mo phong annot CUA TA
                QString uid = mgr.generateUid();
                bool set = mgr.setAnnotUid(pageIndex, annotIndex, uid);
                out << "FTAPC: makeOwn setUid=" << (set?"true":"false") << " uid=" << uid << "\n";
            }
            bool ok = mgr.setAnnotContents(pageIndex, annotIndex, newText);
            out << "FTAPC: own=" << (makeOwn?"true":"false")
                << " setAnnotContents=" << (ok?"true":"false")
                << " newText=[" << newText << "]\n";
            bool sv = mgr.saveDocument();
            out << "FTAPC: save=" << (sv?"true":"false") << " err=" << mgr.lastError() << "\n";
        }
        out.flush();
        PdfDocument::libRelease();
        return 0;
    }

    // 🔴 P1 (0921): moveAnnot tren annot NGOAI (Ink) — phai false VA /Rect khong doi.
    // usage: --move-test <in.pdf> <page> <idx> <out.pdf>
    if (argc >= 6 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--move-test")) {
        QString inPath = QString::fromLocal8Bit(argv[2]);
        int pageIndex = QString::fromLocal8Bit(argv[3]).toInt();
        int annotIndex = QString::fromLocal8Bit(argv[4]).toInt();
        QString outPath = QString::fromLocal8Bit(argv[5]);
        QTextStream out(stdout);
        QFile::remove(outPath);
        PdfDocument::libAddRef();
        PdfDocument doc;
        if (!doc.open(inPath)) { out << "MOVET: FAIL open\n"; out.flush(); PdfDocument::libRelease(); return 1; }
        {
            AnnotationManager mgr;
            mgr.setDocument(doc.raw(), outPath);
            bool ok = mgr.moveAnnot(pageIndex, annotIndex, 50.0, 30.0);
            out << "MOVET: moveAnnot=" << (ok?"true":"false") << "\n";
            bool sv = mgr.saveDocument();
            out << "MOVET: save=" << (sv?"true":"false") << " err=" << mgr.lastError() << "\n";
        }
        out.flush();
        PdfDocument::libRelease();
        return 0;
    }

    // 🔴 VIỆC 1 (0921): chứng minh CỬA DUY NHẤT — in quyết định guardWrite cho từng
    // loại ghi, rồi gọi THẬT các hàm ghi để đối chiếu (annot ngoài phải false hết,
    // trừ Delete; --own mô phỏng annot của ta ⇒ phải true).
    // usage: --guardwrite-suite <in.pdf> <page> <idx> <out.pdf> [--own]
    if (argc >= 6 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--guardwrite-suite")) {
        QString inPath = QString::fromLocal8Bit(argv[2]);
        int pageIndex = QString::fromLocal8Bit(argv[3]).toInt();
        int annotIndex = QString::fromLocal8Bit(argv[4]).toInt();
        QString outPath = QString::fromLocal8Bit(argv[5]);
        const bool makeOwn = (argc >= 7 && QString::fromLocal8Bit(argv[6]) == QLatin1String("--own"));
        QTextStream out(stdout);
        QFile::remove(outPath);
        PdfDocument::libAddRef();
        PdfDocument doc;
        if (!doc.open(inPath)) { out << "GUARD: FAIL open\n"; out.flush(); PdfDocument::libRelease(); return 1; }
        {
            AnnotationManager mgr;
            mgr.setDocument(doc.raw(), outPath);
            if (makeOwn) {
                mgr.setAllowTestStampUid(true);
                QString uid = mgr.generateUid();
                bool set = mgr.setAnnotUid(pageIndex, annotIndex, uid);
                out << "GUARD: makeOwn setUid=" << (set?"true":"false") << " uid=" << uid << "\n";
            }
            out << "GUARD: own=" << (makeOwn?"true":"false") << "\n";
            const struct { const char* name; AnnotationManager::AnnotWrite what; } kinds[] = {
                {"Contents", AnnotationManager::AnnotWrite::Contents},
                {"Style",    AnnotationManager::AnnotWrite::Style},
                {"Rect",     AnnotationManager::AnnotWrite::Rect},
                {"Geometry", AnnotationManager::AnnotWrite::Geometry},
                {"Delete",   AnnotationManager::AnnotWrite::Delete},
                {"Uid",      AnnotationManager::AnnotWrite::Uid},
            };
            for (const auto& k : kinds) {
                QString why;
                bool g = mgr.guardWrite(pageIndex, annotIndex, k.what, &why);
                out << "GUARD: guardWrite." << k.name << "=" << (g?"true":"false")
                    << " why=[" << why << "]\n";
            }
            // Gọi THẬT (có thể ghi nếu là annot của ta). Chỉ để in true/false.
            out << "GUARD: real.setAnnotContents=" << (mgr.setAnnotContents(pageIndex, annotIndex, QStringLiteral("GUARD-TEST"))?"true":"false") << "\n";
            out << "GUARD: real.moveAnnot="       << (mgr.moveAnnot(pageIndex, annotIndex, 7.0, 5.0)?"true":"false") << "\n";
            out << "GUARD: real.setAnnotRectDisplay=" << (mgr.setAnnotRectDisplay(pageIndex, annotIndex, QRectF(10,10,200,120))?"true":"false") << "\n";
            out << "GUARD: real.setAnnotStyle="  << (mgr.setAnnotStyle(pageIndex, annotIndex, QColor(0,128,255), 4.0f, false, 255)?"true":"false") << "\n";
            bool sv = mgr.saveDocument();
            out << "GUARD: save=" << (sv?"true":"false") << " err=" << mgr.lastError() << "\n";
        }
        out.flush();
        PdfDocument::libRelease();
        return 0;
    }

    // 🔴 VIỆC 3-A1 (0921): co giãn gốc annot — annot ngoài phải false, /Rect không đổi.
    // usage: --resize-test <in.pdf> <page> <idx> <out.pdf>
    if (argc >= 6 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--resize-test")) {
        QString inPath = QString::fromLocal8Bit(argv[2]);
        int pageIndex = QString::fromLocal8Bit(argv[3]).toInt();
        int annotIndex = QString::fromLocal8Bit(argv[4]).toInt();
        QString outPath = QString::fromLocal8Bit(argv[5]);
        QTextStream out(stdout);
        QFile::remove(outPath);
        PdfDocument::libAddRef();
        PdfDocument doc;
        if (!doc.open(inPath)) { out << "RESIZET: FAIL open\n"; out.flush(); PdfDocument::libRelease(); return 1; }
        {
            AnnotationManager mgr;
            mgr.setDocument(doc.raw(), outPath);
            bool ok = mgr.setAnnotRectDisplay(pageIndex, annotIndex, QRectF(20, 20, 260, 180));
            out << "RESIZET: setAnnotRectDisplay=" << (ok?"true":"false") << "\n";
            bool sv = mgr.saveDocument();
            out << "RESIZET: save=" << (sv?"true":"false") << " err=" << mgr.lastError() << "\n";
        }
        out.flush();
        PdfDocument::libRelease();
        return 0;
    }

    // 🔴 VIỆC 3-A3 (0921): "backup rồi xoá" + dựng lại byte-bằng.
    // usage: --annotdel <in.pdf> <page> <idx> <out.pdf>
    //   → in ra đường dẫn sidecar (backup) rồi xoá annot, lưu `out.pdf`.
    if (argc >= 6 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--annotdel")) {
        QString inPath = QString::fromLocal8Bit(argv[2]);
        int pageIndex = QString::fromLocal8Bit(argv[3]).toInt();
        int annotIndex = QString::fromLocal8Bit(argv[4]).toInt();
        QString outPath = QString::fromLocal8Bit(argv[5]);
        QTextStream out(stdout);
        QFile::remove(outPath);
        if (!QFile::copy(inPath, outPath)) { out << "ANNOTDEL: FAIL copy\n"; out.flush(); return 1; }
        PdfDocument::libAddRef();
        PdfDocument doc;
        if (!doc.open(inPath)) { out << "ANNOTDEL: FAIL open\n"; out.flush(); PdfDocument::libRelease(); return 1; }
        {
            AnnotationManager mgr;
            mgr.setDocument(doc.raw(), outPath);
            const QString side = mgr.backupAnnotForDelete(pageIndex, annotIndex);
            out << "ANNOTDEL: sidecar=[" << side << "]\n";
            if (side.isEmpty()) { out << "ANNOTDEL: backup=false\n"; out.flush(); PdfDocument::libRelease(); return 1; }
            bool rm = mgr.removeAnnot(pageIndex, annotIndex);
            out << "ANNOTDEL: removeAnnot=" << (rm?"true":"false") << "\n";
            bool sv = mgr.saveDocument();
            out << "ANNOTDEL: save=" << (sv?"true":"false") << " err=" << mgr.lastError() << "\n";
        }
        out.flush();
        PdfDocument::libRelease();
        return 0;
    }

    // usage: --annotundel <in.pdf> <page> <insertIdx> <sidecar.pdf> <out.pdf>
    if (argc >= 7 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--annotundel")) {
        QString inPath = QString::fromLocal8Bit(argv[2]);
        int pageIndex = QString::fromLocal8Bit(argv[3]).toInt();
        int insertAt = QString::fromLocal8Bit(argv[4]).toInt();
        QString sidePath = QString::fromLocal8Bit(argv[5]);
        QString outPath = QString::fromLocal8Bit(argv[6]);
        QTextStream out(stdout);
        QFile::remove(outPath);
        if (!QFile::copy(inPath, outPath)) { out << "ANNOTUNDEL: FAIL copy\n"; out.flush(); return 1; }
        PdfDocument::libAddRef();
        PdfDocument doc;
        if (!doc.open(inPath)) { out << "ANNOTUNDEL: FAIL open\n"; out.flush(); PdfDocument::libRelease(); return 1; }
        {
            AnnotationManager mgr;
            mgr.setDocument(doc.raw(), outPath);
            bool ok = mgr.restoreAnnotFromBackup(pageIndex, insertAt, sidePath);
            out << "ANNOTUNDEL: restore=" << (ok?"true":"false") << " err=" << mgr.lastError() << "\n";
        }
        out.flush();
        PdfDocument::libRelease();
        return 0;
    }

    // 🔴 P0 (0921): REDO "xoá annot ngoài" — mô phỏng ĐÚNG việc MainWindow::doRedo phải
    // làm trước khi loadTabFile (nạp lại TỪ ĐĨA): removeAnnot rồi GHI XUỐNG ĐĨA.
    //   --old : tái hiện lỗi cũ (KHÔNG saveDocument) ⇒ tệp không đổi ⇒ xoá bị mất.
    //   mặc định: nhánh ĐÃ VÁ (saveDocument) ⇒ tệp đổi, số annot giảm 1 (kiểm bằng --annotcount).
    // usage: --annotredo <in.pdf> <page> <idx> <out.pdf> [--old]
    if (argc >= 6 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--annotredo")) {
        QString inPath = QString::fromLocal8Bit(argv[2]);
        int pageIndex = QString::fromLocal8Bit(argv[3]).toInt();
        int annotIndex = QString::fromLocal8Bit(argv[4]).toInt();
        QString outPath = QString::fromLocal8Bit(argv[5]);
        const bool oldMode = (argc >= 7 && QString::fromLocal8Bit(argv[6]) == QLatin1String("--old"));
        QTextStream out(stdout);
        QFile::remove(outPath);
        if (!QFile::copy(inPath, outPath)) { out << "ANNOTREDO: FAIL copy\n"; out.flush(); return 1; }
        PdfDocument::libAddRef();
        PdfDocument doc;
        // Mở bản GỐC, ghi ra `outPath` (giống --annotdel): tránh tự ghi đè tệp đang mở.
        if (!doc.open(inPath)) { out << "ANNOTREDO: FAIL open\n"; out.flush(); PdfDocument::libRelease(); return 1; }
        bool rm = false, sv = oldMode;   // --old: KHÔNG ghi (đúng lỗi cũ)
        {
            AnnotationManager mgr;
            mgr.setDocument(doc.raw(), outPath);
            rm = mgr.removeAnnot(pageIndex, annotIndex);
            if (!oldMode) sv = mgr.saveDocument();   // 🔴 P0: GHI trước khi nạp lại tab
        }
        out << "ANNOTREDO: mode=" << (oldMode ? "OLD" : "NEW")
            << " removeAnnot=" << (rm?"true":"false")
            << " save=" << (sv?"true":"false")
            << (oldMode ? " (OLD: co y KHONG ghi — tai hien loi)" : " (NEW: da ghi xuong dia)")
            << "\n";
        out.flush();
        PdfDocument::libRelease();
        return (rm && (oldMode || sv)) ? 0 : 1;
    }

    // Đếm annot một trang trong tệp (mở MỚI hoàn toàn) — dùng để nghiệm thu REDO.
    // usage: --annotcount <in.pdf> <page>
    if (argc >= 4 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--annotcount")) {
        QString inPath = QString::fromLocal8Bit(argv[2]);
        int pageIndex = QString::fromLocal8Bit(argv[3]).toInt();
        QTextStream out(stdout);
        PdfDocument::libAddRef();
        PdfDocument doc;
        if (!doc.open(inPath)) { out << "ANNOTCOUNT: FAIL open\n"; out.flush(); PdfDocument::libRelease(); return 1; }
        int n = -1;
        {
            AnnotationManager mgr;
            mgr.setDocument(doc.raw(), inPath);
            n = mgr.annotCount(pageIndex);
        }
        out << "ANNOTCOUNT: page=" << pageIndex << " count=" << n << "\n";
        out.flush();
        PdfDocument::libRelease();
        return n >= 0 ? 0 : 1;
    }

    // 🔴 P2 (0921): chung minh addSnapshot CO THE that bai (subtype PDFium khong tao
    // lai duoc, vd /Line) ⇒ nhanh shape undo/redo phai boc `if (ok)`.
    // usage: --addsnap-test <in.pdf> <page> <out.pdf> [--old]
    //   (khong --old): nhanh MOI — chi gan uid khi addSnapshot true.
    //   --old        : nhanh CU — gan uid bat ke ket qua (de doi chieu).
    if (argc >= 5 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--addsnap-test")) {
        QString inPath = QString::fromLocal8Bit(argv[2]);
        int pageIndex = QString::fromLocal8Bit(argv[3]).toInt();
        QString outPath = QString::fromLocal8Bit(argv[4]);
        const bool oldMode = (argc >= 6 && QString::fromLocal8Bit(argv[5]) == QLatin1String("--old"));
        QTextStream out(stdout);
        QFile::remove(outPath);
        PdfDocument::libAddRef();
        PdfDocument doc;
        if (!doc.open(inPath)) { out << "SNAPT: FAIL open\n"; out.flush(); PdfDocument::libRelease(); return 1; }
        {
            AnnotationManager mgr;
            mgr.setDocument(doc.raw(), outPath);
            int before = mgr.annotCount(pageIndex);
            AnnotSnapshot s;
            s.valid = true;
            s.subtype = FPDF_ANNOT_LINE;     // PDFium khong tao lai duoc
            s.rl = 100; s.rt = 200; s.rr = 300; s.rb = 150;
            s.uid = QStringLiteral("P2TESTUID");
            bool ok = mgr.addSnapshot(pageIndex, s);
            int after = mgr.annotCount(pageIndex);
            out << "SNAPT: addSnapshot=" << (ok?"true":"false")
                << " annotCount " << before << " -> " << after << "\n";
            bool stamped = false;
            if (oldMode) {
                if (after > 0) stamped = mgr.setAnnotUid(pageIndex, after - 1, s.uid);
                out << "SNAPT: mode=OLD (bat ke ket qua) stamped=" << (stamped?"true":"false") << "\n";
            } else {
                if (ok && after > 0) stamped = mgr.setAnnotUid(pageIndex, after - 1, s.uid);
                out << "SNAPT: mode=NEW guard_skipped_stamp=" << (ok?"false":"true")
                    << " stamped=" << (stamped?"true":"false") << "\n";
            }
            bool sv = mgr.saveDocument();
            out << "SNAPT: save=" << (sv?"true":"false") << " err=" << mgr.lastError() << "\n";
        }
        out.flush();
        PdfDocument::libRelease();
        return 0;
    }

    // TEMP PROBE 0921: kiem tra GetAP tra ve gi voi AP stream that + SetAP ghi ra stream hay string.
    if (argc >= 6 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--apdump-test")) {
        QString inPath = QString::fromLocal8Bit(argv[2]);
        int pageIndex = QString::fromLocal8Bit(argv[3]).toInt();
        int annotIndex = QString::fromLocal8Bit(argv[4]).toInt();
        QString outPath = QString::fromLocal8Bit(argv[5]);
        QTextStream out(stdout);
        PdfDocument::libAddRef();
        PdfDocument doc;
        if (!doc.open(inPath)) { out << "APDUMP: FAIL open\n"; out.flush(); PdfDocument::libRelease(); return 1; }
        {
            QMutexLocker lock(&s_pdfiumMutex);
            FPDF_PAGE p = FPDF_LoadPage(doc.raw(), pageIndex);
            FPDF_ANNOTATION a = FPDFPage_GetAnnot(p, annotIndex);
            if (!a || !p) { out << "APDUMP: FAIL annot\n"; out.flush(); FPDF_ClosePage(p); PdfDocument::libRelease(); return 1; }
            unsigned long n = FPDFAnnot_GetAP(a, FPDF_ANNOT_APPEARANCEMODE_NORMAL, nullptr, 0);
            out << "APDUMP: orig GetAP len=" << n << "\n";
            if (n > 2) {
                std::vector<unsigned short> buf(n/2+1, 0);
                FPDFAnnot_GetAP(a, FPDF_ANNOT_APPEARANCEMODE_NORMAL, (FPDF_WCHAR*)buf.data(), n);
                QString s = QString::fromUtf16((const char16_t*)buf.data());
                out << "APDUMP: orig GetAP text=[" << s.left(300) << "]\n";
            }
            FS_RECTF r{}; FPDFAnnot_GetRect(a, &r);
            out << "APDUMP: rect=" << r.left << "," << r.bottom << "," << r.right << "," << r.top << "\n";
            FPDFPage_CloseAnnot(a); FPDF_ClosePage(p);
        }
        {
            AnnotationManager mgr;
            mgr.setDocument(doc.raw(), outPath);
            bool ok = mgr.createInlineNote(pageIndex, QRectF(50,100,300,30),
                        QStringLiteral("An detail nay"), QStringLiteral("tmp"),
                        false, QColor(255,0,0), 15.0f);
            out << "APDUMP: createInlineNote=" << (ok?"true":"false") << "\n";
            bool sv = mgr.saveDocument();
            out << "APDUMP: save=" << (sv?"true":"false") << " err=" << mgr.lastError() << "\n";
        }
        out.flush();
        PdfDocument::libRelease();
        return 0;
    }

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

    // usage: --loading-badge-probe <out.png>
    // LUOT 42 (30/09): nghiem thu ham dung chung loadingBadgeImage()/drawLoadingBadge()
    // (xem ui/LoadingBadge.h) — chu Loading/man chao ve qua QImage CPU thay vi
    // drawText() thang len QOpenGLWidget (glyph cache GL hong sau khi GL resource
    // cac tab bi huy/tao lai lam chu vo bien dang, owner bao 30/09). Khong can GL:
    // chi goi ham dung chung o dpr 1.0 va 1.5 voi CHINH tham so dang dung o
    // ContinuousView (xam 150 nen trang) va PdfGpuView (trang 26pt bold nen toi),
    // ghep len mot QImage nen roi luu PNG.
    if (argc >= 3 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--loading-badge-probe")) {
        const QString outPath = QString::fromLocal8Bit(argv[2]);

        const int cellW = 420, cellH = 140, pad = 12;
        const int cols = 2, rows = 2;   // (dpr 1.0/1.5) x (Continuous xam/PdfGpuView trang)
        QImage sheet(cols * cellW, rows * cellH, QImage::Format_ARGB32_Premultiplied);
        sheet.fill(Qt::white);
        QPainter sp(&sheet);

        auto drawCell = [&](int col, int row, const QColor& cellBg, const QImage& badge,
                             const QString& label) {
            const QRect cellRect(col * cellW, row * cellH, cellW, cellH);
            sp.fillRect(cellRect, cellBg);
            drawLoadingBadge(sp, QRectF(cellRect.adjusted(pad, pad, -pad, -pad)), badge);
            sp.setPen(Qt::red);
            sp.drawText(cellRect.adjusted(4, 2, -4, -2), Qt::AlignLeft | Qt::AlignTop, label);
            sp.setPen(QColor(0, 0, 0, 60));
            sp.drawRect(cellRect.adjusted(0, 0, -1, -1));
        };

        // Cot 0 = dpr 1.0, cot 1 = dpr 1.5. Hang 0 = ContinuousView (Loading xam
        // tren nen trang), hang 1 = PdfGpuView (Loading trang bold tren nen toi).
        const qreal dprs[2] = {1.0, 1.5};
        for (int c = 0; c < 2; ++c) {
            const QImage contBadge = loadingBadgeImage(QStringLiteral("Loading..."), 11, false,
                                                        QColor(150, 150, 150), dprs[c]);
            drawCell(c, 0, Qt::white, contBadge,
                     QString("dpr=%1 ContinuousView").arg(dprs[c]));

            const QImage gpuBadge = loadingBadgeImage(QStringLiteral("Loading…"), 26, true,
                                                       QColor(255, 255, 255), dprs[c]);
            drawCell(c, 1, QColor(45, 45, 45), gpuBadge,
                     QString("dpr=%1 PdfGpuView").arg(dprs[c]));
        }
        sp.end();

        const bool ok = sheet.save(outPath, "PNG");
        fprintf(stdout, "LOADINGPROBE: %s w=%d h=%d\n", ok ? "PASS" : "FAIL",
                sheet.width(), sheet.height());
        fflush(stdout);
        return ok ? 0 : 1;
    }

    // usage: --imgstamp-test <out.pdf>
    // Headless self-check cua Insert Image (SPEC_INSERT_IMAGE_2026-08-30). Dung
    // DUNG duong code that: AnnotationManager::insertStampImage -> save -> reopen.
    // Chay lai logic Gate 1 (1 Stamp + /AP + /Rect trong page) va Gate 2 (anh PNG
    // trong suot GIU kenh alpha: vung trong suot khi render lai phai bang nen trang,
    // KHONG phai khoi den/trang duc). In [imgstamp] + ket qua IMGSTAMP_TEST.
    if (argc >= 3 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--imgstamp-test")) {
        const QString outPath = QString::fromLocal8Bit(argv[2]);
        QTextStream out(stdout);
        PdfDocument::libAddRef();

        // Trang moi 612x792, khong xoay => display == pdf (chi lat Y).
        FPDF_DOCUMENT doc = FPDF_CreateNewDocument();
        FPDF_PAGE np = FPDFPage_New(doc, 0, 612, 792);
        FPDFPage_GenerateContent(np);
        FPDF_ClosePage(np);

        // Anh chân ly: 128x64, phia trai ~75% do duc, phia phai 25% TRONG SUOT.
        QImage src(128, 64, QImage::Format_ARGB32_Premultiplied);
        src.fill(Qt::transparent);
        for (int y = 0; y < 64; ++y)
            for (int x = 0; x < 96; ++x)
                src.setPixelColor(x, y, QColor(220, 40, 40, 255));

        AnnotationManager mgr;
        mgr.setDocument(doc, outPath);
        // rect o toa do hien thi (Y-down), khong xoay.
        QRectF rectDisp(200.0, 250.0, 180.0, 90.0);
        const QString uid = mgr.insertStampImage(0, src, rectDisp);
        if (uid.isEmpty()) {
            out << "[imgstamp] FAIL: insertStampImage tra rong, err="
                << mgr.lastError() << "\n";
            out.flush();
            FPDF_CloseDocument(doc);
            PdfDocument::libRelease();
            return 1;
        }
        if (!mgr.saveDocument()) {
            out << "[imgstamp] FAIL: saveDocument err=" << mgr.lastError() << "\n";
            out.flush();
            FPDF_CloseDocument(doc);
            PdfDocument::libRelease();
            return 1;
        }
        FPDF_CloseDocument(doc);

        // ── Gate 1: mo lai, dem Stamp + /AP + /Rect ─────────────────────────
        FPDF_DOCUMENT doc2 = FPDF_LoadDocument(outPath.toUtf8().constData(), nullptr);
        if (!doc2) {
            out << "[imgstamp] FAIL reopen\n"; out.flush();
            FPDF_CloseDocument(doc2);
            PdfDocument::libRelease();
            return 1;
        }
        int stampCount = 0, hasAPCount = 0;
        QRectF stampRect;
        {
            QMutexLocker lock(&s_pdfiumMutex);
            FPDF_PAGE p = FPDF_LoadPage(doc2, 0);
            if (!p) { out << "[imgstamp] FAIL load page\n"; out.flush(); FPDF_CloseDocument(doc2); PdfDocument::libRelease(); return 1; }
            const int n = FPDFPage_GetAnnotCount(p);
            for (int i = 0; i < n; ++i) {
                FPDF_ANNOTATION a = FPDFPage_GetAnnot(p, i);
                if (!a) continue;
                if (FPDFAnnot_GetSubtype(a) == FPDF_ANNOT_STAMP) {
                    ++stampCount;
                    if (FPDFAnnot_HasKey(a, "AP")) ++hasAPCount;
                    FS_RECTF r{};
                    if (FPDFAnnot_GetRect(a, &r))
                        stampRect = QRectF(r.left, r.bottom, r.right - r.left, r.top - r.bottom);
                }
                FPDFPage_CloseAnnot(a);
            }
            FPDF_ClosePage(p);
        }
        const double pageW = 612.0, pageH = 792.0;
        const bool rectInPage = (stampRect.width() > 0 && stampRect.height() > 0
                                 && stampRect.left() >= 0 && stampRect.top() <= pageH
                                 && stampRect.right() <= pageW && stampRect.bottom() >= 0);
        out << "[imgstamp] Gate1 stamp=" << stampCount << " hasAP=" << hasAPCount
            << " rect=(" << stampRect.x() << "," << stampRect.y() << " "
            << stampRect.width() << "x" << stampRect.height() << ") inPage="
            << (rectInPage ? 1 : 0) << "\n";
        out.flush();

        // ── Gate 2: render 60dpi, do pixel vung trong suot ──────────────────
        const double dpi = 60.0 / 72.0;
        const int rw = int(612 * dpi), rh = int(792 * dpi);
        auto px = [&](double pt) { return int(pt * dpi); };
        // Vung trong suot cua anh nam o TOP-RIGHT cua stamp (phan trai trong anh
        // do duc ~75%). Quy doi sang pixel render (display == pdf vi khong xoay).
        const int x0 = px(rectDisp.left() + 135.0);   // 96/128 * 180 = 135
        const int y0 = px(rectDisp.top());
        const int x1 = px(rectDisp.right());
        const int y1 = px(rectDisp.top() + rectDisp.height() / 4.0);   // 64/4
        auto nonWhiteIn = [&](const QImage& im, int a0, int b0, int a1, int b1) {
            qint64 c = 0;
            for (int y = b0; y < b1 && y < im.height(); ++y)
                for (int x = a0; x < a1 && x < im.width(); ++x) {
                    const QColor cpx = im.pixelColor(x, y);
                    if (qAbs(cpx.red() - 255) > 2 || qAbs(cpx.green() - 255) > 2
                        || qAbs(cpx.blue() - 255) > 2)
                        ++c;
                }
            return c;
        };
        qint64 nonWhiteTopRight = 0, nonWhiteLeft = 0, nonWhiteTopRightFull = 0;
        bool hasAlphaInVisual = false;
        {
            QMutexLocker lock(&s_pdfiumMutex);
            QImage limited(rw, rh, QImage::Format_ARGB32); limited.fill(Qt::white);
            QImage full(rw, rh, QImage::Format_ARGB32);       full.fill(Qt::white);
            FPDF_PAGE p = FPDF_LoadPage(doc2, 0);
            if (p) {
                FPDF_BITMAP bmp = FPDFBitmap_CreateEx(rw, rh, FPDFBitmap_BGRA,
                                                      limited.bits(), limited.bytesPerLine());
                FPDF_RenderPageBitmap(bmp, p, 0, 0, rw, rh, 0,
                                      FPDF_ANNOT | FPDF_RENDER_LIMITEDIMAGECACHE);
                FPDFBitmap_Destroy(bmp);
                FPDF_BITMAP bmp2 = FPDFBitmap_CreateEx(rw, rh, FPDFBitmap_BGRA,
                                                       full.bits(), full.bytesPerLine());
                FPDF_RenderPageBitmap(bmp2, p, 0, 0, rw, rh, 0, FPDF_ANNOT);
                FPDFBitmap_Destroy(bmp2);
                FPDF_ClosePage(p);
            }
            nonWhiteTopRight    = nonWhiteIn(limited, x0, y0, x1, y1);
            nonWhiteTopRightFull= nonWhiteIn(full, x0, y0, x1, y1);
            nonWhiteLeft        = nonWhiteIn(limited, px(rectDisp.left()), px(rectDisp.top()),
                                             px(rectDisp.left() + 60.0), px(rectDisp.bottom()));
        }

        // Overlay cung phai giu alpha (loadPageVisuals -> anh goc) — duong hien
        // thi Single + Continuous.
        {
            AnnotationManager mgr2;
            mgr2.setDocument(doc2, outPath);
            bool ovCap = false; bool hasFgn = false;
            const QList<AnnotVisual> vis = mgr2.loadPageVisuals(0, &ovCap, &hasFgn);
            out << "[imgstamp] overlayCapable=" << (ovCap ? 1 : 0)
                << " hasForeign=" << (hasFgn ? 1 : 0) << " visuals=" << vis.size() << "\n";
            for (const AnnotVisual& av : vis) {
                if (av.subtype != FPDF_ANNOT_STAMP) continue;
                if (!av.image.isNull()) {
                    // Vung trong suot cua nguon: pixel alpha=0 phai con ton tai.
                    for (int y = 0; y < av.image.height(); ++y)
                        for (int x = 0; x < av.image.width(); ++x)
                            if (qAlpha(av.image.pixel(x, y)) == 0) { hasAlphaInVisual = true; break; }
                }
            }
        }

        out << "[imgstamp] Gate2 limitedNonWhite=" << nonWhiteTopRight
            << " fullNonWhite=" << nonWhiteTopRightFull
            << " redLeft=" << nonWhiteLeft
            << " overlayKeepsAlpha=" << (hasAlphaInVisual ? 1 : 0) << "\n";
        out.flush();
        FPDF_CloseDocument(doc2);

        const bool pass = (stampCount >= 1 && hasAPCount >= 1 && rectInPage
                           && nonWhiteTopRight == 0 && nonWhiteLeft > 100
                           && hasAlphaInVisual);
        out << "IMGSTAMP_TEST " << (pass ? "PASS" : "FAIL")
            << " stamp=" << stampCount << " hasAP=" << hasAPCount
            << " alphaClean=" << nonWhiteTopRight << " overlayAlpha=" << hasAlphaInVisual << "\n";
        out.flush();
        PdfDocument::libRelease();
        return pass ? 0 : 1;
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

            // Start ThumbnailRenderPool on same file (R1: dung CHUNG doc voi renderer chinh)
            auto thumbPoolB = std::make_unique<ThumbnailRenderPool>();
            bool poolOkB = thumbPoolB->open(inputPath, docB.raw());
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
            // 0927 M2: qua registry + tra CHUNG doi tuong cho pool (nhu app), thay vi
            // tu `open()` roi pool lai tu mo them 1 doi tuong rieng tren cung file.
            std::shared_ptr<TileCacheFile> tileCacheC;
            {
                uint64_t hash = TileCacheFile::hashFile(inputPath);
                QFile szFile(inputPath);
                uint64_t sz   = static_cast<uint64_t>(szFile.size());
                tileCacheC = TileCacheRegistry::acquire(inputPath, hash, sz, docC.pageCount());
                if (tileCacheC->isOpen())
                    rendererC->setTileCache(tileCacheC);
            }

            // Start ThumbnailRenderPool like the app does (R1: dung CHUNG doc cua renderer)
            auto thumbPoolC = std::make_unique<ThumbnailRenderPool>();
            if (thumbPoolC->open(inputPath, docC.raw(), 0, 0, 0, tileCacheC)) {
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
            // 0927: nha handle + xoa .torcache/.lock. Bo dong nay thi TEST C giu
            // QLockFile va file .torcache tren dia (chi `moCoi` lan sau don duoc).
            rendererC->setTileCache(nullptr);
            if (tileCacheC) TileCacheRegistry::release(tileCacheC);
            tileCacheC.reset();
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
            PdfDocument poolDoc;   // R1: pool dung CHUNG doc voi renderer — mo truoc de lay handle
            if (!poolDoc.open(workPath)) {
                out << "SAVEBENCH: FAIL open doc for thumbpool\n"; out.flush();
                QFile::remove(workPath); PdfDocument::libRelease(); return 1;
            }
            QElapsedTimer t; t.start();
            if (!pool.open(workPath, poolDoc.raw())) {
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

    // ── fgnobj-probe: hop hinh hoc ben trong annot ngoai (FreeText/Square/Line) ──
    // usage: TorReader.exe --fgnobj-probe <pdf_path> [page1based]
    // Duyệt annot từng trang, in subtype / hasAP / TRUID / objCount / types object.
    if (argc >= 3 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--fgnobj-probe")) {
        QString pdfPath = QString::fromLocal8Bit(argv[2]);
        QList<int> pages;                      // pages 1-based; empty = quét cả tài liệu
        for (int i = 3; i < argc; ++i)
            pages << QString::fromLocal8Bit(argv[i]).toInt();

        auto subtypeName = [](int t) -> QString {
            switch (t) {
            case FPDF_ANNOT_FREETEXT: return "FreeText";
            case FPDF_ANNOT_SQUARE: return "Square";
            case FPDF_ANNOT_CIRCLE: return "Circle";
            case FPDF_ANNOT_LINE: return "Line";
            case FPDF_ANNOT_INK: return "Ink";
            case FPDF_ANNOT_STAMP: return "Stamp";
            case FPDF_ANNOT_HIGHLIGHT: return "Highlight";
            case FPDF_ANNOT_UNDERLINE: return "Underline";
            case FPDF_ANNOT_STRIKEOUT: return "StrikeOut";
            case FPDF_ANNOT_SQUIGGLY: return "Squiggly";
            case FPDF_ANNOT_TEXT: return "Text";
            case FPDF_ANNOT_LINK: return "Link";
            case FPDF_ANNOT_POLYGON: return "Polygon";
            case FPDF_ANNOT_POLYLINE: return "PolyLine";
            case FPDF_ANNOT_CARET: return "Caret";
            default: return "(" + QString::number(t) + ")";
            }
        };
        auto objTypeName = [](int t) -> QString {
            switch (t) {
            case FPDF_PAGEOBJ_TEXT: return "TEXT";
            case FPDF_PAGEOBJ_PATH: return "PATH";
            case FPDF_PAGEOBJ_IMAGE: return "IMAGE";
            case FPDF_PAGEOBJ_SHADING: return "SHADING";
            case FPDF_PAGEOBJ_FORM: return "FORM";
            default: return "UNKNOWN";
            }
        };

        QTextStream out(stdout);
        PdfDocument::libAddRef();
        FPDF_DOCUMENT doc = FPDF_LoadDocument(pdfPath.toUtf8().constData(), nullptr);
        if (!doc) {
            out << "[fgnobj] FAIL cannot open " << pdfPath << "\n"; out.flush();
            PdfDocument::libRelease(); return 1;
        }
        int pageCount = FPDF_GetPageCount(doc);
        QList<int> scan = pages.isEmpty()
            ? [&]{ QList<int> all; for (int i = 1; i <= pageCount; ++i) all << i; return all; }()
            : pages;
        int annotNgoaiCoAP = 0, trongDoObjCount0 = 0;
        for (int p : scan) {
            QMutexLocker lock(&s_pdfiumMutex);
            FPDF_PAGE page = FPDF_LoadPage(doc, p - 1);
            if (!page) { out << "[fgnobj] page=" << p << " FAIL cannot load page\n"; out.flush(); continue; }
            int n = FPDFPage_GetAnnotCount(page);
            for (int i = 0; i < n; ++i) {
                FPDF_ANNOTATION a = FPDFPage_GetAnnot(page, i);
                if (!a) continue;
                int sub = FPDFAnnot_GetSubtype(a);
                int hasAP  = FPDFAnnot_HasKey(a, "AP") ? 1 : 0;
                int truid  = FPDFAnnot_HasKey(a, "TRUID") ? 1 : 0;
                int oc = FPDFAnnot_GetObjectCount(a);
                QString types = "-";
                if (oc > 0) {
                    QHash<QString,int> g;
                    for (int j = 0; j < oc; ++j) {
                        FPDF_PAGEOBJECT o = FPDFAnnot_GetObject(a, j);
                        if (o) g[objTypeName(FPDFPageObj_GetType(o))]++;
                    }
                    QStringList parts;
                    for (auto it = g.constBegin(); it != g.constEnd(); ++it)
                        parts << it.key() + "×" + QString::number(it.value());
                    types = parts.join(",");
                }
                out << "[fgnobj] page=" << p << " idx=" << i << " subtype=" << subtypeName(sub)
                    << " hasAP=" << hasAP << " truid=" << truid
                    << " objCount=" << oc << " types=" << types << "\n";
                out.flush();
                if (truid == 0 && hasAP) {
                    bool haveObj = false;
                    for (int j = 0; j < oc; ++j) {
                        if (FPDFAnnot_GetObject(a, j)) { haveObj = true; break; }
                    }
                    ++annotNgoaiCoAP;
                    if (haveObj) ++trongDoObjCount0;
                }
                FPDFPage_CloseAnnot(a);
            }
            FPDF_ClosePage(page);
        }
        FPDF_CloseDocument(doc);
        PdfDocument::libRelease();
        int pct = annotNgoaiCoAP ? qRound(100.0 * trongDoObjCount0 / annotNgoaiCoAP) : 0;
        out << "[fgnobj] TONG page=" << scan.size()
            << " annotNgoaiCoAP=" << annotNgoaiCoAP
            << " trongDoObjCount>0=" << trongDoObjCount0
            << "  tyle=" << pct << "%\n";
        out.flush();
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
                // R1 (SPEC_PERF_HEAVYPAGE): di qua PageCache de dem FPDF_LoadPage bang
                // [pagecache] LOAD. Da giu s_pdfiumMutex nen goi acquire() hop le.
                page = PageCache::acquire(doc.raw(), pageNum - 1);
                loadPageMs = timer.elapsed();
            }
            if (!page) {
                fprintf(stderr, "RENDER_PROBE_FAIL cannot load page %d\n", pageNum);
                PdfDocument::libRelease();
                return 1;
            }
            // R1: cap doi acquire() — tu dong release khi het scope probe.
            PageCache::PageBorrow _probeBorrow(doc.raw(), pageNum - 1);

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

            // Handle muon tu PageCache — KHONG FPDF_ClosePage (PdfDocument::close se
            // goi PageCache::forgetDocument khi doc ra khoi pham vi).
        }
        PdfDocument::libRelease();
        return 0;
    }

    // ── Annot-ROI render bench (do chi phí render RIENG o tung chu thich) ──────
    // usage: TorReader.exe --annotroi-bench <pdf_path> <page_0based>
    // Cauthoi song con: tren trang nang, render mot o nho (clip dung ROI) co RE
    // hon render ca trang khong, hay PDFium van duyet toan bo object nen van dat?
    // Day la bai do DOC LAP — khong dung bat ky duong ve nao cua app.
    if (argc >= 4 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--annotroi-bench")) {
        const QString pdfPath = QString::fromLocal8Bit(argv[2]);
        const int pageIndex = QString::fromLocal8Bit(argv[3]).toInt();
        const double scale = 2.0;   // px/pt — ti le CHUNG cho ca ROI va ca trang
        const int margin = 4;       // px le quanh o chu thich
        QTextStream out(stdout);

        PdfDocument::libAddRef();
        FPDF_DOCUMENT doc = nullptr;
        { QMutexLocker lock(&s_pdfiumMutex);
          doc = FPDF_LoadDocument(pdfPath.toUtf8().constData(), nullptr); }
        if (!doc) {
            out << "[roibench] FAIL cannot open " << pdfPath << "\n"; out.flush();
            PdfDocument::libRelease(); return 1;
        }
        FPDF_PAGE page = nullptr;
        double pageW = 0, pageH = 0;
        int nAnnot = 0;
        { QMutexLocker lock(&s_pdfiumMutex);
          if (pageIndex < 0 || pageIndex >= FPDF_GetPageCount(doc)) {
              out << "[roibench] FAIL page " << pageIndex << " out of range\n"; out.flush();
              FPDF_CloseDocument(doc); PdfDocument::libRelease(); return 1;
          }
          page = FPDF_LoadPage(doc, pageIndex);
          if (page) {
              pageW = FPDF_GetPageWidth(page);
              pageH = FPDF_GetPageHeight(page);
              nAnnot = FPDFPage_GetAnnotCount(page);
          }
        }
        if (!page) {
            out << "[roibench] FAIL cannot load page " << pageIndex << "\n"; out.flush();
            FPDF_CloseDocument(doc); PdfDocument::libRelease(); return 1;
        }
        out << "[roibench] file=" << pdfPath << " page=" << pageIndex
            << " size=" << QString::number(pageW, 'f', 1) << "x" << QString::number(pageH, 'f', 1)
            << "pt scale=" << QString::number(scale, 'f', 1) << "px/pt annots=" << nAnnot << "\n";
        out.flush();

        auto subtypeName = [](int t) -> QString {
            switch (t) {
            case FPDF_ANNOT_FREETEXT: return "FreeText";
            case FPDF_ANNOT_SQUARE: return "Square";
            case FPDF_ANNOT_CIRCLE: return "Circle";
            case FPDF_ANNOT_LINE: return "Line";
            case FPDF_ANNOT_INK: return "Ink";
            case FPDF_ANNOT_STAMP: return "Stamp";
            case FPDF_ANNOT_HIGHLIGHT: return "Highlight";
            case FPDF_ANNOT_UNDERLINE: return "Underline";
            case FPDF_ANNOT_STRIKEOUT: return "StrikeOut";
            case FPDF_ANNOT_SQUIGGLY: return "Squiggly";
            case FPDF_ANNOT_TEXT: return "Text";
            case FPDF_ANNOT_LINK: return "Link";
            case FPDF_ANNOT_POLYGON: return "Polygon";
            case FPDF_ANNOT_POLYLINE: return "PolyLine";
            case FPDF_ANNOT_CARET: return "Caret";
            default: return "(" + QString::number(t) + ")";
            }
        };

        qint64 totalMs = 0, slowestMs = 0; int measured = 0;
        for (int i = 0; i < nAnnot; ++i) {
            FS_RECTF r{0, 0, 0, 0};
            int sub = FPDF_ANNOT_UNKNOWN;
            bool ok = false;
            { QMutexLocker lock(&s_pdfiumMutex);
              FPDF_ANNOTATION a = FPDFPage_GetAnnot(page, i);
              if (a) {
                  sub = FPDFAnnot_GetSubtype(a);
                  ok = (FPDFAnnot_GetRect(a, &r) != 0);
                  FPDFPage_CloseAnnot(a);
              } }
            const double rw = r.right - r.left, rh = r.top - r.bottom;
            if (!ok || rw <= 0 || rh <= 0) {
                out << "[roibench] annot i=" << i << " subtype=" << subtypeName(sub)
                    << " rect=INVALID px=0x0 ms=0\n"; out.flush();
                continue;
            }
            const int bw = qMax(1, (int)(rw * scale)) + 2 * margin;
            const int bh = qMax(1, (int)(rh * scale)) + 2 * margin;
            // PDF user space (y-up) -> device (y-down): a=s, d=-s,
            // e=margin - s*left, f=margin + s*top => goc (left,top) roi vao (margin,margin).
            FS_MATRIX m{ (float)scale, 0.f, 0.f, (float)-scale,
                         (float)(margin - scale * r.left), (float)(margin + scale * r.top) };
            FS_RECTF clip{ 0.f, 0.f, (float)bw, (float)bh };
            qint64 ms = 0;
            { QMutexLocker lock(&s_pdfiumMutex);
              QImage img(bw, bh, QImage::Format_ARGB32); img.fill(Qt::white);
              FPDF_BITMAP bmp = FPDFBitmap_CreateEx(bw, bh, FPDFBitmap_BGRA,
                                                    img.bits(), img.bytesPerLine());
              QElapsedTimer t; t.start();
              if (bmp) FPDF_RenderPageBitmapWithMatrix(bmp, page, &m, &clip, FPDF_ANNOT);
              ms = t.elapsed();
              if (bmp) FPDFBitmap_Destroy(bmp); }
            out << "[roibench] annot i=" << i << " subtype=" << subtypeName(sub)
                << " rect=" << (int)rw << "x" << (int)rh
                << " px=" << bw << "x" << bh << " ms=" << ms << "\n"; out.flush();
            totalMs += ms; ++measured; if (ms > slowestMs) slowestMs = ms;
        }
        const double avgMs = measured ? (double)totalMs / measured : 0.0;
        out << "[roibench] TONG " << nAnnot << " annot: tong ms=" << totalMs
            << "  trung binh=" << QString::number(avgMs, 'f', 1)
            << "  lau nhat=" << slowestMs << "\n"; out.flush();

        // MOC: render CA TRANG cung ti le (cung flags) de so sanh.
        {
            const int fw = qMax(1, (int)(pageW * scale)), fh = qMax(1, (int)(pageH * scale));
            FS_MATRIX fm{ (float)scale, 0.f, 0.f, (float)-scale, 0.f, (float)(scale * pageH) };
            FS_RECTF fc{ 0.f, 0.f, (float)fw, (float)fh };
            qint64 ms = 0;
            { QMutexLocker lock(&s_pdfiumMutex);
              QImage img(fw, fh, QImage::Format_ARGB32); img.fill(Qt::white);
              FPDF_BITMAP bmp = FPDFBitmap_CreateEx(fw, fh, FPDFBitmap_BGRA,
                                                    img.bits(), img.bytesPerLine());
              QElapsedTimer t; t.start();
              if (bmp) FPDF_RenderPageBitmapWithMatrix(bmp, page, &fm, &fc, FPDF_ANNOT);
              ms = t.elapsed();
              if (bmp) FPDFBitmap_Destroy(bmp); }
            out << "[roibench] MOC: render CA TRANG cung ti le: ms=" << ms
                << " (" << fw << "x" << fh << "px)\n"; out.flush();
        }

        { QMutexLocker lock(&s_pdfiumMutex); FPDF_ClosePage(page); FPDF_CloseDocument(doc); }
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
        PdfDocument poolDoc;   // R1: pool dung CHUNG doc voi renderer — mo truoc de lay handle
        if (!poolDoc.open(inPath)) {
            out << "THUMBBENCH: FAIL poolDoc.open\n"; out.flush();
            PdfDocument::libRelease(); return 1;
        }
        QElapsedTimer t; t.start();
        if (!pool.open(inPath, poolDoc.raw())) {
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
        if (!poolA.open(pdfA, docA.raw())) { out << "THUMBEPOCH: FAIL poolA.open\n"; out.flush(); return 1; }
        quint64 epochA = poolA.epoch();

        // Step 3: set up panel with docA
        PdfRenderer rendererA;
        rendererA.setDocument(&docA);
        ThumbnailPanel panel;
        panel.setDocument(&docA, &rendererA, &poolA, true);

        // Step 4: switch pool to pdfB, verify epoch advances
        poolA.close();
        if (!poolA.open(pdfB, docB.raw())) { out << "THUMBEPOCH: FAIL poolA.open(pdfB)\n"; out.flush(); return 1; }
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
        if (!poolA.open(pdfA, docA.raw())) { out << "THUMBEPOCH: FAIL poolA.open(pdfA)\n"; out.flush(); return 1; }
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
            if (!poolD.open(pdfA, docA.raw())) { out << "THUMBEPOCH: FAIL poolD.open(pdfA)\n"; out.flush(); return 1; }
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

        if (!pool.open(pdfA, docA.raw())) { out << "THUMBRELOAD: FAIL pool.open(pdfA)\n"; out.flush(); return 1; }
        panel.setDocument(&docA, &rendererA, &pool, true);
        QCoreApplication::processEvents();
        if (!runPhase("mo", pool, panel, 5)) return 1;

        pool.close();
        docA.close();
        docB.open(pdfB);
        if (!pool.open(pdfB, docB.raw())) { out << "THUMBRELOAD: FAIL pool.open(pdfB) insert\n"; out.flush(); return 1; }
        panel.setDocument(&docB, &rendererB, &pool, true);
        QCoreApplication::processEvents();
        if (!runPhase("insert", pool, panel, 5)) return 1;

        pool.close();
        if (!pool.open(pdfB, docB.raw())) { out << "THUMBRELOAD: FAIL pool.open(pdfB) save\n"; out.flush(); return 1; }
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
        const bool realLoop = !qEnvironmentVariableIsEmpty("TORREADER_UIPROBE_REALLOOP");
        qDebug().noquote() << "[uiprobe] waitMode=" << (realLoop ? "realloop" : "sleep50");
        if (realLoop) {
            QEventLoop _probeLoop;
            QTimer::singleShot(waitMs, &_probeLoop, &QEventLoop::quit);
            _probeLoop.exec();
        } else {
            const int loops = qMax(waitMs / 50, 1);
            for (int i = 0; i < loops; ++i) {
                QCoreApplication::processEvents();
                QThread::msleep(50);
            }
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

    // usage: --viewprobe <input.pdf> <out.png> <continuous:0|1> <zoomPercent> <page1Based> [waitMs] [centerXpt] [centerYpt] [zoom2Percent]
    //   dat VIEWPROBE_GL=1 de chup framebuffer GL (Continuous khong bi trang).
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
        // LUOT 38: cho phep thu nho cua so de chup OS-level (Continuous GL) khi man hinh nho.
        const int envW = qEnvironmentVariableIntValue("VIEWPROBE_W");
        const int envH = qEnvironmentVariableIntValue("VIEWPROBE_H");
        w.resize(envW > 0 ? envW : 1600, envH > 0 ? envH : 1000);
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

        // 🔴 THEM 2026-09-01: tham so 11 = ZOOM THU HAI. Bai do cu chi DAT zoom mot lan roi
        // chup, nen khong bao gio di qua thao tac ZOOM (huy render + doi ty le). Owner bao mat
        // hinh khi "zoom xuong 50", ma dung yen o 50 thi van du net ⇒ loi nam o CHINH THAO TAC.
        if (argc >= 11) {
            const double z2 = QString::fromLocal8Bit(argv[10]).toDouble();
            if (z2 > 0) {
                fprintf(stdout, "VIEWPROBE: zoom lan 2 -> %.0f%%\n", z2);
                fflush(stdout);
                if (continuous) w.probeSetZoom(z2 / 100.0);
                else            w.probeSetZoomSingle(z2 / 100.0);
                for (int k = 0; k < 400; ++k) { QCoreApplication::processEvents(); QThread::msleep(25); }
            }
        }
        // LUOT 38: che do GL (Continuous) — w.grab() khong chup duoc QOpenGLWidget (ra trang).
        // Dat VIEWPROBE_GL=1 de lay framebuffer cua chinh widget GL.
        QImage img;
        if (qEnvironmentVariableIsSet("VIEWPROBE_GL")) img = w.probeGrabView();
        else                                            img = w.grab().toImage();
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

    // ── LƯỢT 35 — MÀN HÌNH CHÀO: chụp CẢ CỬA SỔ ở 4 ca ───────────────────────────
    // usage: --welcome-probe <pdf> <outprefix>
    //   outprefix vd "C:/Users/Public/welcome" → welcome_first/_single/_cont/_dark.png
    // Chứng minh màn chào sau khi ĐÓNG HẾT TAB là widget THƯỜNG (PdfView/CPU),
    // chữ theo theme, KHÔNG còn vỡ trên GL. CEO nhìn ảnh.
    if (argc >= 4 && QCoreApplication::arguments().value(1) == QLatin1String("--welcome-probe")) {
        const QStringList as = QCoreApplication::arguments();
        auto strip = [](QString v) {
            if (v.size() > 1 && v.startsWith(QLatin1Char('"')) && v.endsWith(QLatin1Char('"')))
                return v.mid(1, v.size() - 2);
            return v;
        };
        const QString pdf = strip(as.value(2));
        const QString outPrefix = strip(as.value(3));
        MainWindow w; w.resize(1400, 900); w.show();
        auto spin = [](int ms) { int n = qMax(ms / 50, 1);
            for (int i = 0; i < n; ++i) { QCoreApplication::processEvents(); QThread::msleep(50); } };
        spin(1200);
        auto shot = [&](const char* name) {
            QCoreApplication::processEvents();
            const QString p = outPrefix + QLatin1Char('_') + QString::fromLatin1(name) + QLatin1String(".png");
            const bool ok = w.grab().save(p, "PNG");
            fprintf(stdout, "WELCOME %s ok=%d tabs=%d docs=%d contVisible=%d\n",
                    p.toLocal8Bit().constData(), ok ? 1 : 0, w.probeTabCount(), w.probeOpenDocCount(),
                    w.probeContinuousVisible() ? 1 : 0);
            fflush(stdout);
        };
        auto closeAll = [&] {
            for (int n = w.probeTabCount(); n > 0; ) {
                w.probeCloseTab(0);
                const int n2 = w.probeTabCount();
                if (n2 >= n) break;
                n = n2;
            }
            spin(1500);
        };
        auto waitOpen = [&] { QElapsedTimer t; t.start();
            while (t.elapsed() < 30000 && w.probePageCount() < 1) spin(100); };

        shot("first");                                                        // ca 1: mo app lan dau
        w.probeSetFastMode(false); w.openFile(pdf); waitOpen(); closeAll(); shot("single"); // ca 2: Single
        w.probeSetFastMode(true);  w.openFile(pdf); waitOpen(); closeAll(); shot("cont");   // ca 3: Continuous (BUG goc)
        w.probeSetFastMode(false); w.probeSetDarkMode(true); w.openFile(pdf); waitOpen(); closeAll(); shot("dark"); // ca 4: Dark
        // 🔴 LƯỢT 37 (mục B, reviewer lỗi 2): KHONG CON TAB nao ⇒ bam View Fast / Quality.
        // setViewMode(true) show ContinuousView VO DIEU KIEN ⇒ ban khong sua thi CV trong hien ra,
        // Welcome bi ep con chieu cao thanh tab ⇒ MAT man chao. Sau sua: applyWelcomeVisibility()
        // cuoi setViewMode an CV ⇒ ca 2 anh van LA man chao.
        w.probeSetDarkMode(false);                               // ve sang de anh de doi chieu
        w.probeSetFastMode(false); closeAll();                    // ve Quality, chac chan het tab
        w.probeSetFastMode(true);  shot("fast_notab");            // ca 5: het tab roi bam VIEW FAST
        w.probeSetFastMode(false); shot("quality_notab");         // ca 6: het tab roi bam QUALITY
        fprintf(stdout, "WELCOME: DONE\n"); fflush(stdout);
        return 0;
    }

    // usage: --viewfast-probe <input.pdf> <out.png> [waitMs]
    // Probe (SPEC_VIEWFAST 2026-08-31): mo file KHI DANG O View Fast, giong dung
    // tinh huong goc (settle timer + requestPage trong khi fast mode). Kiem log
    // khong con [lockhold] > 5000 / [render done] > 10000 / [lockwait main=1] > 1000.
    // ── BAI DO DONG TAB (2026-08-31, kich ban owner) ───────────────────────────
    // Owner do duoc: mo 3 tab, DONG 2 tab nang, RAM van con 4,5 GB.
    // Bai nay tai hien y het: mo f1,f2,f3 -> do -> dong tab 0 va 1 -> do lai.
    //   --tabclose-probe <f1> <f2> <f3> [waitMs=20000]
    if (argc >= 5 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--tabclose-probe")) {
        auto strip = [](QString v) {
            if (v.startsWith(QLatin1Char('"')) && v.endsWith(QLatin1Char('"'))) return v.mid(1, v.size()-2);
            return v;
        };
        QString f1 = strip(QString::fromLocal8Bit(argv[2]));
        QString f2 = strip(QString::fromLocal8Bit(argv[3]));
        QString f3 = strip(QString::fromLocal8Bit(argv[4]));
        int waitMs = (argc >= 6) ? QString::fromLocal8Bit(argv[5]).toInt() : 20000;
        if (waitMs <= 0) waitMs = 20000;

        MainWindow w; w.resize(1400, 900); w.show();
        QCoreApplication::processEvents();
        auto spin = [&](int ms) {
            const int loops = qMax(ms / 50, 1);
            for (int i = 0; i < loops; ++i) { QCoreApplication::processEvents(); QThread::msleep(50); }
        };
        auto dump = [&](const char* moc) {
            long long ws = 0, pv = 0;
#ifdef Q_OS_WIN
            PROCESS_MEMORY_COUNTERS_EX pmc{};
            if (GetProcessMemoryInfo(GetCurrentProcess(),
                                     reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {
                ws = (long long)(pmc.WorkingSetSize / 1048576);
                pv = (long long)(pmc.PrivateUsage   / 1048576);
            }
#endif
            fprintf(stdout, "TABCLOSE[%s] WorkingSet=%lldMB RIENG=%lldMB | tai lieu mo=%d dong=%d"
                            " | pool mo=%d dong=%d | trang mo=%d dong=%d\n",
                    moc, ws, pv,
                    g_pdfiumDocOpen.loadRelaxed(),  g_pdfiumDocClose.loadRelaxed(),
                    g_pdfiumPoolOpen.loadRelaxed(), g_pdfiumPoolClose.loadRelaxed(),
                    g_pdfiumPageOpen.loadRelaxed(), g_pdfiumPageClose.loadRelaxed());
            fprintf(stdout, "%s\n", w.probeMemBreakdown().toLocal8Bit().constData());
            fflush(stdout);
        };

        w.probeResetThumbCounters();
        w.openFile(f1); spin(waitMs); dump("sau tab 1");
        fprintf(stdout, "[COLD] %s\n", w.probeThumbCounters().toLocal8Bit().constData()); fflush(stdout);
        w.probeResetThumbCounters();
        w.openFile(f2); spin(waitMs); dump("sau tab 2");
        fprintf(stdout, "[COLD] %s\n", w.probeThumbCounters().toLocal8Bit().constData()); fflush(stdout);
        w.probeResetThumbCounters();
        w.openFile(f3); spin(waitMs); dump("sau tab 3");
        fprintf(stdout, "[COLD] %s\n", w.probeThumbCounters().toLocal8Bit().constData()); fflush(stdout);
        // ── NGHIEM THU CO POLLING: switch to each tab and wait until its list is
        // FULL (or timeout). Immune to how slow the huge A0 files open/render.
        for (int tab = 0; tab < 3; ++tab) {
            fprintf(stdout, "%s\n", w.probeThumbVerify(tab, 90000).toLocal8Bit().constData());
            fflush(stdout);
        }
        // Moi lan mo file o tren LA mot lan doi tab that (openFile -> tab moi len
        // dau bang -> setDocument), nen 3 dong [COLD] tren da chung minh ca 3 tab
        // gan duoc dung so trang. Khong can vong bam-tab nua (no da duoc path
        // openFile bao pham va tung do ra nhan tab sai o ban cu).
        // dong 2 tab NANG (index 0 va 1) — dong index 0 hai lan vi danh sach dich len
        w.probeCloseTab(0); spin(5000);
        w.probeCloseTab(0); spin(8000);
        dump("sau khi DONG 2 tab nang");
        fprintf(stdout, "TABCLOSE: DONE\n"); fflush(stdout);
        return 0;
    }

    // ── LƯỢT 17 — BÀI TEST TỰ ĐỘNG KỊCH BẢN OWNER 3 TAB (đêm 28/09) ─────────────
    //   --owner3tab-probe <f1> <f2> <f3> [lap=1]
    // Tái hiện đúng VONG_LAP_DEM_0928: eYACHO → MEP 244MB (chưa kịp xong thumbnail
    // nền — CỐ Ý chỉ chờ 6 s) → tab3 "Phan ngam" → cuộn tab3 đo từng trang →
    // đóng tab3 khi MEP còn chạy → đóng tab NỀN → đóng 2 tab liên tiếp → đóng hết.
    // CÁCH ĐO "trang render xong có ảnh": ContinuousView::hasPageImage(p) =
    // m_pageImages[p] khác rỗng — CHÍNH CÁI KHO mà signal thật
    // PdfRenderer::continuousPageReady → "[perf] cont ACCEPT continuousPageReady
    // idx= p" (ContinuousView.cpp ~433) ghi vào. Không phải tín hiệu giả.
    // Đường dẫn đọc bằng QCoreApplication::arguments() — Qt phân giải UTF-8 đúng
    // cho tên tiếng Việt; fromLocal8Bit(argv) sẽ hỏng (f3 có "Ầ Ử Ơ &").
    if (argc >= 5 && QCoreApplication::arguments().value(1) == QLatin1String("--owner3tab-probe")) {
        const QStringList as = QCoreApplication::arguments();
        // Duong dan co dau cach + dau nhay hay bi cmd/WSL tach thanh nhieu arg
        // (da do 28/09: arg = "\"C:\\...260920"). Noi lai cho den khi dau nhay
        // dong; arg khong mo dau nhay = giu nguyen.
        int cur = 2;
        auto nextPath = [&as, &cur]() -> QString {
            QString v;
            for (int j = cur; j < as.size(); ++j) {
                v = v.isEmpty() ? as.value(j) : v + QLatin1Char(' ') + as.value(j);
                cur = j + 1;
                if (!(v.startsWith(QLatin1Char('"')) && !v.endsWith(QLatin1Char('"')))) break;
            }
            if (v.size() > 1 && v.startsWith(QLatin1Char('"')) && v.endsWith(QLatin1Char('"')))
                v = v.mid(1, v.size() - 2);
            return v;
        };
        const QString f1 = nextPath(), f2 = nextPath(), f3 = nextPath();
        const int laps = (cur < as.size()) ? qMax(1, as.value(cur).toInt()) : 1;
        // 🔴 LƯỢT 33a: OWNER3TAB_ONLY=JKL ⇒ bỏ qua A–I/G/H cho nhanh, CHỈ chạy J/K/L
        // (đo tranh quyền tab + nhảy trang 1 trên mã r31b — CEO tái hiện lỗi owner).
        const bool jklOnly = (qEnvironmentVariable("OWNER3TAB_ONLY") == QLatin1String("JKL"));
        // 🔴 LƯỢT 37: OWNER3TAB_ONLY=O ⇒ CHỈ chạy bước O (đo nhanh mục A, không kéo A–I).
        const bool oOnly = (qEnvironmentVariable("OWNER3TAB_ONLY") == QLatin1String("O"));
        for (const QString& f : {f1, f2, f3}) {
            if (!QFile::exists(f)) {
                fprintf(stdout, "OWNER3TAB: FAIL file khong ton tai: %s\n",
                        f.toLocal8Bit().constData()); fflush(stdout);
                return 2;
            }
        }

        MainWindow w; w.resize(1400, 900); w.show();
        QCoreApplication::processEvents();
        auto spin = [&](int ms) {
            const int loops = qMax(ms / 50, 1);
            for (int i = 0; i < loops; ++i) { QCoreApplication::processEvents(); QThread::msleep(50); }
        };
        bool allOk = true; QString reason;
        // 0928 LƯỢT 23 (DO): RSS + trang PDFium dang mo, in cung moi buoc va moi trang
        // do duoc. Gia thuyet can kiem chung: lap 2 cham KHONG phai tranh khoa (da do
        // voi TORREADER_NO_THUMB=1 — van cham 4,7 lan MỖI object), ma do bo nho cua
        // ca process phinh to tu lap 1 ⇒ moi lan PDFium cham object la mot lan
        // fault/TLB miss.
        auto rssMB = []() -> long long {
            long long ws = 0;
#ifdef Q_OS_WIN
            PROCESS_MEMORY_COUNTERS_EX pmc{};
            if (GetProcessMemoryInfo(GetCurrentProcess(),
                                     reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc)))
                ws = (long long)(pmc.WorkingSetSize / 1048576);
#endif
            return ws;
        };
        // 🔴 LƯỢT 27 (săn rò): bo nho COMMIT (PrivateUsage) — khong bi OS thu hoi.
        // WorkingSet co the la do da free nhung Windows chua tra ve OS; commit moi la
        // "app dang thuc giu". commit cao khi docMo=0 ⇒ RO THAT; commit thap ⇒ giu
        // working set cua vung da giai phong (khong phai ro).
        auto commitMB = []() -> long long {
            long long pv = 0;
#ifdef Q_OS_WIN
            PROCESS_MEMORY_COUNTERS_EX pmc{};
            if (GetProcessMemoryInfo(GetCurrentProcess(),
                                     reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc)))
                pv = (long long)(pmc.PrivateUsage / 1048576);
#endif
            return pv;
        };
        // 🔴 LƯỢT 27: ép OS thu hoi working set (EmptyWorkingSet). Neu RSS sau trim
        // ≈ commit ⇒ phan "leak" chi la trang da free chua tra ve OS (khong phai ro
        // handle). Neu RSS sau trim van ≈ truoc trim va commit cao ⇒ giu that.
        auto trimWS = []() -> bool {
#ifdef Q_OS_WIN
            return EmptyWorkingSet(GetCurrentProcess()) != FALSE;
#else
            return false;
#endif
        };
        // 0928 LƯỢT 23 (DO): tong so loi trang cua process (soft + hard). Neu lap 2
        // "an" nhieu loi trang hon het cho mot trang thi cham la do fault, khong
        // phai do PDFium tinh cham.
        auto pfCount = []() -> long long {
            // 0928 LƯỢT 25 (reviewer L23+L24 mục 1): psapi chỉ tồn tại trên Windows —
            // bọc Q_OS_WIN như rssMB ở trên, nhánh khác trả -1 (Linux vẫn biên dịch).
#ifdef Q_OS_WIN
            PROCESS_MEMORY_COUNTERS_EX pmc{};
            return GetProcessMemoryInfo(GetCurrentProcess(),
                       reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))
                       ? (long long)pmc.PageFaultCount : -1;
#else
            return -1;
#endif
        };
        // 🔴 0928 LƯỢT 26 (VIỆC 1): tong CPU MOI LUONG cua process (user+kernel).
        // So voi [latcpu] cua rieng luong render: proc_cpu >> cpu_render ⇒ co luong
        // khac dang an CPU (thumbnail/vector/luong dong tab) — "bi cuop CPU".
        auto procCpuMs = []() -> long long {
#ifdef Q_OS_WIN
            FILETIME c, e, u, k;
            if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &u, &k)) return -1;
            LARGE_INTEGER lu, lk;
            lu.LowPart = u.dwLowDateTime; lu.HighPart = u.dwHighDateTime;
            lk.LowPart = k.dwLowDateTime; lk.HighPart = k.dwHighDateTime;
            return (long long)((lu.QuadPart + lk.QuadPart) / 10000);   // 100ns -> ms
#else
            return -1;
#endif
        };
        // 0928 LƯỢT 23 (DO): nhịp đồng hồ CPU THỰC. Mot vong lap tinh toan tinh co
        // dinh (16M lan FNV) — neu no cham di 3-4 lan o lap 2 thi cham la do may
        // (tan so/heat/doi nutrition), khong phai do PDFium. Khac biot = giu nguyen.
        auto cpuNs = []() -> long long {
            volatile unsigned long long acc = 0;
            QElapsedTimer t; t.start();
            for (unsigned long long i = 0; i < 16000000ULL; ++i) acc = (acc ^ i) * 1099511628211ULL;
            const qint64 el = t.nsecsElapsed();
            return el;   // ns cho 16M vong (khong chia — chia nguyen ra 0)
        };
        auto noteFail = [&allOk, &reason](const QString& r) {
            if (!allOk) return;                 // gi nguyen ly do DAU TIEN
            allOk = false; reason = r;
        };
        // 🔴 LƯỢT 30 (BẮT BUỘC, vĩnh viễn): TRẦN RAM. commit (PrivateUsage) vượt
        // 3000 MB ở BẤT KỲ bước nào = FAIL. Sau khi đóng hết tab (docMo=0) commit phải
        // về dưới 1,5× mức khởi động + 300 MB. Chủ nhân báo RAM 10 GB mà thước cũ
        // không có trần RAM ⇒ chiều đó chưa được chứng minh.
        long long commitMax = 0, commitBaseline = -1;
        auto noteCommit = [&](const char* where) {
            const long long c = commitMB();
            if (c > commitMax) commitMax = c;
            if (c > 3000)
                noteFail(QStringLiteral("commit %1MB > 3000MB tran RAM (%2)").arg(c).arg(where));
        };
        // 0928 LƯỢT 24 (DO, khong sua pdfium): toc do Windows heap — 100k lan
        // malloc(16..255) + free, cham hon lap 1 tuc phan manh hoat dong cua
        // PDFium (font/glyph cache toan cuc, CPDF_PageModule) hoac heap nan.
        // So voi cpu_ns16M (giu nguyen) de tach "may cham" / "heap cham" / "PDFium cham".
        auto heapNs = []() -> long long {
            QElapsedTimer t; t.start();
            for (int i = 0; i < 100000; ++i) {
                void* p = malloc(static_cast<size_t>(16 + (i % 240)));
                if (p) { *static_cast<volatile char*>(p) = 1; free(p); }
            }
            return t.nsecsElapsed();
        };
        // 🔴 LƯỢT 21 — ĐO ĐỨNG HÌNH LUỒNG GIAO DIỆN: QTimer 20 ms, ghi khoảng cách
        // lớn nhất giữa 2 tick trong từng bước. FAIL nếu > 1000 ms.
        static QAtomicInteger<long long> pageObj[512];   // [heavycap] pre-count, theo trang
        QElapsedTimer tickT; tickT.start();
        qint64 freezeMax = 0; char curStep = '?'; int curLap = 0;
        QTimer freeze;
        QObject::connect(&freeze, &QTimer::timeout, &freeze, [&]{
            const qint64 g = tickT.restart();
            if (g > freezeMax) freezeMax = g;
            // 🔴 LƯỢT 34 (muc 1 — DO): in TAN GO dung hinh. Handler chay tren UI luong,
            // ngay SAU khi UI thoat khoi nop >700ms ⇒ dong nay co timestamp [UI] de
            // chong voi log: nop nam giua dong [UI] TRUOC no va chinh no.
            static const bool kDbg = !qEnvironmentVariableIsEmpty("TORREADER_GUILOCK");
            if (kDbg && g > 700)
                qDebug().noquote() << "[FREEZE] gap=" << g << "ms step=" << curStep;
        });
        freeze.start(20);
        auto reportFreeze = [&]() {
            if (curStep == '?') return;
            fprintf(stdout, "OWNER3TAB freeze_max step=%c lap=%d ms=%lld ok=%d\n",
                    curStep, curLap, (long long)freezeMax, freezeMax <= 1000 ? 1 : 0);
            fflush(stdout);
            if (freezeMax > 1000)
                noteFail(QStringLiteral("lap %1: step %2 dung hinh %3ms > 1000ms")
                             .arg(curLap).arg(curStep).arg((long long)freezeMax));
            freezeMax = 0; tickT.restart();
        };
        auto step = [&](char s, int lap) {
            reportFreeze();   // dong bang dong ding cua buoc TRUOC do
            curStep = s; curLap = lap;
            // 🔴 LƯỢT 28 (VIỆC 1a): them hp_big_ns + tom tat heap — neu hp_big_ns
            // tang theo lap ⇒ phan manh khoi lon (gia thuyet CEO), bang thi loai heap.
            long long nh = -1, fk = -1, fb = -1;
            const long long hbig = heapBigNs();
            heapSummary(nh, fk, fb);
            fprintf(stdout, "OWNER3TAB step=%c lap=%d rss=%lldMB commit=%lldMB cpu_ns16M=%lld hp_ns100k=%lld"
                            " hp_big_ns=%lld nHeaps=%lld hsFreeKB=%lld hsFreeBlocks=%lld"
                            " docMo=%d pageMo=%d poolMo=%d\n",
                    s, lap, rssMB(), commitMB(), cpuNs(), heapNs(),
                    hbig, nh, fk, fb,
                    g_pdfiumDocOpen.loadRelaxed() - g_pdfiumDocClose.loadRelaxed(),
                    g_pdfiumPageOpen.loadRelaxed() - g_pdfiumPageClose.loadRelaxed(),
                    g_pdfiumPoolOpen.loadRelaxed() - g_pdfiumPoolClose.loadRelaxed());
            fflush(stdout);
            noteCommit("step");   // 🔴 LƯỢT 30: trần RAM 3000 MB ở mọi bước
        };
        // 🔴 LƯỢT 27 (săn rò): do tai lieu da DONG HET (docMo=0) thi in CAN LEAK:
        // rss truoc/sau EmptyWorkingSet + commit + so handle PageCache con song.
        // commit thấp + RSS sau trim ≈ commit ⇒ KHÔNG rò handle, chỉ là trang đã free
        // chưa trả OS. commit cao + doomed/orphan>0 ⇒ rò FPDF_PAGE cua tab đã đóng.
        auto leakCheck = [&](const char* tag, int lap) {
            const long long ws0 = rssMB(), cm = commitMB();
            const bool trimmed = trimWS();
            const long long ws1 = rssMB();
            fprintf(stdout, "OWNER3TAB LEAKCHECK tag=%s lap=%d ws_tr=%lldMB ws_sau_trim=%lldMB commit=%lldMB trim_ok=%d"
                            " docMo=%d poolMo=%d pageMo=%d liveDocs=%d pcEntries=%d pcDoomed=%d pcOrphan=%d\n",
                    tag, lap, ws0, ws1, cm, trimmed ? 1 : 0,
                    g_pdfiumDocOpen.loadRelaxed()  - g_pdfiumDocClose.loadRelaxed(),
                    g_pdfiumPoolOpen.loadRelaxed() - g_pdfiumPoolClose.loadRelaxed(),
                    g_pdfiumPageOpen.loadRelaxed() - g_pdfiumPageClose.loadRelaxed(),
                    PdfCloseTrace::liveDocs(),
                    PageCache::entryCount(), PageCache::doomedCount(), PageCache::orphanCount());
            fflush(stdout);
            // 🔴 LƯỢT 30: sau khi đóng HẾT tab (docMo=0) commit phải về gần mức khởi động.
            noteCommit(tag);
            const int docMo = g_pdfiumDocOpen.loadRelaxed() - g_pdfiumDocClose.loadRelaxed();
            if (docMo == 0 && commitBaseline >= 0 && cm > commitBaseline * 3 / 2 + 300)
                noteFail(QStringLiteral("%1: commit %2MB > 1,5x khoi dong (%3)+300 — khong ve sau khi dong het tab")
                             .arg(tag).arg(cm).arg(commitBaseline));
        };
        fprintf(stdout, "OWNER3TAB do_anh=ContinuousView::m_pageImages (kho cua 'cont ACCEPT')\n");
        fflush(stdout);

        // Đếm [lockhold] ms= qua handler CHUỖI: giữ nguyên logHandler của
        // installTextLog (gọi lại handler trước). Handler nổ trên nhiều luồng ⇒ atomic.
        static QAtomicInteger<long long> lhMax{0};
        static QAtomicInteger<int>       lhOver{0};
        static QtMessageHandler          prevHd = nullptr;
        prevHd = qInstallMessageHandler([](QtMsgType tp, const QMessageLogContext& ctx, const QString& msg) {
            if (msg.startsWith(QLatin1String("[lockhold] ms="))) {
                const long long v = msg.section(QLatin1Char(' '), 2, 2).toLongLong();
                if (v > lhMax.loadRelaxed()) lhMax.storeRelaxed(v);
                if (v > 2000) lhOver.fetchAndAddRelaxed(1);
            }
            // 🔴 LƯỢT 21: "page= N objects= M" → mục 3 và 5 (qDebug tach token bang khoang trang).
            if (msg.startsWith(QLatin1String("[heavycap] pre-count"))) {
                const int pg = msg.section(QLatin1Char(' '), 3, 3).toInt();
                const long long n = msg.section(QLatin1Char(' '), 5, 5).toLongLong();
                if (pg >= 0 && pg < 512) pageObj[pg].storeRelaxed(n);
            }
            if (prevHd) prevHd(tp, ctx, msg);   // ponytail: race ~vài ns giữa install và gán prevHd — probe-only, chấp nhận
        });

        static const int kPages[] = {3, 4, 5, 6, 8, 10, 20};
        // Do mot trang toi khi m_pageImages co anh — tran 30 s/trang (LƯỢT 21: trang
        // ≥1M object duoc phep toi 30 s; truong hop khong anh bao gio den van bi bat),
        // de phong lenh cuon roi (rescroll moi 500 ms). In SO OBJECT canh moi dong page=.
        auto measurePage = [&](int p, qint64* msOut) -> bool {
            { QElapsedTimer bt; bt.start(); w.probeScrollToPage(p);
              if (bt.elapsed() > 100) qDebug().noquote() << "[BLOCK] measurePage-scrollToPage ms=" << bt.elapsed(); }
            const long long pf0 = pfCount();          // 0928 LƯỢT 23 (DO): loi trang
            QElapsedTimer et; et.start();
            int lastRescroll = 0;
            while (et.elapsed() < 30000) {
                spin(50);
                if (w.probePageHasImage(p)) break;
                if (et.elapsed() - lastRescroll >= 500) {
                    w.probeScrollToPage(p); lastRescroll = (int)et.elapsed();
                }
            }
            const qint64 ms = et.elapsed();
            const long long nObj = (p >= 0 && p < 512) ? pageObj[p].loadRelaxed() : 0;
            const bool img = w.probePageHasImage(p);
            fprintf(stdout, "OWNER3TAB page=%d ms=%lld objs=%lld ok=%d rss=%lldMB fault=%lld\n",
                    p, (long long)ms, nObj, img ? 1 : 0, rssMB(), pfCount() - pf0);
            fflush(stdout);
            *msOut = ms;
            return img;
        };
        // 🔴 LƯỢT 19 (CEO 18.7): lap chay HET A-F, KHONG dung som khi B fail —
        // C/D/E/F la duong dong tab (cho văng), tat ca lap deu phai chay; PASS/FAIL
        // tong in cuoi. Ly do chi ghi lan dau.
        // 🔴 LƯỢT 21: BO MỐC 0 + nguong "baseline × 1.5" — trang 3 Phần ngầm có
        // 2,18M object, 10–15 s là BẢN CHẤT trang, không phải tranh khoá. Ngưỡng
        // mới theo SỐ OBJECT: <1M obj ⇒ 3 s; ≥1M obj ⇒ 30 s. Thêm bước G (ca crash).
        // 🔴 LƯỢT 30: mức commit LÚC KHỞI ĐỘNG (chưa mở file nào) — mốc so "về sau khi
        // đóng hết tab". Đo ngay trước vòng lap, khi docMo=0.
        commitBaseline = commitMB();
        fprintf(stdout, "OWNER3TAB commit_baseline=%lldMB\n", commitBaseline); fflush(stdout);
        // 🔴 LƯỢT 37 (mục A, reviewer lỗi 1) — bước O:
        //   Anh THU NHO nen cua tai lieu CU khong duoc ghi vao kho tai lieu MOI.
        //   Mo f1 + f3, zoom 200% trang 3 (f3) ⇒ phat mot luot thu nho nen; DOI NGAY sang f1
        //   va keo f1 ve trang 0 (f1 KHONG hien trang 3 ⇒ f1 khong tu ve no). Cho luot thu
        //   nho f3 ve. FIXED: guard docGen bo ⇒ m_pageImages[3] TRONG + staleDrops tang.
        //   BUG: anh f3 ghi nguoc vao kho f1 ⇒ con anh trang 3 ⇒ FAIL.
        auto runStepO = [&](int lap) {
            step('O', lap);
            w.openFile(f1);
            { QElapsedTimer dt; dt.start(); while (dt.elapsed() < 30000 && w.probePageCount() < 1) spin(100); }
            spin(3000);
            w.openFile(f3);
            { QElapsedTimer dt; dt.start(); while (dt.elapsed() < 30000 && w.probePageCount() < 21) spin(100); }
            w.probeActivateTab(w.probeTabCount() - 1); spin(300);   // f3 hien hanh
            // Trang 3 quai vat: mo o ZOOM THAP ⇒ anh DISK-CACHE ~4966px duoc phuc vu OVERSIZE
            // (needW nho) ⇒ acceptContinuousImage rui vao NHANH THU NHO NEN (QtConcurrent) — dung
            // cua sổ race reviewer lỗi 1 mô tả (giong het f1 page 0/1 "4000px -> 1100" trong log).
            // Thu vài muc zoom de chac chan phat duoc thu nho nen, ke ca khi cache lanh.
            QElapsedTimer it; it.start();
            bool inFlight = false;
            const double zls[] = { 0.4, 0.25, 0.6, 1.0 };
            for (double z : zls) {
                if (inFlight) break;
                w.probeSetZoom(z); spin(150);
                w.probeScrollToPage(3);
                for (int k = 0; k < 60 && !inFlight; ++k) {   // toi da ~6 s/zoom
                    if (w.probeContDownscaleInFlight() > 0) { inFlight = true; break; }
                    w.probeScrollToPage(3);
                    spin(100);
                }
            }
            const qint64 drops0 = w.probeContStaleDrops();
            w.probeActivateTab(0);                   // DOI NGAY sang f1 (giữa cửa sổ thu nhỏ)
            w.probeScrollToPage(0);                  // f1 ve trang 0 — khong yeu cau ve trang 3
            spin(6000);                              // du cho luot thu nho f3 ve toi finished+guard
            const bool p3clean = !w.probePageHasImage(3);
            const qint64 dropsSau = w.probeContStaleDrops();
            const bool okO = inFlight && p3clean && (dropsSau > drops0);
            fprintf(stdout, "OWNER3TAB O inFlight=%d p3clean=%d staleDrops=%lld ok=%d\n",
                    inFlight ? 1 : 0, p3clean ? 1 : 0, (long long)(dropsSau - drops0), okO ? 1 : 0);
            fflush(stdout);
            if (!okO)
                noteFail(QStringLiteral("O: thu nhỏ nen tai lieu cu ghi vao tab moi (inFlight=%1 p3clean=%2 drops=%3)")
                             .arg(inFlight ? 1 : 0).arg(p3clean ? 1 : 0).arg((long long)(dropsSau - drops0)));
            w.probeSetZoom(1.0); spin(300);
            for (int n = w.probeTabCount(); n > 0; ) {   // don ve trang de cac buoc tiep chay nhu cu
                w.probeCloseTab(0);
                const int n2 = w.probeTabCount();
                if (n2 >= n) break;
                n = n2;
            }
            spin(2000);
            leakCheck("O", lap);
        };
        if (oOnly) { for (int lap = 1; lap <= laps; ++lap) runStepO(lap); }
        if (!jklOnly && !oOnly)   // 🔴 LƯỢT 33a: OWNER3TAB_ONLY=JKL bo qua A–I/G/H
        for (int lap = 1; lap <= laps; ++lap) {
            // A. mở 3 tab đúng thứ tự, đúng cửa sổ thời gian kịch bản owner
            step('A', lap);
            w.openFile(f1); spin(8000);
            w.openFile(f2); spin(6000);   // MEP còn vẽ thumbnail nền — cố ý không chờ
            w.openFile(f3);
            // Chờ doc tab3 mở xong (async) — đo 28/09: cuộn lúc doc chưa mở là no-op,
            // view ở lại trang 0 và page 3 "timeout" GIẢ. 30 s trần.
            { QElapsedTimer dt; dt.start();
              while (dt.elapsed() < 30000 && w.probePageCount() < 21) spin(100);
              fprintf(stdout, "OWNER3TAB doc3 pages=%d ms=%lld\n",
                      w.probePageCount(), (long long)dt.elapsed()); fflush(stdout);
            }
            // 🔴 LƯỢT 22b (reviewer mục 1): BẮT LỖI THIẾU `!` ở tabAlive. Bản lỗi:
            //   MainWindow.cpp:5261 `if (tabAlive(tab,s)) return;` — initWatcher thoát
            //     sớm trên tab CÒN SỐNG ⇒ pdfHash không bao giờ được gán ⇒ ok=0.
            //   MainWindow.cpp:6262/6294/6301 — lambda quét comment thoát sớm ⇒
            //     annotCache không bao giờ nạp ⇒ panel Comments trống ⇒ comments=0.
            // Probe cũ (r22) PASS vì không soi hai đường này. f1 (site plan Checked)
            // KHÔNG có /Annots trên đĩa (đã đếm thô 0) — nên probe TỰ tạo một note
            // bằng API thật (probeCreateNote) rồi mới quét: có annot chắc chắn để
            // bản lỗi phải lộ. Note là nhân vật chứng ⇒ xoá cờ dirty sau khi đo.
            {
                w.probeActivateTab(0); spin(300);
                const quint64 h = w.probePdfHash(0);
                fprintf(stdout, "OWNER3TAB pdfHash ok=%d\n", h != 0 ? 1 : 0); fflush(stdout);
                if (h == 0)
                    noteFail(QStringLiteral("lap %1: f1 pdfHash rong — initWatcher tabAlive thieu `!`").arg(lap));
                const bool noteOk = w.probeCreateNote(0, 100.0, 100.0);
                const int real = w.probeRealAnnotCount(0);
                w.probeRequestComments();
                QElapsedTimer ct; ct.start();
                while (ct.elapsed() < 30000 && w.probeCommentCache(0) <= 0) spin(100);
                const int cached = w.probeCommentCache(0);
                fprintf(stdout, "OWNER3TAB comments f1=%d real=%d note=%d\n", cached, real, noteOk ? 1 : 0);
                fflush(stdout);
                if (!noteOk)
                    noteFail(QStringLiteral("lap %1: probeCreateNote FAIL — khong dung duoc tinh huong co annot").arg(lap));
                else if (cached <= 0)
                    noteFail(QStringLiteral("lap %1: file co annot (note=%2 real=%3) nhung panel cache=%4 — scan lambda thieu `!`")
                                 .arg(lap).arg(noteOk ? 1 : 0).arg(real).arg(cached));
                w.probeClearDirty(0);
                w.probeActivateTab(2); spin(300);   // tra ve tab3 cho buoc B
            }
            // B. cuộn tab3 (tab hiện hành, index 2), đo từng trang TỚI KHI XONG hết
            //    kPages — lượt 19/20 chỉ đo trang 3 rồi break vì ngưỡng baseline;
            //    ngưỡng mới theo object nên các trang nhẹ vẫn phải qua cửa 3 s.
            step('B', lap);
            // 0928 LƯỢT 26 (VIỆC 1): do CPU toan process truoc/sau buoc B.
            const long long pcB0 = procCpuMs();
            QElapsedTimer wallB; wallB.start();
            for (int p : kPages) {
                qint64 ms = 0;
                if (!measurePage(p, &ms)) {
                    noteFail(QStringLiteral("lap %1: page %2 khong co anh trong 30s").arg(lap).arg(p));
                    break;   // trang khong bao gio co anh => cuon tiep vo nghia
                }
                const long long nObj = (p >= 0 && p < 512) ? pageObj[p].loadRelaxed() : 0;
                const qint64 limit = (nObj >= 1000000) ? 30000 : 3000;
                if (ms >= limit)
                    noteFail(QStringLiteral("lap %1: page %2 %3ms >= %4ms (objs=%5)")
                                 .arg(lap).arg(p).arg((long long)ms).arg((long long)limit).arg(nObj));
            }
            fprintf(stdout, "OWNER3TAB proc_cpu step=B lap=%d ms=%lld wall=%lld\n",
                    lap, procCpuMs() - pcB0, (long long)wallB.elapsed());
            fflush(stdout);
            // C. đóng tab3 NGAY khi MEP còn chạy nền (không chờ)
            step('C', lap);
            w.probeCloseTab(2); spin(3000);
            fprintf(stdout, "OWNER3TAB step=C tabs_con_lai=%d\n", w.probeTabCount()); fflush(stdout);
            leakCheck("C", lap);   // 🔴 LƯỢT 27: ngay sau khi DONG tab Phan ngam
            // D. về tab0, đóng tab NỀN (MEP index 1 — không phải tab hiện hành),
            //    cuộn tab0 vài trang: paint SAU khi đóng tab nền (nhánh L16.2 chưa ai bắt)
            step('D', lap);
            w.probeActivateTab(0);
            w.probeCloseTab(1); spin(3000);
            fprintf(stdout, "OWNER3TAB step=D tabs_con_lai=%d\n", w.probeTabCount()); fflush(stdout);
            for (int q : {1, 3, 5, 7}) { w.probeScrollToPage(q); spin(400); }
            // E. mở lại f2+f3, đóng 2 tab LIÊN TIẾP không chờ (m_thumbCloseJobs=2)
            step('E', lap);
            w.openFile(f2); w.openFile(f3);
            w.probeCloseTab(2); w.probeCloseTab(1); spin(5000);
            fprintf(stdout, "OWNER3TAB step=E tabs_con_lai=%d\n", w.probeTabCount()); fflush(stdout);
            // F. đóng hết tab còn lại. Welcome KHÔNG phải DocTab ⇒ onTabClose(0)
            //    không làm gì nó — chỉ đi tiếp khi số tab thực giảm (chống lặp vô hạn).
            step('F', lap);
            for (int n = w.probeTabCount(); n > 0; ) {
                w.probeCloseTab(0);
                const int n2 = w.probeTabCount();
                if (n2 >= n) break;
                n = n2;
            }
            spin(2000);
            fprintf(stdout, "OWNER3TAB step=F tabs_con_lai=%d\n", w.probeTabCount()); fflush(stdout);
            leakCheck("F", lap);   // 🔴 LƯỢT 27: sau khi DONG HET tab (docMo phai=0)
            // 🔴 0928 LƯỢT 25 mục 3 — THÍ NGHIỆM (chi do, co env TORREADER_REINIT_IDLE=1,
            // mac dinh TAT): sau khi tai lieu cuoi dong xong (moi closeJob xong, khong
            // pool/doc/page mo — dung bo dem san co + PdfCloseTrace::liveDocs), destroy
            // + init lai PDFium tren UI luong, duoi s_pdfiumMutex (libReinitIdle).
            // So lap2 p3 + RSS step A lap 2 voi base khong co khi.
            if (qEnvironmentVariableIsSet("TORREADER_REINIT_IDLE")) {
                // pageMo AM (pageOpen<pageClose, ~-111) la anomaly dem-kap co tu
                // truoc (PageCache dong trang khong tang giu lenh) => dung can bang
                // pageOpen==pageClose thi VONG LUON tran 60s va destroy duoi tai.
                // Dao an idle that = closeJobs het + doc/pool can bang + liveDocs=0.
                QElapsedTimer rt; rt.start();
                bool idle = false;
                while (rt.elapsed() < 20000) {
                    spin(100);
                    if (w.probeCloseJobs() == 0 &&
                        g_pdfiumDocOpen.loadRelaxed()  == g_pdfiumDocClose.loadRelaxed() &&
                        g_pdfiumPoolOpen.loadRelaxed() == g_pdfiumPoolClose.loadRelaxed() &&
                        PdfCloseTrace::liveDocs() == 0) { idle = true; break; }
                }
                const qint64 rss0 = rssMB();
                const bool done = idle && PdfDocument::libReinitIdle();
                spin(500);
                fprintf(stdout, "OWNER3TAB reinit idle=%d wait=%lldms rss_tr=%lldMB rss_sau=%lldMB\n",
                        done ? 1 : 0, (long long)rt.elapsed(), (long long)rss0, (long long)rssMB());
                fflush(stdout);
            }
            // 🔴 0928 LƯỢT 25 mục 5 — buoc I (co env OWNER3TAB_STEP_I=1): ca thuc te
            // cua owner — GIU tab f1 mo, dong f3 roi MO LAI f3, do lai trang 3.
            // KHONG phai luc nao cung dong het tab (nhánh F khong bao gio chay).
            if (qEnvironmentVariableIsSet("OWNER3TAB_STEP_I")) {
                step('I', lap);
                w.openFile(f1); spin(8000);
                w.openFile(f3);
                { QElapsedTimer dt; dt.start();
                  while (dt.elapsed() < 30000 && w.probePageCount() < 21) spin(100); }
                w.probeActivateTab(w.probeTabCount() - 1); spin(300);
                qint64 mI1 = 0; measurePage(3, &mI1);
                fprintf(stdout, "OWNER3TAB stepI page=3 ms=%lld (lan1, f1 van mo)\n", (long long)mI1);
                fflush(stdout);
                // 🔴 LƯỢT 37 muc C: do DUNG LENH TRUC TIEP nay (khong qua notify ⇒ [slotms]
                // khong bat duoc) de quy nguyen 2.4 s dung hinh step I cho ai.
                auto blkI = [](const char* what, auto&& fn) {
                    QElapsedTimer t; t.start(); fn();
                    const qint64 ms = t.elapsed();
                    if (ms > 100) qDebug().noquote() << "[BLOCK]" << what << "ms=" << ms;
                };
                blkI("probeCloseTab", [&]{ w.probeCloseTab(w.probeTabCount() - 1); });
                spin(3000);   // dong f3, f1 CON mo
                blkI("openFile-f3", [&]{ w.openFile(f3); });
                { QElapsedTimer dt; dt.start();
                  while (dt.elapsed() < 30000 && w.probePageCount() < 21) spin(100); }
                blkI("activateTab", [&]{ w.probeActivateTab(w.probeTabCount() - 1); });
                spin(300);
                qint64 mI2 = 0; measurePage(3, &mI2);
                fprintf(stdout, "OWNER3TAB stepI page=3 ms=%lld (lan2, mo-lai-khi-chua-dong-het)\n", (long long)mI2);
                fflush(stdout);
                // 🔴 LƯỢT 31b (reviewer mục 2): ZOOM LỚN KHÔNG MỜ KẸT. Mở lại f3 ⇒ cache
                // hit, ảnh trang 3 chỉ ~357px (scale thật ~0,6). Zoom 200% ⇒ trong ≤ 20 s
                // ảnh trang phải có renderedScale >= 0,9×2,0 = 1,8 (nhãn full-quality giả
                // trên ảnh tạm ⇒ ContinuousView không vẽ lại ⇒ kẹt mờ ⇒ FAIL ở đây).
                w.probeSetZoom(2.0); spin(300);
                w.probeScrollToPage(3);
                QElapsedTimer zt; zt.start();
                double zScale = -1.0;
                while (zt.elapsed() < 20000) {
                    zScale = w.probePageImageScale(3);
                    if (zScale >= 2.0 * 0.9) break;
                    spin(200);
                }
                fprintf(stdout, "OWNER3TAB zoomsharp page=3 ms=%lld scale=%.3f ok=%d\n",
                        (long long)zt.elapsed(), zScale, zScale >= 2.0 * 0.9 ? 1 : 0);
                fflush(stdout);
                if (zScale < 2.0 * 0.9)
                    noteFail(QStringLiteral("lap %1: zoom 200%% page 3 mo ket sau %2ms (scale=%3 < 1.8)")
                                 .arg(lap).arg((long long)zt.elapsed()).arg(zScale, 0, 'f', 3));
                w.probeSetZoom(1.0); spin(300);
                for (int n = w.probeTabCount(); n > 0; ) {   // don ve trang de G/H chay nhu cu
                    w.probeCloseTab(0);
                    const int n2 = w.probeTabCount();
                    if (n2 >= n) break;
                    n = n2;
                }
                spin(2000);
                leakCheck("I", lap);   // 🔴 LƯỢT 27: sau buoc I (dong mo-lai f3), docMo phai=0
            }
            if (qEnvironmentVariableIsSet("OWNER3TAB_STEP_O")) runStepO(lap);
            // G. 🔴 LƯỢT 21 — CA CRASH NÀY (dump 32832): mo f2 (244 MB) roi dong tab
            //    NGAY LAP TUC (open() con chay tren pool — `dongTab ms=0`). 3 lan
            //    lien tiep, KHONG spin giua open va close. closeJob nen phai
            //    waitForFinished(openFuture) truoc `delete t`; dong nghia UI khong
            //    duoc dung hinh (freeze_max step=G <= 1000 ms).
            step('G', lap);
            for (int k = 1; k <= 3; ++k) {
                w.openFile(f2);
                w.probeCloseTab(w.probeTabCount() - 1);
                QElapsedTimer gt; gt.start();
                while (gt.elapsed() < 60000 && w.probeCloseJobs() > 0) spin(100);
                fprintf(stdout, "OWNER3TAB step=G try=%d ms=%lld tabs_con_lai=%d closeJobs=%d\n",
                        k, (long long)gt.elapsed(), w.probeTabCount(), w.probeCloseJobs());
                fflush(stdout);
            }
            // H. 🔴 LƯỢT 22 — THOÁT CỬA SỔ LÚC open() CÒN CHẠY: MainWindow THỨ HAI
            //    trên heap, openFile(f2) (244 MB — open ngốn giây trên pool), rồi
            //    `delete` NGAY ⇒ ~MainWindow chạy giữa chừng: shutdownTab + chờ
            //    openFuture TRÊN UI (1285) + `delete t` đồng bộ. Đây là đường
            //    G không phủ ("đóng tab" ≠ "đóng cả cửa sổ"). Lặp 2 lần/lap.
            //    (freeze_max của H được reset quanh delete: ~MainWindow CHỜ mở xong
            //    là HÀNH VI ĐÚNG — app đang tắt — không tính là đứng hình.)
            step('H', lap);
            for (int k = 1; k <= 2; ++k) {
                QElapsedTimer ht; ht.start();
                auto* w2 = new MainWindow;
                w2->resize(900, 700);
                w2->show();
                w2->openFile(f2);
                spin(300);                 // cho open() kịp xếp hàng vào pool
                tickT.restart();           // đếm lại từ trước delete
                delete w2;                 // ~MainWindow chờ openFuture — cửa sổ chết
                const qint64 hms = ht.elapsed();
                freezeMax = 0; tickT.restart();   // H chờ CÓ CHỦ ĐÍCH — không gate freeze
                fprintf(stdout, "OWNER3TAB step=H try=%d delete_ms=%lld song\n",
                        k, (long long)hms);
                fflush(stdout);
            }
        }

        // ═══ LƯỢT 33a — BÀI ĐO J/K/L (CHỈ ĐO, KHÔNG SỬA HÀNH VI APP) ═══════════════
        // Owner 29/09 trên r31b: (1) tab nặng đang render ⇒ tab mới mở không load nổi
        // [J]; (2) đóng tab ⇒ chặn UI / văng [K]; (3) đổi tab ⇒ NHẢY VỀ TRANG 1 [L].
        // Chạy riêng: OWNER3TAB_ONLY=JKL. Chạy đủ: sau A–I/H. Không dừng sớm khi FAIL.
        // LƯỢT 37: OWNER3TAB_ONLY=O ⇒ bo J/K/L/M, chi giu ket luan cuoi.
        if (!oOnly) {
            auto closeAllTabs = [&]() {
                for (int n = w.probeTabCount(); n > 0; ) {
                    w.probeCloseTab(0);
                    const int n2 = w.probeTabCount();
                    if (n2 >= n) break;
                    n = n2;
                }
                spin(2000);
            };
            auto waitDoc = [&](int minPages) -> long long {
                QElapsedTimer dt; dt.start();
                while (dt.elapsed() < 30000 && w.probePageCount() < minPages) spin(100);
                return (long long)dt.elapsed();
            };
            // Chup CA CUA SO (nghiem thu GUI phai nhin thay trang hien hanh).
            auto shot = [&](const char* buoc) {
                const QString path = QStringLiteral("C:/Users/Public/jkl_%1.png").arg(buoc);
                const bool sv = w.grab().save(path);
                fprintf(stdout, "OWNER3TAB shot=%s ok=%d\n",
                        path.toLocal8Bit().constData(), sv ? 1 : 0); fflush(stdout);
            };
            // ── J: mo f3, cuon trang 3 (quai vat) KHONG cho; +300 ms mo f2 ⇒ do ms
            //    toi khi trang 0 CUA F2 co anh (can probePageCount()>=1 de khong dem
            //    nhiam anh trang 0 cua f3 van con trong view dung chung).
            // 🔴 LƯỢT 33d: nguong 1500 ms (muc tieu CEO). Trang 3 PHAI duoc ve LAN
            //    DAU — dong het tab (release file cache) roi XOA cache dia cua f3
            //    (.torcache anh nen + .torvec vector, key = chinh app sinh ra) truoc
            //    khi mo; in `J cache_f3=cold` lam bang chung.
            step('J', 1);
            closeAllTabs();
            {
                // 🔴 LƯỢT 33e (muc 5): ten file cache cua f3 CO CHU TIENG VIET
                // ("PHẦN CHUNG & PHẦN NGẦM - Copy.pdf_<hex>.torcache/_p<N>.torvec").
                // r33d dung entryList(pattern) — pattern qua 8-bit tren Windows khong
                // bao gio khop ten co dau (removed=0). Den bu: Liet ke TOAN BO temp
                // bang QString, so sanh startsWith/endsWith truc tiep (Unicode day du,
                // khong qua pattern/8-bit). In base bang UTF-8 de log doc duoc.
                const QString base = QFileInfo(f3).fileName() + QLatin1Char('_');
                QDir td(QDir::temp());
                int removed = 0;
                const QStringList all = td.entryList(QDir::Files);
                for (const QString& fn : all) {
                    if (!fn.startsWith(base, Qt::CaseInsensitive)) continue;
                    if (!fn.contains(QLatin1String(".torcache"), Qt::CaseInsensitive)
                        && !fn.contains(QLatin1String(".torvec"), Qt::CaseInsensitive))
                        continue;
                    if (QFile::remove(td.absoluteFilePath(fn))) ++removed;
                }
                const int anyCache = td.entryList(QStringList()
                        << QStringLiteral("*.torcache") << QStringLiteral("*.torvec"),
                        QDir::Files).size();
                fprintf(stdout, "OWNER3TAB J cache_f3=cold removed=%d tmp=%s anyCache=%d base=%s\n",
                        removed, QDir::tempPath().toUtf8().constData(),
                        anyCache, base.toUtf8().constData());
                fflush(stdout);
            }
            w.openFile(f3);
            const long long jd3 = waitDoc(21);
            fprintf(stdout, "OWNER3TAB J doc3=%lldms pages=%d\n",
                    jd3, w.probePageCount()); fflush(stdout);
            w.probeScrollToPage(3);            // noi dung ve quai vat — khong cho xong
            spin(300);
            QElapsedTimer jt; jt.start();
            w.openFile(f2);                    // tab MOI len dau — do tu luc nay
            qint64 jms = 30000; bool jImg = false;
            while (jt.elapsed() < 30000) {
                spin(50);
                if (w.probePageCount() >= 1 && w.probePageHasImage(0)) {
                    jms = jt.elapsed(); jImg = true; break;
                }
            }
            fprintf(stdout, "OWNER3TAB J mep_first_ms=%lld ok=%d tabs=%d\n",
                    (long long)jms, (jImg && jms <= 1500) ? 1 : 0, w.probeTabCount());
            fflush(stdout);
            if (!jImg)
                noteFail(QStringLiteral("J: trang 0 f2 khong co anh trong 30s (f3 dang ve trang 3)"));
            else if (jms > 1500)
                noteFail(QStringLiteral("J: mep_first_ms=%1 > 1500 — tab nang chan tab moi").arg((long long)jms));
            // ── K: f3 DANG ve trang 3 (giua luot) ⇒ dong tab ⇒ do ms UI CHAN qua
            //    khe giua 2 tick freeze 20ms. FAIL > 500 ms. NEU VĂNG: process chet
            //    TRUOC khi in dong OWNER3TAB K — thieu dong nay tren log la bang chung.
            //    (Thu 'mo f2 roi dong ngay khi dang load' da co buoc G — gi nguyen.)
            step('K', 1);
            w.probeActivateTab(0);             // ve f3 (tab 0; f2 dang hien hanh)
            w.probeScrollToPage(3);            // noi dung ve giua luot
            spin(300);
            freezeMax = 0; tickT.restart();
            QElapsedTimer kt; kt.start();
            w.probeCloseTab(0);
            const qint64 kWall = kt.elapsed();
            spin(300);                         // cho tick freeze no de lay duoc khe
            const qint64 kBlock = freezeMax; freezeMax = 0;
            fprintf(stdout, "OWNER3TAB K close_ms=%lld wall=%lld tabs_con_lai=%d ok=%d\n",
                    (long long)kBlock, (long long)kWall, w.probeTabCount(),
                    (kBlock <= 500) ? 1 : 0); fflush(stdout);
            if (kBlock > 500)
                noteFail(QStringLiteral("K: UI bi chan %1ms khi dong tab dang ve > 500ms").arg((long long)kBlock));
            // ── L: vi tri tab khi doi tab. Continuous (mac dinh) + Single + ca truong
            //    hop tab DICH CON DANG LOAD. In trang hien + trang ky vong + offset
            //    tam viewport so voi luc da cuon. FAIL neu nhay ve trang khong phai
            //    trang da dat (0/1). Poll toi 5s de khong dem nham "cham render" = "nhay trang".
            step('L', 1);
            closeAllTabs();
            auto checkL = [&](const char* mode, int idx, const char* name, int expect,
                              double savedDy, int loading, const char* buoc) {
                const bool single = (mode[0] == 'S');
                const int loading0 = loading;   // in gia tri NGUYEN BAN (tab dang load)
                // 🔴 LƯỢT 33f (muc 5 — L f2 "hien trang 2 thay vi 0"): bang log 33f,
                // KHONG AI cuon sau restoreScrollY 0 (khong co [cont-scroll] nao giua
                // restore va do; [thumbq] scroll=0/59457). "page=2" la trang o TAM
                // viewport — trang thap hon khung nhin (zoom 0.1) nen tam roi trang 2
                // DU VIEW DUNG O DINH. Voi tab CHUA TUNG cuon, vi tri DUNG = dinh:
                // do scrollY THAT, khong do trang tam.
                const bool dinh = loading0 && !single;
                w.probeActivateTab(idx);
                QElapsedTimer lt; lt.start();
                int pg = -1; QPointF c;
                if (loading) {
                    // 🔴 LƯỢT 33d (mục 3): "tab dang load" — do sau khi doc MO XONG
                    // (duong 5415+ da setDocument + restoreScrollY). Do MOT lan luc
                    // doc chua mo la do gia dinh cua view (noi dung tab cu), khong
                    // phai loi — loi THAT la vi tri SAI sau khi load xong.
                    while (lt.elapsed() < 15000 && w.probePageCount() < 1) spin(100);
                    spin(400);                 // cho setDocument + layout + ve
                    loading = 0;               // poll nhu tab da mo
                }
                for (;;) {
                    spin(100);
                    c = QPointF();
                    if (single) pg = w.probeTabShownPage(idx);
                    else        w.probeContCenter(&pg, &c);
                    const bool dat = dinh ? (w.probeContScrollY() == 0) : (pg == expect);
                    if (loading || dat || lt.elapsed() >= 5000) break;
                }
                const bool ok = dinh ? (w.probeContScrollY() == 0) : (pg == expect);
                fprintf(stdout, "OWNER3TAB L mode=%s tab=%s(%d) page=%d expect=%d saved=%d"
                                " dy=%.1f saved_dy=%.1f wait=%lldms loading=%d ok=%d\n",
                        mode, name, idx, pg, expect, w.probeTabSavedPage(idx),
                        c.y(), savedDy, (long long)lt.elapsed(), loading0, ok ? 1 : 0);
                fflush(stdout);
                shot(buoc);
                if (!ok && !loading)
                    noteFail(QStringLiteral("L %1: doi sang tab %2 ra trang %3 (expect %4) — khong giu vi tri")
                                 .arg(mode).arg(name).arg(pg).arg(expect));
            };
            // Cuon + de view LANG (scrollToPage animate; trang A0 loai 3400px can
            // re-scroll nhu measurePage). Tra ve trang THUC O TAM viewport — do chinh
            // la "vi tri da dat" ma 4 lan doi tab phai giu (nominal co the khac khi
            // trang nho hon viewport: tam viewport nam o trang ke tiep).
            auto contSettle = [&](int nominal, double* dyOut) -> int {
                QElapsedTimer t; t.start();
                int pg = -1, last = -999, stable = 0;
                QPointF c;
                for (;;) {
                    w.probeScrollToPage(nominal);
                    spin(500);
                    if (!w.probeContCenter(&pg, &c)) pg = -1;
                    if (pg == nominal) break;
                    if (pg == last) { if (++stable >= 3) break; } else stable = 0;
                    last = pg;
                    if (t.elapsed() > 15000) break;
                }
                *dyOut = c.y();
                return pg;
            };
            // L — Continuous: f1→trang 5, f3→trang 20, doi f1→f3→f1→f3.
            w.openFile(f1);
            const long long ld1 = waitDoc(6);
            fprintf(stdout, "OWNER3TAB L doc1=%lldms pages=%d\n",
                    ld1, w.probePageCount()); fflush(stdout);
            double dyF1 = 0.0, dyF3 = 0.0;
            const int expF1 = contSettle(5, &dyF1);
            fprintf(stdout, "OWNER3TAB L setup mode=Cont tab=f1 nominal=5 reached=%d saved=%d dy=%.1f\n",
                    expF1, w.probeTabSavedPage(0), dyF1); fflush(stdout);
            w.openFile(f3);
            const long long ld3 = waitDoc(21);
            fprintf(stdout, "OWNER3TAB L doc3=%lldms pages=%d\n",
                    ld3, w.probePageCount()); fflush(stdout);
            const int expF3 = contSettle(20, &dyF3);
            fprintf(stdout, "OWNER3TAB L setup mode=Cont tab=f3 nominal=20 reached=%d saved=%d dy=%.1f\n",
                    expF3, w.probeTabSavedPage(1), dyF3); fflush(stdout);
            checkL("Cont", 0, "f1", expF1, dyF1, 0, "L_cont_1");
            checkL("Cont", 1, "f3", expF3, dyF3, 0, "L_cont_2");
            checkL("Cont", 0, "f1", expF1, dyF1, 0, "L_cont_3");
            checkL("Cont", 1, "f3", expF3, dyF3, 0, "L_cont_4");
            // L — Continuous, tab dich CON DANG LOAD: mo f2 khong cho, doi di roi doi lai.
            w.openFile(f2);                    // tab 2 = f2 dang load
            w.probeActivateTab(0); spin(300);  // roi sang f1 khi f2 chua mo xong
            checkL("Cont", 2, "f2", 0, 0.0, 1, "L_cont_load");      // doi sang tab dang load
            waitDoc(1);                        // tab hien hanh = f2 — cho mo xong that
            checkL("Cont", 0, "f1", expF1, dyF1, 0, "L_cont_back");  // f1 van giu vi tri?
            // L — Single: moi tab co PdfGpuView rieng; do trang view THUC VE.
            w.probeSetView(false, 100.0, 1); spin(500);
            w.probeActivateTab(0); w.probeSetPage(5);  spin(500);
            w.probeActivateTab(1); w.probeSetPage(20); spin(500);
            fprintf(stdout, "OWNER3TAB L setup mode=Single f1 saved=%d f3 saved=%d\n",
                    w.probeTabSavedPage(0), w.probeTabSavedPage(1)); fflush(stdout);
            checkL("Single", 0, "f1", 5,  0.0, 0, "L_single_1");
            checkL("Single", 1, "f3", 20, 0.0, 0, "L_single_2");
            checkL("Single", 0, "f1", 5,  0.0, 0, "L_single_3");
            checkL("Single", 1, "f3", 20, 0.0, 0, "L_single_4");
            // L — Single, tab dich dang load.
            w.probeCloseTab(2); spin(1500);
            w.openFile(f2);
            w.probeActivateTab(0); spin(300);
            checkL("Single", 2, "f2", 0, 0.0, 1, "L_single_load");
            waitDoc(1);
            w.probeSetView(true, 100.0, 1); spin(300);   // tra ve Continuous mac dinh
            closeAllTabs();
            // ── M (LƯỢT 33e — BẮT BUỘC vĩnh viễn): markup + comment của f1 PHẢI còn
            //    nguyên sau mỗi lần đổi tab. Ba lỗi reviewer bắt ở 33b/33d (visuals
            //    rỗng bị cache, scan chết không ai spawn lại, stopScan lây lúc
            //    activeDoc null) đều biểu hiện đúng bằng HIỆN TƯỢNG này — đếm bằng
            //    getter CHỈ-ĐỌC: overlay visuals của trang hiện (kho mà CẢ HAI view
            //    vẽ từ đó) + số comment trong panel. Chuyển f1→f3→f1 NHANH (≤200 ms)
            //    và CHẬM (2 s), 3 lần; kèm case mở tab mới (activeDoc null giữa
            //    chuyển tiếp) rồi quay về f1. Sau mỗi lần chờ ≤3 s phải bằng mốc đầu.
            step('M', 1);
            w.openFile(f1); waitDoc(6);
            w.openFile(f3); waitDoc(21);
            w.probeActivateTab(0); spin(400);
            if (w.probeCommentCache(0) <= 0) {          // JKL-only: buoc B chua tao note
                w.probeCreateNote(0, 100.0, 100.0);     // note tren trang 0 = trang dang hien
                w.probeClearDirty(0);
            }
            w.probeRequestComments();
            int baseV = 0, baseC = 0;
            { QElapsedTimer bt; bt.start();
              do { spin(100);
                   baseV = w.probeOverlayVisualCount(0); baseC = w.probeCommentCache(0); }
              while (bt.elapsed() < 10000 && (baseV < 1 || baseC < 1)); }
            fprintf(stdout, "OWNER3TAB M baseline visuals=%d comments=%d panel=%d\n",
                    baseV, baseC, w.probePanelCommentRows()); fflush(stdout);
            if (baseV < 1 || baseC < 1)
                noteFail(QStringLiteral("M: moc dau thieu visuals=%1 hoac comments=%2 (f1 da co note)").arg(baseV).arg(baseC));
            auto checkM = [&](int lap, const char* mode) {
                QElapsedTimer wt; wt.start();
                int v = -1, c = -1;
                while (wt.elapsed() <= 3000) {
                    v = w.probeOverlayVisualCount(0); c = w.probeCommentCache(0);
                    if (v == baseV && c == baseC) break;
                    spin(100);
                }
                const bool ok = (v == baseV && c == baseC);
                fprintf(stdout, "OWNER3TAB M lap=%d mode=%s visuals=%d/%d comments=%d/%d wait=%lldms ok=%d\n",
                        lap, mode, v, baseV, c, baseC, (long long)wt.elapsed(), ok ? 1 : 0);
                fflush(stdout);
                if (!ok)
                    noteFail(QStringLiteral("M lap %1 (%2): doi tab lam mat markup/comment (visuals=%3/%4 comments=%5/%6)")
                                 .arg(lap).arg(QString::fromUtf8(mode)).arg(v).arg(baseV).arg(c).arg(baseC));
            };
            for (int lap = 1; lap <= 3; ++lap) {
                w.probeActivateTab(1); spin(150);  w.probeActivateTab(0); checkM(lap, "fast");
                w.probeActivateTab(1); spin(2000); w.probeActivateTab(0); checkM(lap, "slow");
            }
            // Ca MO TAB MOI (activeDoc NULL giua chuyen tiep) roi quay ve f1:
            w.openFile(f2); spin(100);                   // f2 dang mo — activeDoc = null
            w.probeActivateTab(0); checkM(4, "newtab");
            waitDoc(1); spin(500);                       // de f2 mo xong roi don tab
            closeAllTabs();
            // ── N (LƯỢT 33h — stress đóng tab GIỮA lượt vẽ trang quái vật): lặp 20 lần
            //    { mở f3 → cuộn trang 3 (đang vẽ dở) → chờ NGẪU NHIÊN 0–1500 ms → đóng }.
            //    Đúng cửa sổ race dump r33g: closeJob nền đang shutdownHeavy/doc->close
            //    trong khi UI giao event cho renderer. 0 văng (process sống tới dòng PASS
            //    + Event Log sạch sau mỗi lần) mới coi là sửa.
            step('N', 1);
            for (int lap = 1; lap <= 20; ++lap) {
                closeAllTabs();
                w.openFile(f3);
                waitDoc(21);
                w.probeScrollToPage(3);                  // noi dung ve quai vat — KHONG cho xong
                spin(QRandomGenerator::global()->bounded(1500));
                w.probeCloseTab(0);                      // dong GIUA luot ve
                QElapsedTimer nt; nt.start();            // cho closeJob nen xong + UI `delete t`
                while (nt.elapsed() < 15000 && w.probeCloseJobs() > 0) spin(50);
                fprintf(stdout, "OWNER3TAB N lap=%d tabs_con_lai=%d closejobs=%d\n",
                        lap, w.probeTabCount(), w.probeCloseJobs()); fflush(stdout);
            }
            closeAllTabs();
        }

        reportFreeze();   // buoc cuoi cung (G) khong co buoc ke tiep dong bang ho
        fprintf(stdout, "OWNER3TAB lockhold_max=%lld lockhold_over2000=%d\n",
                (long long)lhMax.loadRelaxed(), lhOver.loadRelaxed());
        fflush(stdout);
        // 🔴 LƯỢT 30 (BẮT BUỘC): in trần RAM đã đo + mốc khởi động để so sánh giữa các bản.
        fprintf(stdout, "OWNER3TAB commit_max=%lldMB commit_baseline=%lldMB\n",
                commitMax, commitBaseline);
        fflush(stdout);
        qInstallMessageHandler(prevHd);   // gỡ hook, trả lại logHandler gốc
        // 🔴 LƯỢT 18 (VIỆC 4): lockhold_max > 2000 ms la FAIL (VONG_LAP_DEM muc 3).
        if (lhMax.loadRelaxed() > 2000)
            noteFail(QStringLiteral("lockhold_max %1ms > 2000ms").arg((long long)lhMax.loadRelaxed()));
        if (allOk) { fprintf(stdout, "OWNER3TAB: PASS\n"); fflush(stdout); return 0; }
        fprintf(stdout, "OWNER3TAB: FAIL %s\n", reason.toLocal8Bit().constData());
        fflush(stdout);
        return 2;
    }

    // ── LƯỢT 30 — RAM PROBE (VIỆC 1: ĐO để chỉ ra ai giữ ≥80% RAM) ──────────────
    //   --ram-probe <f3> <f2> [maxPages=75]
    // f3 = trang quái vật (PHẦN CHUNG & PHẦN NGẦM, 75 tr, trang 3 = 2,18M obj),
    // f2 = MEP 244 MB. Kịch bản chủ nhân: CHỈ 2 file mà RAM lên 10 GB.
    // Mở f3 → cuộn Continuous TỪNG trang 0..maxPages-1 (chờ có ảnh hoặc ≤15 s),
    // zoom 100% vài trang; mở f2 → cuộn ~40 trang. Sau mỗi 5 trang in: commit
    // (PrivateUsage), RSS, BẢNG PHÂN RÃ (pool doc từng dùng, globalCache ảnh,
    // PageCache, vector layers, thumbnails). CUỐI: đóng TỪNG pool doc RẢNH, đo
    // commit+RSS delta ⇒ ước lượng mỗi pool doc giữ bao nhiêu (nghi phạm A).
    // FAIL: commit > 3000 MB ở bất kỳ bước nào, hoặc sau khi đóng hết tab commit
    // không về dưới 1,5× lúc khởi động + 300 MB. In `OWNER3TAB commit_max=`.
    if (argc >= 4 && QCoreApplication::arguments().value(1) == QLatin1String("--ram-probe")) {
        const QStringList as = QCoreApplication::arguments();
        int cur = 2;
        auto nextPath = [&as, &cur]() -> QString {
            QString v;
            for (int j = cur; j < as.size(); ++j) {
                v = v.isEmpty() ? as.value(j) : v + QLatin1Char(' ') + as.value(j);
                cur = j + 1;
                if (!(v.startsWith(QLatin1Char('"')) && !v.endsWith(QLatin1Char('"')))) break;
            }
            if (v.size() > 1 && v.startsWith(QLatin1Char('"')) && v.endsWith(QLatin1Char('"')))
                v = v.mid(1, v.size() - 2);
            return v;
        };
        const QString f3 = nextPath(), f2 = nextPath();
        const int maxPages = (cur < as.size()) ? qMax(1, as.value(cur).toInt()) : 75;
        for (const QString& f : {f3, f2}) {
            if (!QFile::exists(f)) {
                fprintf(stdout, "RAMP: FAIL file khong ton tai: %s\n",
                        f.toLocal8Bit().constData()); fflush(stdout);
                return 2;
            }
        }
        MainWindow w; w.resize(1400, 900); w.show();
        QCoreApplication::processEvents();
        auto spin = [&](int ms) {
            const int loops = qMax(ms / 50, 1);
            for (int i = 0; i < loops; ++i) { QCoreApplication::processEvents(); QThread::msleep(50); }
        };
        auto commitMB = []() -> long long {
            long long pv = 0;
#ifdef Q_OS_WIN
            PROCESS_MEMORY_COUNTERS_EX pmc{};
            if (GetProcessMemoryInfo(GetCurrentProcess(),
                                     reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc)))
                pv = (long long)(pmc.PrivateUsage / 1048576);
#endif
            return pv;
        };
        auto rssTrimMB = []() -> long long {
            long long ws = 0;
#ifdef Q_OS_WIN
            EmptyWorkingSet(GetCurrentProcess());   // ép OS thu hồi trang đã free -> RSS = live
            PROCESS_MEMORY_COUNTERS_EX pmc{};
            if (GetProcessMemoryInfo(GetCurrentProcess(),
                                     reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc)))
                ws = (long long)(pmc.WorkingSetSize / 1048576);
#endif
            return ws;
        };
        // Working set TRUOC khi ép OS thu hồi (so voi rssLive = sau trim).
        auto rssMB = []() -> long long {
            long long ws = 0;
#ifdef Q_OS_WIN
            PROCESS_MEMORY_COUNTERS_EX pmc{};
            if (GetProcessMemoryInfo(GetCurrentProcess(),
                                     reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc)))
                ws = (long long)(pmc.WorkingSetSize / 1048576);
#endif
            return ws;
        };
        bool allOk = true; QString reason;
        auto noteFail = [&allOk, &reason](const QString& r) {
            if (!allOk) return; allOk = false; reason = r;
        };
        long long commitMax = 0, commitBaseline = -1;
        auto noteCommit = [&](const char* where) {
            const long long c = commitMB();
            if (c > commitMax) commitMax = c;
            if (c > 3000)
                noteFail(QStringLiteral("commit %1MB > 3000MB tran RAM (%2)").arg(c).arg(where));
        };
        // Cuon toi trang p, co anh hoac ≤15 s. Tra ve co anh khong.
        auto measurePage = [&](int p) -> bool {
            w.probeScrollToPage(p);
            QElapsedTimer et; et.start();
            int lastRescroll = 0;
            while (et.elapsed() < 15000) {
                spin(50);
                if (w.probePageHasImage(p)) break;
                if (et.elapsed() - lastRescroll >= 500) {
                    w.probeScrollToPage(p); lastRescroll = (int)et.elapsed();
                }
            }
            return w.probePageHasImage(p);
        };
        auto waitDocOpen = [&](int minPages) {
            QElapsedTimer dt; dt.start();
            while (dt.elapsed() < 30000 && w.probePageCount() < minPages) spin(100);
        };
        // In một dòng phân rã: commit + RSS(live) + RAMBREAK + pool từng tab.
        auto breakdown = [&](const char* where) {
            const long long cm = commitMB();
            if (cm > commitMax) commitMax = cm;
            if (cm > 3000) noteFail(QStringLiteral("commit %1MB > 3000MB (%2)").arg(cm).arg(where));
            // 🔴 LƯỢT 31 (B): WS = WorkingSetSize TRUOC trim (working set that),
            //   rssLive = SAU EmptyWorkingSet (phan da free OS lay lai duoc). commit
            //   (PrivateUsage) cao hon WS = da free nhung Segment Heap chua tra OS.
            const long long wsRaw = rssMB();
            fprintf(stdout, "RAMP %s commit=%lldMB ws=%lldMB rssLive=%lldMB\n", where, cm, wsRaw, rssTrimMB());
            fprintf(stdout, "%s\n", w.probeRamBreakdown().toLocal8Bit().constData());
            fprintf(stdout, "RAMP pool %s\n", w.probePoolInfoAll().toLocal8Bit().constData());
            fflush(stdout);
        };
        commitBaseline = commitMB();
        fprintf(stdout, "RAMP commit_baseline=%lldMB pool_size_env=%s maxPages=%d\n",
                commitBaseline, qgetenv("TORREADER_POOL_SIZE").constData(), maxPages);
        fflush(stdout);

        // ── f3: quái vật ──
        w.openFile(f3); waitDocOpen(1);
        const int n3 = qBound(1, w.probePageCount(), maxPages);
        for (int p = 0; p < n3; ++p) {
            measurePage(p);
            if (p % 5 == 0 || p == n3 - 1) {
                char buf[64]; snprintf(buf, sizeof(buf), "f3 p=%d/%d", p, n3);
                breakdown(buf);
            }
        }
        // zoom 100% vài trang (chủ nhân: ~9 s ở fit zoom — do thêm ở zoom thật)
        w.probeSetZoom(1.0);
        for (int p : {3, 10, 20}) { if (p < n3) { measurePage(p); char buf[64]; snprintf(buf, sizeof(buf), "f3 zoom100 p=%d", p); breakdown(buf); } }

        // ── f2: MEP 244 MB ──
        w.openFile(f2); waitDocOpen(1);
        const int n2 = qBound(1, w.probePageCount(), 40);
        for (int p = 0; p < n2; ++p) {
            measurePage(p);
            if (p % 5 == 0 || p == n2 - 1) {
                char buf[64]; snprintf(buf, sizeof(buf), "f2 p=%d/%d", p, n2);
                breakdown(buf);
            }
        }

        // ── CUỐI: đóng TỪNG pool doc RẢNH, đo delta (nghi phạm A). RSS(live) sau
        //    trim = bộ nhớ THỰC doc đó đang giữ (commit có thể không tụt vì Segment
        //    Heap giữ segment đã commit). ──
        spin(3000);   // để thumbnail/worker trả handle về rảnh
        const int ndocs = w.probeOpenDocCount();
        for (int d = 0; d < ndocs; ++d) {
            int closed = 0;
            for (;;) {
                const long long c0 = commitMB(), r0 = rssTrimMB();
                if (!w.probeCloseOneIdlePoolDocTab(d)) break;
                const long long c1 = commitMB(), r1 = rssTrimMB();
                ++closed;
                fprintf(stdout, "RAMP POOLDOC tab=%d #%d commit %lld->%lld (d=%lld) rssLive %lld->%lld (d=%lld)\n",
                        d, closed, c0, c1, c1 - c0, r0, r1, r1 - r0);
                fflush(stdout);
                if (closed > 40) break;
            }
            fprintf(stdout, "RAMP POOLDOC tab=%d closed=%d (con ranhg=%d)\n",
                    d, closed, w.probeIdlePoolDocsAll());
            fflush(stdout);
        }

        // ── đóng hết tab ──
        for (int n = w.probeTabCount(); n > 0; ) {
            w.probeCloseTab(0);
            const int n2b = w.probeTabCount();
            if (n2b >= n) break;
            n = n2b;
        }
        spin(3000);
        // 🔴 LƯỢT 30 (thử): khi idle (mọi tab đóng), huỷ + init lại PDFium để giải
        // phóng CACHE TOÀN CỤC (CPDF_PageModule ảnh/font) chỉ được trả ở DestroyLibrary.
        // Đo xem commit có về gần mức khởi động không (Segment Heap có thể vẫn giữ).
        if (qEnvironmentVariableIsSet("TORREADER_REINIT_IDLE")) {
            const long long c0 = commitMB();
            const bool done = PdfDocument::libReinitIdle();
            spin(500);
            fprintf(stdout, "RAMP reinit idle=%d commit %lld->%lldMB\n",
                    done ? 1 : 0, c0, commitMB());
            fflush(stdout);
        }
        breakdown("sau khi DONG HET tab");
        {
            const long long cm = commitMB();
            const int docMo = g_pdfiumDocOpen.loadRelaxed() - g_pdfiumDocClose.loadRelaxed();
            if (docMo == 0 && cm > commitBaseline * 3 / 2 + 300)
                noteFail(QStringLiteral("dong het tab: commit %1MB > 1,5x khoi dong (%2)+300").arg(cm).arg(commitBaseline));
        }
        fprintf(stdout, "OWNER3TAB commit_max=%lldMB commit_baseline=%lldMB\n",
                commitMax, commitBaseline);
        fflush(stdout);
        if (allOk) { fprintf(stdout, "RAMP: PASS\n"); fflush(stdout); return 0; }
        fprintf(stdout, "RAMP: FAIL %s\n", reason.toLocal8Bit().constData());
        fflush(stdout);
        return 2;
    }

    // ── LƯỢT 28 — RERENDER BENCH (VIỆC 1b — rút gọn, loại "tab khác") ──────────
    //   --rerender-bench <f3> [page=3] [N=4]
    // MỘT process, MỘT file: mo f3 -> ve trang bang DUNG duong cua app (openFile /
    // probeScrollToPage / probePageHasImage giong het measurePage cua owner3tab —
    // ProgressiveRenderTask + pool, cung scale do cua so 1400x900) -> DONG tab ->
    // lap N lan. Neu ms TANG DON DIEU khi khong co file nao khac mo => loan hoan
    // "tab khac", guoc tro ve trang thai process (heap). In ms/lan + hp_big_ns +
    // tom tat heap + commit + peak RSS moi vong. TORREADER_HEAPCOMPACT=1 (Thử A)
    // co hieu luc o day vi no nam trong closeJob.finished cua MainWindow — duong
    // dong tab that cua app. Tran cho 1 lan ve 120 s (probe cat o 30 s — can ms
    // THAT cua lan 2+, khong phai gia tri timeout).
    if (argc >= 3 && QCoreApplication::arguments().value(1) == QLatin1String("--rerender-bench")) {
        const QStringList as = QCoreApplication::arguments();
        int cur = 2;
        auto nextPath = [&as, &cur]() -> QString {
            QString v;
            for (int j = cur; j < as.size(); ++j) {
                v = v.isEmpty() ? as.value(j) : v + QLatin1Char(' ') + as.value(j);
                cur = j + 1;
                if (!(v.startsWith(QLatin1Char('"')) && !v.endsWith(QLatin1Char('"')))) break;
            }
            if (v.size() > 1 && v.startsWith(QLatin1Char('"')) && v.endsWith(QLatin1Char('"')))
                v = v.mid(1, v.size() - 2);
            return v;
        };
        const QString f3 = nextPath();
        const int page = (cur < as.size()) ? qMax(0, as.value(cur).toInt()) : 3;
        const int N    = (cur + 1 < as.size()) ? qMax(1, as.value(cur + 1).toInt()) : 4;
        if (!QFile::exists(f3)) {
            fprintf(stdout, "RERENDER: FAIL file khong ton tai: %s\n", f3.toLocal8Bit().constData());
            fflush(stdout);
            return 2;
        }
        MainWindow w; w.resize(1400, 900); w.show();
        QCoreApplication::processEvents();
        auto spin = [&](int ms) {
            const int loops = qMax(ms / 50, 1);
            for (int i = 0; i < loops; ++i) { QCoreApplication::processEvents(); QThread::msleep(50); }
        };
        auto mem = [](long long& rss, long long& commit, long long& peakRss, long long& peakCommit) {
            rss = commit = peakRss = peakCommit = -1;
#ifdef Q_OS_WIN
            PROCESS_MEMORY_COUNTERS_EX pmc{};
            if (GetProcessMemoryInfo(GetCurrentProcess(),
                                     reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {
                rss        = (long long)(pmc.WorkingSetSize      / 1048576);
                commit     = (long long)(pmc.PrivateUsage         / 1048576);
                peakRss    = (long long)(pmc.PeakWorkingSetSize   / 1048576);
                peakCommit = (long long)(pmc.PeakPagefileUsage    / 1048576);
            }
#endif
        };
#ifdef Q_OS_WIN
        // 0=NT heap, 1=LFH, 2=SegmentHeap — bang chung manifest co noi hay bi Win10 bo qua.
        DWORD ht = 0;
        HeapQueryInformation(GetProcessHeap(), HeapCompatibilityInformation, &ht, sizeof(ht), nullptr);
        fprintf(stdout, "RERENDER: heapType=%lu (0=NT,1=LFH,2=SegmentHeap)\n", (unsigned long)ht);
        fflush(stdout);
#endif
        for (int it = 1; it <= N; ++it) {
            w.openFile(f3);
            { QElapsedTimer dt; dt.start();
              while (dt.elapsed() < 30000 && w.probePageCount() < 21) spin(100); }
            w.probeActivateTab(w.probeTabCount() - 1); spin(300);
            w.probeScrollToPage(page);
            QElapsedTimer et; et.start();
            int lastRescroll = 0;
            while (et.elapsed() < 120000) {
                spin(50);
                if (w.probePageHasImage(page)) break;
                if (et.elapsed() - lastRescroll >= 500) {
                    w.probeScrollToPage(page); lastRescroll = (int)et.elapsed();
                }
            }
            const qint64 ms = et.elapsed();
            const bool img = w.probePageHasImage(page);
            long long rss, commit, pRss, pCommit;
            mem(rss, commit, pRss, pCommit);
            fprintf(stdout, "RERENDER it=%d page=%d ms=%lld ok=%d rss=%lldMB commit=%lldMB\n",
                    it, page, (long long)ms, img ? 1 : 0, rss, commit);
            fflush(stdout);
            w.probeCloseTab(w.probeTabCount() - 1);
            { QElapsedTimer ct; ct.start();
              while (ct.elapsed() < 60000 && w.probeCloseJobs() > 0) spin(100); }
            spin(1000);   // cho closeJob.finished (HeapCompact neu co co) chay
            const long long hbig = heapBigNs();
            long long nh = -1, fk = -1, fb = -1;
            heapSummary(nh, fk, fb);
            mem(rss, commit, pRss, pCommit);
            fprintf(stdout, "RERENDER it=%d sau_dong hp_big_ns=%lld nHeaps=%lld hsFreeKB=%lld hsFreeBlocks=%lld"
                            " rss=%lldMB commit=%lldMB peakRss=%lldMB peakCommit=%lldMB\n",
                    it, hbig, nh, fk, fb, rss, commit, pRss, pCommit);
            fflush(stdout);
        }
        fprintf(stdout, "RERENDER: DONE\n"); fflush(stdout);
        return 0;
    }

    // ── BAI DO TRUOT CUA SO TRANG (2026-08-31) ─────────────────────────────────
    // Cau hoi cua owner: neu chi nap 20 trang roi truot tung 5 trang, PDFium co NHA
    // bo nho cua 5 trang truoc khong?
    // Cach do: cham tung lo trang (render thumbnail nho), sau moi lo DONG het trang cu
    // roi in RSS. RSS tang deu => KHONG nha. RSS phang => co nha.
    //   --pagewindow-probe <file> [lo=5] [soLo=8]
    if (argc >= 3 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--pagewindow-probe")) {
        auto strip = [](QString v) {
            if (v.startsWith(QLatin1Char('"')) && v.endsWith(QLatin1Char('"'))) return v.mid(1, v.size()-2);
            return v;
        };
        QString f = strip(QString::fromLocal8Bit(argv[2]));
        int batch = (argc >= 4) ? QString::fromLocal8Bit(argv[3]).toInt() : 5;
        int nBatch= (argc >= 5) ? QString::fromLocal8Bit(argv[4]).toInt() : 8;
        int rw    = (argc >= 6) ? QString::fromLocal8Bit(argv[5]).toInt() : 300;   // do rong render
        // giuMo=1 => KHONG dong trang sau khi render (giong PageCache cua app giu trang mo).
        // Day la bien duy nhat khac giua bai do PDFium thuan (140MB) va duong cua app.
        const bool giuMo = (argc >= 7) && QString::fromLocal8Bit(argv[6]) == QLatin1String("giumo");
        QVector<FPDF_PAGE> giuLai;
        if (batch <= 0) batch = 5;
        if (nBatch <= 0) nBatch = 8;

        auto rssMB = []() -> long long {
#ifdef Q_OS_WIN
            PROCESS_MEMORY_COUNTERS pmc{};
            if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
                return (long long)(pmc.WorkingSetSize / 1048576);
#endif
            return 0;
        };
        PdfDocument::libAddRef();
        FPDF_DOCUMENT doc = FPDF_LoadDocument(f.toUtf8().constData(), nullptr);
        if (!doc) { fprintf(stdout, "PAGEWIN: FAIL mo file\n"); return 1; }
        const int total = FPDF_GetPageCount(doc);
        fprintf(stdout, "PAGEWIN: mo xong, %d trang, RSS=%lldMB\n", total, rssMB());
        fflush(stdout);

        int done = 0;
        for (int b = 0; b < nBatch && done < total; ++b) {
            const int from = done, to = qMin(done + batch, total);
            for (int i = from; i < to; ++i) {
                FPDF_PAGE pg = FPDF_LoadPage(doc, i);
                if (!pg) continue;
                // render nho de ep PDFium PHAN TICH noi dung trang (giong thumbnail)
                const int w = rw, h = qMax(1, int(rw * 0.707));
                FPDF_BITMAP bmp = FPDFBitmap_Create(w, h, 0);
                if (bmp) {
                    FPDFBitmap_FillRect(bmp, 0, 0, w, h, 0xFFFFFFFF);
                    FPDF_RenderPageBitmap(bmp, pg, 0, 0, w, h, 0,
                                          FPDF_RENDER_LIMITEDIMAGECACHE);
                    FPDFBitmap_Destroy(bmp);
                }
                if (giuMo) giuLai.push_back(pg);      // GIU MO — giong PageCache
                else       FPDF_ClosePage(pg);        // DONG NGAY sau khi dung
            }
            done = to;
            fprintf(stdout, "PAGEWIN: da cham %3d/%d trang | RSS=%lldMB\n",
                    done, total, rssMB());
            fflush(stdout);
        }
        if (giuMo) {
            fprintf(stdout, "PAGEWIN: dang giu %d trang MO | RSS=%lldMB\n",
                    int(giuLai.size()), rssMB());
            fflush(stdout);
            for (FPDF_PAGE pg : giuLai) FPDF_ClosePage(pg);
            fprintf(stdout, "PAGEWIN: sau khi dong het trang | RSS=%lldMB\n", rssMB());
            fflush(stdout);
        }
        FPDF_CloseDocument(doc);
        fprintf(stdout, "PAGEWIN: sau khi DONG tai lieu | RSS=%lldMB\n", rssMB());
        fprintf(stdout, "PAGEWIN: DONE\n"); fflush(stdout);
        PdfDocument::libRelease();
        return 0;
    }

    // ── BAI DO VONG DOI BO NHO (2026-08-31) ────────────────────────────────────
    // Cau hoi can tra loi: 2,4 GB moi tai lieu la CHI PHI GIU MO, hay la RO RI?
    // Mo file -> do RSS -> DONG han tab -> do RSS lai. Lap N vong.
    //   RSS tra ve gan muc ban dau  => chi phi giu mo (chua tai lieu it di / dong tab lau khong dung)
    //   RSS KHONG tra ve, cong don  => RO RI THAT (moi lan mo la mat vinh vien)
    //   --memcycle-probe <file> [soVong=3] [waitMs=15000]
    if (argc >= 3 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--memcycle-probe")) {
        auto strip = [](QString v) {
            if (v.startsWith(QLatin1Char('"')) && v.endsWith(QLatin1Char('"')))
                return v.mid(1, v.size() - 2);
            return v;
        };
        QString f = strip(QString::fromLocal8Bit(argv[2]));
        int cycles = (argc >= 4) ? QString::fromLocal8Bit(argv[3]).toInt() : 3;
        int waitMs = (argc >= 5) ? QString::fromLocal8Bit(argv[4]).toInt() : 15000;
        if (cycles <= 0) cycles = 3;
        if (waitMs <= 0) waitMs = 15000;

        MainWindow w; w.resize(1400, 900); w.show();
        QCoreApplication::processEvents();
        auto spin = [&](int ms) {
            const int loops = qMax(ms / 50, 1);
            for (int i = 0; i < loops; ++i) { QCoreApplication::processEvents(); QThread::msleep(50); }
        };
        auto rssMB = []() -> long long {
#ifdef Q_OS_WIN
            PROCESS_MEMORY_COUNTERS pmc{};
            if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
                return (long long)(pmc.WorkingSetSize / 1048576);
#endif
            return 0;
        };
        fprintf(stdout, "MEMCYCLE: bat dau RSS=%lldMB\n", rssMB()); fflush(stdout);
        for (int c = 1; c <= cycles; ++c) {
            w.openFile(f);
            spin(waitMs);
            long long afterOpen = rssMB();
            w.probeCloseTab(0);
            spin(4000);
            long long afterClose = rssMB();
            fprintf(stdout, "MEMCYCLE: vong %d | sau MO=%lldMB | sau DONG=%lldMB\n"
                            "   tai lieu: mo=%d dong=%d | pool: mo=%d dong=%d | trang: mo=%d dong=%d\n",
                    c, afterOpen, afterClose,
                    g_pdfiumDocOpen.loadRelaxed(),  g_pdfiumDocClose.loadRelaxed(),
                    g_pdfiumPoolOpen.loadRelaxed(), g_pdfiumPoolClose.loadRelaxed(),
                    g_pdfiumPageOpen.loadRelaxed(), g_pdfiumPageClose.loadRelaxed());
            fflush(stdout);
        }
        fprintf(stdout, "MEMCYCLE: DONE\n"); fflush(stdout);
        return 0;
    }

    // ── BAI DO HAI TAB (2026-08-31) ──────────────────────────────────────────────
    // Ly do dung: suot ca ngay moi ban va deu chi do duoc canh MOT file, nen loi
    // "mo tab thu 2 khong nap gi" TAI DIEN HAI LAN ma khong bat duoc truoc khi giao.
    // Bai nay mo file 1, cho, roi mo file 2 (thanh tab 2), cho, rooi in ra so lieu
    // CUA TUNG TAI LIEU de biet tab nao khong nap duoc.
    //   --twotab-probe <file1> <file2> [waitMs moi tab, mac dinh 20000]
    // 🔴🔴 CHOT 2026-08-31: bai do PHAI CHET khi file khong ton tai.
    // Da tra gia: `--twotab-probe` chay voi tab 2 tro vao file KHONG CO THAT, app mo ra tab rong,
    // probe van in "TWOTAB: DONE" ⇒ moi so do hai tab tu truoc toi nay VO GIA TRI, va suyt bi
    // dung lam bang chung "khong hoi quy". Bai do im lang khi thieu du lieu con te hon khong co.
    auto _probeRequireFile = [](const QString& path, const char* nhan) -> bool {
        if (QFile::exists(path)) return true;
        fprintf(stderr, "PROBE_FAIL: %s khong ton tai: \"%s\"\n",
                nhan, path.toLocal8Bit().constData());
        return false;
    };

    if (argc >= 4 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--twotab-probe")) {
        auto strip = [](QString v) {
            if (v.startsWith(QLatin1Char('"')) && v.endsWith(QLatin1Char('"')))
                return v.mid(1, v.size() - 2);
            return v;
        };
        QString f1 = strip(QString::fromLocal8Bit(argv[2]));
        QString f2 = strip(QString::fromLocal8Bit(argv[3]));
        int waitMs = (argc >= 5) ? QString::fromLocal8Bit(argv[4]).toInt() : 20000;
        if (waitMs <= 0) waitMs = 20000;
        if (!_probeRequireFile(f1, "tab 1") || !_probeRequireFile(f2, "tab 2")) return 2;

        MainWindow w;
        w.resize(1400, 900);
        w.show();
        QCoreApplication::processEvents();

        // ── BANG KE BO NHO: in ra tung kho dang giu, de biet 90% RAM nam O DAU ──
        auto memDump = [&](const char* moc) {
            qint64 rssMB = 0, privMB = 0, peakMB = 0;
#ifdef Q_OS_WIN
            PROCESS_MEMORY_COUNTERS_EX pmc{};
            if (GetProcessMemoryInfo(GetCurrentProcess(),
                                     reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {
                rssMB  = qint64(pmc.WorkingSetSize) / 1048576;
                privMB = qint64(pmc.PrivateUsage)   / 1048576;   // bo nho RIENG da cam ket
                peakMB = qint64(pmc.PeakWorkingSetSize) / 1048576;
            }
#endif
            const qint64 gCache = PdfRenderer::globalCacheBytes() / 1048576;
            const qint64 pCache = PageCache::totalBytes() / 1048576;
            fprintf(stdout,
                "MEM[%s] WorkingSet=%lldMB | RIENG(commit)=%lldMB | dinh WS=%lldMB"
                " | anhRaster=%lldMB | PageCache=%lldMB (%d trang)\n",
                moc, (long long)rssMB, (long long)privMB, (long long)peakMB,
                (long long)gCache, (long long)pCache, PageCache::entryCount());
            fprintf(stdout, "%s\n", w.probeMemBreakdown().toLocal8Bit().constData());
            fflush(stdout);
        };

        auto spin = [&](int ms) {
            const int loops = qMax(ms / 50, 1);
            for (int i = 0; i < loops; ++i) { QCoreApplication::processEvents(); QThread::msleep(50); }
        };

        fprintf(stdout, "TWOTAB: mo tab 1 = %s\n", f1.toLocal8Bit().constData());
        fflush(stdout);
        w.openFile(f1);
        spin(waitMs);
        fprintf(stdout, "TWOTAB: --- moc sau tab 1 ---\n"); fflush(stdout);
        memDump("sau tab 1");

        fprintf(stdout, "TWOTAB: mo tab 2 = %s\n", f2.toLocal8Bit().constData());
        fflush(stdout);
        w.openFile(f2);
        spin(waitMs);
        fprintf(stdout, "TWOTAB: --- moc sau tab 2 ---\n"); fflush(stdout);
        memDump("sau tab 2");

        // ── THU NGHIEM 31/08: bo nho kia CO PHAI chi la heap chua tra ve OS? ──
        // Tren Windows heap duoc anh xa MEM_COMMIT|MEM_RESERVE nen Windows tinh HET vao RSS,
        // ke ca phan da free ma thu vien C giu lai trong danh sach rong.
        // _heapmin() ep tra phan chua dung ve OS. EmptyWorkingSet() day trang ra khoi
        // working set. Neu RSS TUT MANH sau 2 lenh nay => khong phai bi chiem, chi la chua tra.
#ifdef Q_OS_WIN
        _heapmin();
        memDump("sau _heapmin");
        EmptyWorkingSet(GetCurrentProcess());
        spin(1000);
        memDump("sau EmptyWorkingSet");
#endif
        fprintf(stdout, "TWOTAB: DONE\n"); fflush(stdout);
        return 0;
    }

    // usage: --scroll-probe <input.pdf> [soLanCuon] [msMoiLan]
    // Ep dung tinh huong owner mo ta: LUOT qua cac trang nang. Moi lan cuon lam trang cu
    // khong con nhin thay => render dang chay bi huy giua chung. Truoc 2026-08-31 anh ve do
    // do van duoc phat ra nhu anh day du (chi may net, mat khung ten) va con bi ghi vao dia.
    // usage: --zoommarkup-probe <file_co_markup.pdf>
    // Tai hien "Continuous mat markup khi zoom": mo o View Fast, dem markup ve ra, ZOOM, dem lai.
    if (argc >= 3 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--zoommarkup-probe")) {
        QString inputPath = QString::fromLocal8Bit(argv[2]);
        if (inputPath.startsWith(QLatin1Char('"')) && inputPath.endsWith(QLatin1Char('"')))
            inputPath = inputPath.mid(1, inputPath.size() - 2);
        if (!_probeRequireFile(inputPath, "file dau vao")) return 2;

        MainWindow w;
        w.resize(1600, 1000);
        w.show();
        QCoreApplication::processEvents();
        // tham so 3: "single" => do o View Quality; mac dinh do o View Fast
        const bool doSingle = (argc >= 4 && QString::fromLocal8Bit(argv[3]) == QLatin1String("single"));
        w.probeSetFastMode(!doSingle);
        QCoreApplication::processEvents();
        w.openFile(inputPath);
        auto spin = [](int ms) {
            for (int i = 0; i < qMax(1, ms / 25); ++i) { QCoreApplication::processEvents(); QThread::msleep(25); }
        };
        spin(8000);
        fprintf(stdout, "ZM: === che do = %s ===\n", doSingle ? "SINGLE" : "FAST"); fflush(stdout);
        fprintf(stdout, "ZM: === TRUOC ZOOM ===\n"); fflush(stdout);
        spin(1500);
        if (doSingle) w.probeSetZoomSingle(2.0); else w.probeSetZoom(2.0);
        fprintf(stdout, "ZM: === DA ZOOM 2.0 ===\n"); fflush(stdout);
        spin(8000);
        if (doSingle) w.probeSetZoomSingle(4.0); else w.probeSetZoom(4.0);
        fprintf(stdout, "ZM: === DA ZOOM 4.0 ===\n"); fflush(stdout);
        spin(8000);
        fprintf(stdout, "ZM_OK\n");
        return 0;
    }

    // usage: --newannot-probe <file.pdf> [single]
    // Tai hien DUNG canh owner: TAO Note va Text roi xem chung co hien NGAY khong.
    // Do bang PIXEL tren chinh khung nhin — khong tin log, khong tin bo dem.
    if (argc >= 3 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--newannot-probe")) {
        QString inputPath = QString::fromLocal8Bit(argv[2]);
        if (inputPath.startsWith(QLatin1Char('"')) && inputPath.endsWith(QLatin1Char('"')))
            inputPath = inputPath.mid(1, inputPath.size() - 2);
        if (!_probeRequireFile(inputPath, "file dau vao")) return 2;
        const bool doSingle = (argc >= 4 && QString::fromLocal8Bit(argv[3]) == QLatin1String("single"));

        MainWindow w;
        w.resize(1600, 1000);
        w.show();
        QCoreApplication::processEvents();
        w.probeSetFastMode(!doSingle);
        QCoreApplication::processEvents();
        w.openFile(inputPath);
        auto spin = [](int ms) {
            for (int i = 0; i < qMax(1, ms / 25); ++i) { QCoreApplication::processEvents(); QThread::msleep(25); }
        };
        // Dem pixel VANG (icon Note cua PDFium) va pixel co mau trong khung nhin.
        auto demMau = [&w](int& vang, int& coMau) {
            vang = 0; coMau = 0;
            QImage im = w.probeGrabView().convertToFormat(QImage::Format_ARGB32);
            if (im.isNull()) { fprintf(stderr, "PROBE_FAIL: khong chup duoc khung nhin\n"); return; }
            for (int y = 0; y < im.height(); y += 2) {
                const QRgb* row = reinterpret_cast<const QRgb*>(im.constScanLine(y));
                for (int x = 0; x < im.width(); x += 2) {
                    const QRgb c = row[x];
                    const int r = qRed(c), g = qGreen(c), b = qBlue(c);
                    if (r > 200 && g > 200 && b > 200) continue;
                    ++coMau;
                    if (r > 200 && g > 140 && g < 215 && b < 110) ++vang;
                }
            }
        };
        spin(9000);
        // 🔴 Bai do phai NHIN THAY duoc thu minh dinh do: o muc vua-khung, trang qua nho nen
        // icon Note chi vai pixel va bi nhoe vao nen ⇒ so do khong nhay. Phong to va nhay ve
        // trang 0 truoc khi dem.
        const int naPage = qEnvironmentVariableIntValue("NA_PAGE");
        // NA_ZOOM=<phan tram>: do o dung muc zoom owner dung (vd 75). Mac dinh 100.
        const int naZoom = qEnvironmentVariableIntValue("NA_ZOOM");
        const double zTest = (naZoom > 0 ? naZoom : 100) / 100.0;
        if (doSingle) w.probeSetZoomSingle(zTest); else { w.probeSetZoom(zTest); w.probeScrollToPage(0); }
        spin(4000);
        int vangTruoc = 0, mauTruoc = 0; demMau(vangTruoc, mauTruoc);
        fprintf(stdout, "NA: che do=%s | TRUOC: vang=%d coMau=%d\n",
                doSingle ? "SINGLE" : "FAST", vangTruoc, mauTruoc);
        fflush(stdout);

        // NA_PAGE=<n>: tao chu thich tren trang n (0-based). Mac dinh 0.
        // Trang khac nhau co the cho ket qua khac han — trang 3 cua tep owner co 41 chu thich.
        // 🔴 NA_TEXT_TRUOC=1: tao TEXT truoc roi moi Note — dung thu tu owner lam khi gap loi
        // "new text la mat trang, them Note thi noi dung tro lai". Thu tu tao co the doi ket qua.
        if (!qEnvironmentVariableIsEmpty("NA_TEXT_TRUOC")) {
            const bool okT0 = w.probeCreateText(naPage, 260.0, 320.0, 180.0, 40.0);
            spin(6000);
            int v1 = 0, m1 = 0; demMau(v1, m1);
            fprintf(stdout, "NA: tao Text TRUOC ok=%d | SAU: coMau=%d (%+d)\n",
                    okT0 ? 1 : 0, m1, m1 - mauTruoc);
            fflush(stdout);
            const bool okN0 = w.probeCreateNote(naPage, 200.0, 300.0);
            spin(6000);
            int v2 = 0, m2 = 0; demMau(v2, m2);
            fprintf(stdout, "NA: tao Note SAU  ok=%d | SAU: coMau=%d (%+d)\n",
                    okN0 ? 1 : 0, m2, m2 - m1);
            fprintf(stdout, "NA_OK\n");
            return 0;
        }
        const bool okNote = w.probeCreateNote(naPage, 200.0, 300.0);
        spin(6000);
        int vangSauNote = 0, mauSauNote = 0; demMau(vangSauNote, mauSauNote);
        fprintf(stdout, "NA: tao Note ok=%d | SAU: vang=%d (+%d) coMau=%d (+%d)\n",
                okNote ? 1 : 0, vangSauNote, vangSauNote - vangTruoc,
                mauSauNote, mauSauNote - mauTruoc);
        fflush(stdout);

        const bool okText = w.probeCreateText(naPage, 260.0, 320.0, 180.0, 40.0);
        spin(6000);
        int vangSauText = 0, mauSauText = 0; demMau(vangSauText, mauSauText);
        fprintf(stdout, "NA: tao Text ok=%d | SAU: coMau=%d (+%d so voi sau Note)\n",
                okText ? 1 : 0, mauSauText, mauSauText - mauSauNote);
        fprintf(stdout, "NA_OK\n");
        return 0;
    }

    // usage: --zoomanchor-probe <file.pdf> <page0based> <zoomFrom> <zoomTo> [fx] [fy] [mode] [offY]
    // mode=0: diem (fx,fy) trong trang; mode=1: con tro trong KHE 12px duoi trang (LOI 1).
    // offY: lech con tro khoi tam khung nhin (px).
    // Do lech neo khi Ctrl+zoom o ContinuousView (loi "zoom lam nhay vi tri").
    if (argc >= 6 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--zoomanchor-probe")) {
        QString inputPath = QString::fromLocal8Bit(argv[2]);
        if (inputPath.startsWith(QLatin1Char('"')) && inputPath.endsWith(QLatin1Char('"')))
            inputPath = inputPath.mid(1, inputPath.size() - 2);
        if (!_probeRequireFile(inputPath, "file dau vao")) return 2;
        const int    page     = QString::fromLocal8Bit(argv[3]).toInt();
        const double zoomFrom = QString::fromLocal8Bit(argv[4]).toDouble();
        const double zoomTo   = QString::fromLocal8Bit(argv[5]).toDouble();
        const double fx = (argc >= 7) ? QString::fromLocal8Bit(argv[6]).toDouble() : 0.5;
        const double fy = (argc >= 8) ? QString::fromLocal8Bit(argv[7]).toDouble() : 0.4;
        const int    mode = (argc >= 9) ? QString::fromLocal8Bit(argv[8]).toInt() : 0;
        const double offY = (argc >= 10) ? QString::fromLocal8Bit(argv[9]).toDouble() : 0.0;

        // Do thang tren ContinuousView, KHONG render (renderer = nullptr) — chi can bo
        // cuc + page size. Vong lap su kien cua MainWindow tren may khong GPU bi treo
        // (do duoc: processEvents ket o lan lap 6), ma phep do nay khong can ve.
        PdfDocument::libAddRef();
        {
            PdfDocument doc;
            if (!doc.open(inputPath)) {
                fprintf(stderr, "ZOOMANCHOR_FAIL cannot open %s\n", inputPath.toUtf8().constData());
                PdfDocument::libRelease();
                return 1;
            }
            ContinuousView view;
            view.resize(1600, 1000);
            view.viewport()->resize(1600, 1000);
            view.setDocument(&doc, nullptr, nullptr, nullptr);
            const QString rep = view.probeZoomAnchor(page, zoomFrom, zoomTo, fx, fy, mode, offY);
            fputs(rep.toLocal8Bit().constData(), stdout);
            fflush(stdout);
        }
        PdfDocument::libRelease();
        fprintf(stdout, "ZOOMANCHOR_OK\n");
        fflush(stdout);
        return 0;
    }

    // usage: --scrollpersist-probe <file.pdf> <zoomPercent> [real]
    // LOI 2 (0921): updateScrollBars ep setValue(hRange/2) moi lan goi (ke ca tu
    // resizeEvent) => keo splitter/kich thuoc cua so lam mat vi tri cuon ngang cua
    // nguoi dung. Probe nay dat canvas < vpW, keo ngang lech tam roi goi lai
    // updateScrollBars; vi tri phai duoc giu, con lan dau dat range van canh giua.
    // LOI B (0921): them 2 ca DOI vpW (chua keo => canh giua lai; da keo => giu nguyen).
    // "real": chay them duong CUA SO THAT (show() + thanh cuon) cho LOI A.
    if (argc >= 4 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--scrollpersist-probe")) {
        QString inputPath = QString::fromLocal8Bit(argv[2]);
        if (inputPath.startsWith(QLatin1Char('"')) && inputPath.endsWith(QLatin1Char('"')))
            inputPath = inputPath.mid(1, inputPath.size() - 2);
        if (!_probeRequireFile(inputPath, "file dau vao")) return 2;
        const double zoom = QString::fromLocal8Bit(argv[3]).toDouble();
        const bool realWindow = (argc >= 5) &&
                                (QString::fromLocal8Bit(argv[4]) == QLatin1String("real"));
        PdfDocument::libAddRef();
        {
            PdfDocument doc;
            if (!doc.open(inputPath)) {
                fprintf(stderr, "SCROLLPERSIST_FAIL cannot open %s\n", inputPath.toUtf8().constData());
                PdfDocument::libRelease();
                return 1;
            }
            ContinuousView view;
            view.resize(1600, 1000);
            view.viewport()->resize(1600, 1000);
            view.setDocument(&doc, nullptr, nullptr, nullptr);
            const QString rep = view.probeScrollPersist(zoom, realWindow);
            fputs(rep.toLocal8Bit().constData(), stdout);
            fflush(stdout);
        }
        PdfDocument::libRelease();
        fprintf(stdout, "SCROLLPERSIST_OK\n");
        fflush(stdout);
        return 0;
    }

    // usage: --gpuview-zoomanchor-probe <zoomFrom> <zoomTo> [panX] [panY]
    // LOI 3: PdfGpuView::setZoom (nut +/- / o nhap %) phai neo diem tai lieu o TAM
    // khung nhin, giong duong Ctrl+wheel. Do TRUOC/SAU khi da pan.
    if (argc >= 4 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--gpuview-zoomanchor-probe")) {
        const double zFrom = QString::fromLocal8Bit(argv[2]).toDouble();
        const double zTo   = QString::fromLocal8Bit(argv[3]).toDouble();
        const double panX  = (argc >= 5) ? QString::fromLocal8Bit(argv[4]).toDouble() : 0.0;
        const double panY  = (argc >= 6) ? QString::fromLocal8Bit(argv[5]).toDouble() : 0.0;
        PdfGpuView view;
        view.resize(1200, 900);
        const QString rep = view.probeZoomAnchor(zFrom, zTo, QPointF(panX, panY));
        fputs(rep.toLocal8Bit().constData(), stdout);
        fflush(stdout);
        fprintf(stdout, "GPUVIEW_ZOOMANCHOR_OK\n");
        fflush(stdout);
        return 0;
    }

    if (argc >= 3 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--scroll-probe")) {
        QString inputPath = QString::fromLocal8Bit(argv[2]);
        if (inputPath.startsWith(QLatin1Char('"')) && inputPath.endsWith(QLatin1Char('"')))
            inputPath = inputPath.mid(1, inputPath.size() - 2);
        if (!_probeRequireFile(inputPath, "file dau vao")) return 2;
        const int nScroll = (argc >= 4) ? QString::fromLocal8Bit(argv[3]).toInt() : 12;
        const int stepMs  = (argc >= 5) ? QString::fromLocal8Bit(argv[4]).toInt() : 700;

        MainWindow w;
        w.resize(1600, 1000);
        w.show();
        QCoreApplication::processEvents();
        w.probeSetFastMode(true);
        QCoreApplication::processEvents();
        w.openFile(inputPath);

        auto spin = [](int ms) {
            for (int i = 0; i < qMax(1, ms / 25); ++i) {
                QCoreApplication::processEvents();
                QThread::msleep(25);
            }
        };
        spin(6000);   // cho bo cuc dung xong

        const int nPg = qMax(1, w.probePageCount());
        for (int k = 0; k < nScroll; ++k) {
            const int target = k % nPg;
            fprintf(stdout, "SCROLL_PROBE step=%d page=%d\n", k, target);
            fflush(stdout);
            w.probeScrollToPage(target);
            spin(stepMs);
        }
        spin(8000);   // dung lai — trang cuoi phai duoc ve TRON VEN
        fprintf(stdout, "SCROLL_PROBE_OK scrolls=%d pages=%d\n", nScroll, nPg);
        return 0;
    }

    if (argc >= 4 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--viewfast-probe")) {
        QString inputPath = QString::fromLocal8Bit(argv[2]);
        QString outPng = QString::fromLocal8Bit(argv[3]);
        // WSL→cmd.exe co the them dau ngoac kep vao argv — loai bo
        if (inputPath.startsWith(QLatin1Char('"')) && inputPath.endsWith(QLatin1Char('"')))
            inputPath = inputPath.mid(1, inputPath.size() - 2);
        if (outPng.startsWith(QLatin1Char('"')) && outPng.endsWith(QLatin1Char('"')))
            outPng = outPng.mid(1, outPng.size() - 2);
        int waitMs = (argc >= 5) ? QString::fromLocal8Bit(argv[4]).toInt() : 60000;

        MainWindow w;
        w.resize(1600, 1000);
        w.show();
        QCoreApplication::processEvents();

        // Vao Fast mode TRUOC khi openFile — dung nhu nguoi dung dang o View Fast
        // roi mo file (goc cua 58,7s lockhold). probeSetFastMode chi set mode khi
        // chua co doc (currentTab null) — set m_fastMode=true + show continuousView.
        w.probeSetFastMode(true);
        QCoreApplication::processEvents();

        w.openFile(inputPath);

        const int loops = qMax(waitMs / 50, 80);
        for (int i = 0; i < loops; ++i) {
            QCoreApplication::processEvents();
            QThread::msleep(50);
        }

        QPixmap pm = w.grab();
        QImage img = pm.toImage();
        if (img.isNull()) {
            fprintf(stderr, "VIEWFAST_PROBE: FAIL grab returned null\n");
            return 1;
        }
        // Neu save khong duoc, thu format ARGB32 (mot so GL-backing store co van de)
        if (!img.save(outPng, "PNG")) {
            fprintf(stderr, "VIEWFAST_PROBE: WARN img.save failed size=%dx%d fmt=%d — try ARGB32\n",
                    img.width(), img.height(), img.format());
            img = img.convertToFormat(QImage::Format_ARGB32);
            if (!img.save(outPng, "PNG")) {
                fprintf(stderr, "VIEWFAST_PROBE: WARN ARGB32 also failed — try QImageWriter\n");
                QImageWriter wr(outPng, "PNG");
                if (!wr.write(img)) {
                    fprintf(stderr, "VIEWFAST_PROBE: FAIL cannot save \"%s\" — %s\n",
                            outPng.toLocal8Bit().constData(),
                            wr.errorString().toLocal8Bit().constData());
                    return 1;
                }
            }
        }

        fprintf(stdout, "VIEWFAST_PROBE_OK %s\n", outPng.toLocal8Bit().constData());
        return 0;
    }

    // usage: --viewfast-syncprobe <input.pdf> <page5Based>
    // Probe (SPEC_VIEWFAST VIỆC 3): mo file o Quality, nhay toi trang page5Based,
    // chuyen sang Fast (Continuous phai giu trang do), roi quay ve Quality (Single
    // van giu trang do). In trang sau moi buoc de nghiem thu bang so.
    if (argc >= 4 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--viewfast-syncprobe")) {
        QString inputPath = QString::fromLocal8Bit(argv[2]);
        int target5Based = QString::fromLocal8Bit(argv[3]).toInt();
        int target = target5Based - 1;

        MainWindow w;
        w.resize(1600, 1000);
        w.show();
        QCoreApplication::processEvents();

        w.openFile(inputPath);
        const qint64 openDeadline = QDateTime::currentMSecsSinceEpoch() + 120000;
        while ((!w.currentTabForProbe() || !w.currentTabForProbe()->doc
                || !w.currentTabForProbe()->doc->isOpen())
               && QDateTime::currentMSecsSinceEpoch() < openDeadline) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            QThread::msleep(20);
        }
        if (!w.currentTabForProbe() || !w.currentTabForProbe()->doc
            || !w.currentTabForProbe()->doc->isOpen()) {
            fprintf(stderr, "VIEWFAST_SYNC: FAIL document did not open in 120s\n");
            return 1;
        }
        const int total = w.currentTabForProbe()->doc->pageCount();
        target = qBound(0, target, total - 1);

        // Buoc 1: o Quality (mac dinh), nhay toi trang target.
        w.probeSetPage(target);
        QCoreApplication::processEvents();
        const int pageAfterQualityJump = w.probeCurrentPage();
        fprintf(stdout, "[viewfast-sync] after Quality jump to %d -> currentPage=%d fastMode=%d\n",
                target, pageAfterQualityJump, w.probeIsFastMode() ? 1 : 0);

        // Buoc 2: chuyen sang Fast — Continuous phai giu trang target.
        w.probeSetFastMode(true);
        for (int i = 0; i < 40; ++i) { QCoreApplication::processEvents(); QThread::msleep(50); }
        const int pageAfterFast = w.probeCurrentPage();
        fprintf(stdout, "[viewfast-sync] after switch to Fast -> currentPage=%d fastMode=%d\n",
                pageAfterFast, w.probeIsFastMode() ? 1 : 0);

        // Buoc 3: quay ve Quality — Single phai giu trang target.
        w.probeSetFastMode(false);
        for (int i = 0; i < 40; ++i) { QCoreApplication::processEvents(); QThread::msleep(50); }
        const int pageAfterQualityBack = w.probeCurrentPage();
        fprintf(stdout, "[viewfast-sync] after switch back to Quality -> currentPage=%d fastMode=%d\n",
                pageAfterQualityBack, w.probeIsFastMode() ? 1 : 0);

        bool pass = (pageAfterFast == target) && (pageAfterQualityBack == target);
        fprintf(stdout, "VIEWFAST_SYNC: %s (target=%d)\n", pass ? "PASS" : "FAIL", target);
        return pass ? 0 : 1;
    }

    // usage: --viewfast-twodoc-probe <doc1.pdf> <doc2.pdf> <out.png> [waitMs]
    // Probe (SPEC_PAGECACHE_THRASH_2026-08-31): mo HAI tai lieu cung luc o View Fast
    // (doc1 -> tab 0, doc2 -> tab 1), luan phien chuyen tab de tai trang tu ca 2 doc
    // trong khi doc kia la inactive. Nghiem thu log khong con LOAD+evict cung trang
    // cung thoi diem, khong co trang nap >2 lan, [stall] < 1000.
    if (argc >= 5 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--viewfast-twodoc-probe")) {
        QString pathA = QString::fromLocal8Bit(argv[2]);
        QString pathB = QString::fromLocal8Bit(argv[3]);
        QString outPng = QString::fromLocal8Bit(argv[4]);
        // WSL→cmd.exe co the them dau ngoac kep vao argv — loai bo
        if (pathA.startsWith(QLatin1Char('"')) && pathA.endsWith(QLatin1Char('"')))
            pathA = pathA.mid(1, pathA.size() - 2);
        if (pathB.startsWith(QLatin1Char('"')) && pathB.endsWith(QLatin1Char('"')))
            pathB = pathB.mid(1, pathB.size() - 2);
        if (outPng.startsWith(QLatin1Char('"')) && outPng.endsWith(QLatin1Char('"')))
            outPng = outPng.mid(1, outPng.size() - 2);
        int waitMs = (argc >= 6) ? QString::fromLocal8Bit(argv[5]).toInt() : 60000;

        MainWindow w;
        w.resize(1600, 1000);
        w.show();
        QCoreApplication::processEvents();

        // Vao Fast mode TRUOC khi mo file (giong --viewfast-probe).
        w.probeSetFastMode(true);
        QCoreApplication::processEvents();

        // Mo doc A (tab 0) — giong viewfast-probe: pump den khi doc that su mo
        w.openFile(pathA);
        {
            const qint64 dl = QDateTime::currentMSecsSinceEpoch() + 90000;
            while ((!w.currentTabForProbe() || !w.currentTabForProbe()->doc
                    || !w.currentTabForProbe()->doc->isOpen())
                   && QDateTime::currentMSecsSinceEpoch() < dl) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
                QThread::msleep(20);
            }
            fprintf(stdout, "[twodoc] doc A open=%d fast=%d tab0\n",
                    (w.currentTabForProbe() && w.currentTabForProbe()->doc
                     && w.currentTabForProbe()->doc->isOpen()) ? 1 : 0,
                    w.probeIsFastMode() ? 1 : 0);
        }

        // Mo doc B (tab 1) — doc B tro thanh active, doc A thanh background
        w.openFile(pathB);
        {
            const qint64 dl = QDateTime::currentMSecsSinceEpoch() + 90000;
            while ((!w.currentTabForProbe() || !w.currentTabForProbe()->doc
                    || !w.currentTabForProbe()->doc->isOpen())
                   && QDateTime::currentMSecsSinceEpoch() < dl) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
                QThread::msleep(20);
            }
            fprintf(stdout, "[twodoc] doc B open=%d fast=%d tab1\n",
                    (w.currentTabForProbe() && w.currentTabForProbe()->doc
                     && w.currentTabForProbe()->doc->isOpen()) ? 1 : 0,
                    w.probeIsFastMode() ? 1 : 0);
        }

        // Luan phien chuyen tab 3 lan de tai trang tu ca 2 doc (doc kia inactive)
        for (int rep = 0; rep < 3; ++rep) {
            w.probeActivateTab(0);
            for (int i = 0; i < 40; ++i) { QCoreApplication::processEvents(); QThread::msleep(50); }
            w.probeActivateTab(1);
            for (int i = 0; i < 40; ++i) { QCoreApplication::processEvents(); QThread::msleep(50); }
            fprintf(stdout, "[twodoc] switch rep=%d done\n", rep);
        }

        // Doc o tab 1 de chup anh
        const int loops = qMax(waitMs / 50, 80);
        for (int i = 0; i < loops; ++i) {
            QCoreApplication::processEvents();
            QThread::msleep(50);
        }

        QPixmap pm = w.grab();
        QImage img = pm.toImage();
        if (img.isNull()) {
            fprintf(stderr, "TWODOC: FAIL grab returned null\n");
            // Khong return 1 — van tiep tuc dem pixel neu co the
        } else if (!img.save(outPng, "PNG")) {
            fprintf(stderr, "TWODOC: WARN img.save failed, size=%dx%d format=%d\n",
                    img.width(), img.height(), img.format());
            // Thu luu bang QPixmap
            if (!pm.save(outPng, "PNG"))
                fprintf(stderr, "TWODOC: WARN pm.save also failed\n");
        } else {
            // Dem mau khac nhau
            QSet<QRgb> colors;
            for (int y = 0; y < img.height() && colors.size() <= 5000; ++y)
                for (int x = 0; x < img.width() && colors.size() <= 5000; ++x)
                    colors.insert(img.pixel(x, y));
            fprintf(stdout, "TWODOC distinctColors=%d img=%dx%d\n", colors.size(), img.width(), img.height());
        }

        fprintf(stdout, "VIEWFAST_TWODOC_OK %s\n", outPng.toLocal8Bit().constData());
        return 0;
    }

    // usage: --contvec-probe <input.pdf> <out.png> <zoomPercent> <page1Based> <awayPages> [waitMs]
    // Nghiem thu Viec A/B/C voi trang nang (owner 2026-08-30): mo file o Continuous,
    // cuon xuong awayPages trang roi quay lai trang dau. Chot log that khi quay lai:
    //   - KHONG con [torvec] SKIP ... reason=nokey
    //   - Co [torvec] HIT page= 0 (nap cache ~100 ms) thay vi [contvec] SKIP-HEAVY
    //   - Khong con [cont] paint LOWRES page= 0
    if (argc >= 7 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--contvec-probe")) {
        QString inputPath = QString::fromLocal8Bit(argv[2]);
        QString outPng = QString::fromLocal8Bit(argv[3]);
        bool zoomOk = false;
        double zoomPercent = QString::fromLocal8Bit(argv[4]).toDouble(&zoomOk);
        int page1Based = QString::fromLocal8Bit(argv[5]).toInt();
        int awayPages = QString::fromLocal8Bit(argv[6]).toInt();
        int waitMs = (argc >= 8) ? QString::fromLocal8Bit(argv[7]).toInt() : 8000;
        if (!zoomOk || awayPages < 0) {
            fprintf(stderr, "CONTVEC_PROBE: FAIL tham so khong hop le\n");
            return 1;
        }

        MainWindow w;
        w.resize(1600, 1000);
        w.show();
        QCoreApplication::processEvents();
        w.openFile(inputPath);

        auto* tab = w.currentTabForProbe();
        const qint64 openDeadline = QDateTime::currentMSecsSinceEpoch() + 120000;
        while ((!tab || !tab->doc || !tab->doc->isOpen())
               && QDateTime::currentMSecsSinceEpoch() < openDeadline) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            QThread::msleep(20);
            tab = w.currentTabForProbe();
        }
        if (!tab || !tab->doc || !tab->doc->isOpen()) {
            fprintf(stderr, "CONTVEC_PROBE: FAIL document did not open in 120s\n");
            return 1;
        }
        qDebug().noquote() << "[contvec-probe] doc open pages=" << tab->doc->pageCount();

        // Continuous che do xem, trang dau, zoom cho truoc.
        w.probeSetView(true, zoomPercent, page1Based,
                       std::numeric_limits<double>::quiet_NaN(),
                       std::numeric_limits<double>::quiet_NaN());
        const int page0 = qBound(0, page1Based - 1, tab->doc->pageCount() - 1);

        // Cho lop vector trang dau nap xong tu cache (~100ms) hoac build (~7s).
        const int warmLoops = qMax(waitMs / 50, 80);
        for (int i = 0; i < warmLoops; ++i) {
            QCoreApplication::processEvents();
            QThread::msleep(50);
        }
        qDebug().noquote() << "[contvec-probe] warmed page=" << page0;

        // Cuon xuong awayPages trang (nap bien: trang nang phai roi ngoai vung giu).
        w.probeContScrollTo(page0 + awayPages);
        qDebug().noquote() << "[contvec-probe] scrolled away to page=" << (page0 + awayPages);

        // Quay lai trang dau — phai HIT cache .torvec.
        w.probeContScrollTo(page0);
        qDebug().noquote() << "[contvec-probe] returned page=" << page0;

        for (int i = 0; i < qMax(waitMs / 50, 80); ++i) {
            QCoreApplication::processEvents();
            QThread::msleep(50);
        }

        QPixmap pm = w.grab();
        QImage img = pm.toImage();
        if (img.isNull()) {
            fprintf(stderr, "CONTVEC_PROBE: FAIL grab returned null\n");
            return 1;
        }
        if (!img.save(outPng, "PNG")) {
            fprintf(stderr, "CONTVEC_PROBE: FAIL cannot save %s\n", outPng.toLocal8Bit().constData());
            return 1;
        }
        fprintf(stdout, "CONTVEC_PROBE_OK %s\n", outPng.toLocal8Bit().constData());
        return 0;
    }

    // usage: --vecrender-probe <input.pdf> <page1Based> <out.png> [waitMs]
    // LUOT 38: render LOP VECTOR (net + anh) ra QImage o DUNG huong hien thi (/Rotate),
    // KHONG dung GL => chung minh duoc ca Single lan Continuous (cung lop du lieu nay),
    // va doi chieu pixel duoc voi MuPDF. Khong ve fill (chi net + anh).
    if (argc >= 5 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--vecrender-probe")) {
        QString inputPath = QString::fromLocal8Bit(argv[2]);
        if (inputPath.startsWith(QLatin1Char('"')) && inputPath.endsWith(QLatin1Char('"')))
            inputPath = inputPath.mid(1, inputPath.size() - 2);
        const int page1 = QString::fromLocal8Bit(argv[3]).toInt();
        const QString outPng = QString::fromLocal8Bit(argv[4]);

        MainWindow w;
        w.resize(1200, 800);
        w.show();
        QCoreApplication::processEvents();
        w.openFile(inputPath);
        auto* tab = w.currentTabForProbe();
        QElapsedTimer to; to.start();
        while ((!tab || !tab->doc || !tab->doc->isOpen()) && to.elapsed() < 120000) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            QThread::msleep(20);
            tab = w.currentTabForProbe();
        }
        if (!tab || !tab->doc || !tab->doc->isOpen()) {
            fprintf(stderr, "VECRENDER: FAIL document not open\n");
            return 1;
        }
        const int page = qBound(0, page1 - 1, tab->doc->pageCount() - 1);
        w.probeSetView(false, 100, page + 1,
                       std::numeric_limits<double>::quiet_NaN(),
                       std::numeric_limits<double>::quiet_NaN());
        for (int i = 0; i < 400; ++i) {
            QCoreApplication::processEvents();
            QThread::msleep(50);
            auto L = tab->vecLayers.value(page);
            if (L && L->isReady()) break;
        }
        auto L = tab->vecLayers.value(page);
        if (!L) { fprintf(stderr, "VECRENDER: FAIL no vector layer page=%d\n", page); return 1; }

        const int rot = L->rotation() & 3;
        const double vw = L->pageSizePt().width();
        const double vh = L->pageSizePt().height();
        const double dispW = (rot & 1) ? vh : vw;
        const double dispH = (rot & 1) ? vw : vh;
        QImage out(qMax(1, (int)std::ceil(dispW)), qMax(1, (int)std::ceil(dispH)),
                   QImage::Format_ARGB32);
        out.fill(Qt::white);
        QPainter p(&out);
        p.setRenderHint(QPainter::Antialiasing, true);
        QTransform t;
        switch (rot) {
            case 1: t.translate(dispW, 0.0);    t.rotate(90.0);  break;
            case 2: t.translate(dispW, dispH);  t.rotate(180.0); break;
            case 3: t.translate(0.0, dispH);    t.rotate(270.0); break;
            default: break;
        }
        p.setTransform(t);
        // Net vector (4 float/doan: x0 y0 x1 y1; 4 byte mau; 1 float be rong).
        const QVector<float>&   V  = L->verts();
        const QVector<uint8_t>& C  = L->colors();
        const QVector<float>&   Wd = L->widths();
        for (int i = 0, k = 0, ki = 0; i + 3 < V.size(); i += 4, k += 4, ++ki) {
            QColor c(C[k], C[k+1], C[k+2], C[k+3]);
            double wpx = Wd.value(ki, 1.0);
            if (wpx <= 0.0) wpx = 0.5;
            p.setPen(QPen(c, wpx));
            p.drawLine(QPointF(V[i], V[i+1]), QPointF(V[i+2], V[i+3]));
        }
        // Anh: tile.img da duoc xoay theo ma tran doi tuong (ban sua luot 38).
        for (const TextTile& tt : L->imageTiles())
            p.drawImage(tt.rectPt, tt.img);
        p.end();
        if (!out.save(outPng, "PNG")) {
            fprintf(stderr, "VECRENDER: FAIL save %s\n", outPng.toLocal8Bit().constData());
            return 1;
        }
        fprintf(stdout, "VECRENDER_OK %s rot=%d vw=%.1f vh=%.1f segs=%d tiles=%d\n",
                outPng.toLocal8Bit().constData(), rot, vw, vh, V.size() / 4,
                L->imageTiles().size());
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

    // usage: --torvec-test <input.pdf> <page1Based> [outfile=.torvec tam]
    // Headless self-test (SPEC_PERF_HEAVYPAGE buoc A): build VectorLayer cho 1
    // trang nang, ghi ra file, doc lai va so khop TUNG MANG giua doi tuong goc
    // `a` va doi tuong nap lai `b` bang DU LIEU that (khong chi so so phan tu).
    if (argc >= 4 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--torvec-test")) {
        QTextStream out(stdout);
        const QString inputPath = QString::fromLocal8Bit(argv[2]);
        bool pageOk = false;
        const int page1 = QString::fromLocal8Bit(argv[3]).toInt(&pageOk);
        if (!pageOk || page1 < 1) {
            out << "TORVEC_FAIL invalid page\n"; out.flush(); return 1;
        }
        const int page0 = page1 - 1;
        const QString outPath = (argc >= 5) ? QString::fromLocal8Bit(argv[4])
                                            : inputPath + QStringLiteral(".torvec");

        PdfDocument::libAddRef();

        PdfDocument doc;
        if (!doc.open(inputPath) || page0 >= doc.pageCount()) {
            out << "TORVEC_FAIL cannot open " << inputPath << "\n";
            out.flush();
            PdfDocument::libRelease();
            return 1;
        }

        VectorLayer a;
        QElapsedTimer t; t.start();
        const bool built = a.build(doc.raw(), page0);
        const qint64 buildMs = t.elapsed();
        if (!built) {                       // trang nhe -> build tra false
            out << "TORVEC_SKIP trang nhe\n";
            out.flush();
            PdfDocument::libRelease();
            return 0;
        }
        out << "TORVEC build_ms=" << buildMs << "\n";
        out.flush();

        {
            QFile f(outPath);
            if (!f.open(QIODevice::WriteOnly)) {
                out << "TORVEC_FAIL cannot write " << outPath << "\n";
                out.flush(); PdfDocument::libRelease(); return 1;
            }
            t.start();
            const bool ok = a.saveTo(f);
            const qint64 saveMs = t.elapsed();
            const qint64 size = f.size();
            f.close();
            out << "TORVEC save_ms=" << saveMs << " bytes=" << size << "\n";
            out << "TORVEC approx_bytes=" << a.approxBytes() << "\n";
            out.flush();
            if (!ok) { out << "TORVEC_FAIL saveTo\n"; out.flush(); PdfDocument::libRelease(); return 1; }
        }

        VectorLayer b;
        {
            QFile f(outPath);
            if (!f.open(QIODevice::ReadOnly)) {
                out << "TORVEC_FAIL cannot read " << outPath << "\n";
                out.flush(); PdfDocument::libRelease(); return 1;
            }
            t.start();
            const bool ok = b.loadFrom(f);
            const qint64 loadMs = t.elapsed();
            f.close();
            out << "TORVEC load_ms=" << loadMs << "\n";
            out.flush();
            if (!ok) { out << "TORVEC_FAIL loadFrom\n"; out.flush(); PdfDocument::libRelease(); return 1; }
        }

        QString firstFail;
        auto fail = [&](const char* name) { if (firstFail.isEmpty()) firstFail = QLatin1String(name); };

        auto emitCmp = [&](const char* name, bool same, int na, int nb, int idx) {
            if (same) {
                out << "TORVEC cmp " << name << "=OK\n";
            } else {
                out << "TORVEC cmp " << name << "=MISMATCH(" << na << " vs " << nb
                    << " | " << (idx < 0 ? -1 : idx) << ")\n";
                fail(name);
            }
        };
        auto cmpFloats = [&](const char* name, const QVector<float>& fa, const QVector<float>& fb) {
            bool same = (fa.size() == fb.size()); int idx = -1;
            if (same)
                for (int i = 0; i < fa.size(); ++i)
                    if (fa[i] != fb[i]) { same = false; idx = i; break; }
            emitCmp(name, same, fa.size(), fb.size(), idx);
        };
        auto cmpU8s = [&](const char* name, const QVector<uint8_t>& fa, const QVector<uint8_t>& fb) {
            bool same = (fa.size() == fb.size()); int idx = -1;
            if (same)
                for (int i = 0; i < fa.size(); ++i)
                    if (fa[i] != fb[i]) { same = false; idx = i; break; }
            emitCmp(name, same, fa.size(), fb.size(), idx);
        };
        auto cmpInts = [&](const char* name, const QVector<int>& fa, const QVector<int>& fb) {
            bool same = (fa.size() == fb.size()); int idx = -1;
            if (same)
                for (int i = 0; i < fa.size(); ++i)
                    if (fa[i] != fb[i]) { same = false; idx = i; break; }
            emitCmp(name, same, fa.size(), fb.size(), idx);
        };
        auto cmpRects = [&](const char* name, const QVector<QRectF>& fa, const QVector<QRectF>& fb) {
            bool same = (fa.size() == fb.size()); int idx = -1;
            if (same)
                for (int i = 0; i < fa.size(); ++i)
                    if (fa[i] != fb[i]) { same = false; idx = i; break; }
            emitCmp(name, same, fa.size(), fb.size(), idx);
        };
        auto cmpTiles = [&](const char* name, const QVector<TextTile>& ta, const QVector<TextTile>& tb) {
            bool same = (ta.size() == tb.size()); int idx = -1;
            if (same)
                for (int i = 0; i < ta.size(); ++i) {
                    const TextTile& x = ta[i]; const TextTile& y = tb[i];
                    if (x.rectPt != y.rectPt || x.depth != y.depth || x.clipIdx != y.clipIdx
                        || x.color != y.color || x.isAlpha != y.isAlpha || x.isNote != y.isNote
                        || !(x.img == y.img)) { same = false; idx = i; break; }
                }
            emitCmp(name, same, ta.size(), tb.size(), idx);
        };
        auto cmpScalar = [&](const char* name, bool eq) {
            emitCmp(name, eq, 1, 1, -1);
        };

        cmpFloats("verts", a.verts(), b.verts());
        cmpU8s("colors", a.colors(), b.colors());
        cmpFloats("widths", a.widths(), b.widths());
        cmpFloats("fillVerts", a.fillVerts(), b.fillVerts());
        cmpU8s("fillColors", a.fillColors(), b.fillColors());
        cmpFloats("depths", a.depths(), b.depths());
        cmpFloats("fillDepths", a.fillDepths(), b.fillDepths());
        cmpFloats("clipIdx", a.clipIdx(), b.clipIdx());
        cmpFloats("fillClipIdx", a.fillClipIdx(), b.fillClipIdx());
        cmpRects("clips", a.clips(), b.clips());
        cmpTiles("texts", a.textTiles(), b.textTiles());
        cmpTiles("images", a.imageTiles(), b.imageTiles());
        cmpScalar("page", a.pageIndex() == b.pageIndex());
        cmpScalar("rotation", a.rotation() == b.rotation());
        cmpScalar("pageSizePt", a.pageSizePt() == b.pageSizePt());
        cmpScalar("fillOpaqueFloats", a.fillOpaqueFloats() == b.fillOpaqueFloats());
        cmpScalar("tilesGeneration", a.tilesGeneration() == b.tilesGeneration());
        cmpScalar("isReady", a.isReady() == b.isReady());
        cmpScalar("isComplete", a.isComplete() == b.isComplete());
        out.flush();

        PdfDocument::libRelease();
        if (!firstFail.isEmpty()) {
            out << "TORVEC_FAIL " << firstFail << "\n";
            out.flush();
            return 1;
        }
        out << "TORVEC_OK\n";
        out.flush();
        return 0;
    }

    // usage: --torcache-probe <input.pdf>
    // 0927 M1..M6 — harness NGHIEM THU TREN MAY THAT. MainWindow THAT (khong mo
    // rong, khong gia lap): buoc (a) mo pdf = tab1, (b) mo CUNG pdf = tab2 (phai
    // ra log `DUNG CHUNG`), (c) dong tab1 (phai ra log `ok=0 loi=conTab=1` va file
    // .torcache CON), (d) nap lai tab2 qua DUNG `loadTabFile` (log `ly do=taiLai`),
    // (e) thoat app bang `close()` (log `ly do=thoatApp`). Moi buoc cho ~2 s bang
    // QTimer — KHONG sleep chan UI, vi sleep se lam `openFile`/thumbnail worker
    // khong bao gio chay giua chung. In `TCPROBE step=<a..e> exists=<0|1>` = file
    // .torcache co tren dia hay khong + `torvec=<n>` = so file .torvec con lai.
    // Chay voi file CO TRANG NANG (vi du PDF CAD) thi moi co .torvec that.
    // 0927 LUOT 6: het buoc clearCache (nut da bi go han) + them PHAI THOI
    // `TCPROBE leftover=<n>` dem CHINH trong app sau khi `close()` chay xong
    // (closeEvent = luc rut cache) ⇒ KY VONG 0.
    if (argc >= 3 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--torcache-probe")) {
        const QString inPath = QString::fromLocal8Bit(argv[2]);
        QTextStream pout(stdout);
        MainWindow w;
        w.resize(1280, 800);
        w.show();
        const QString cachePath = TileCacheFile::cachePathFor(inPath,
                                                               TileCacheFile::hashFile(inPath));
        const QString key = VectorCache::keyFromCachePath(cachePath);
        auto step = [&](const char* tag) {
            // In SAU khi buoc do chay: `exists` la ket qua cua chinh buoc do. (c)
            // `exists=1` ⇒ file .torcache CON sau khi dong tab1; (e) `exists=0` ⇒
            // thoat app da rut sach cache cua tai lieu do.
            pout << "TCPROBE step=" << tag << " exists=" << (QFile::exists(cachePath) ? 1 : 0)
                 << " torvec=" << VectorCache::countForKey(key) << "\n";
            pout.flush();
        };
        int s = 0;
        auto* timer = new QTimer(&w);
        QObject::connect(timer, &QTimer::timeout, &w, [&]() {
            switch (s++) {
            case 0:  w.openFile(inPath);        step("a_open1");     break;
            case 1:  w.openFile(inPath);        step("b_open2");     break; // CUNG pdf => DUNG CHUNG
            case 2:  w.probeCloseOpenDoc(0);    step("c_dong1");     break; // tab1 = openDoc 0
            case 3:  w.probeLoadTabFile(0);     step("d_taiLai");    break;
            default: break;
            }
        });
        timer->start(2000);
        // Buoc (e) sau khi 4 buoc tren da chay: 4 x 2 s + du phong.
        QTimer::singleShot(4 * 2000 + 3000, &w, [&]() {
            timer->stop();
            w.close();                       // thoat BINH THUONG => closeEvent that
            step("e_thoatApp");
            // 0927 LUOT 6: dem moi thu con sot cua CHINH tai lieu nay — .torcache,
            // .lock, .torvec, .torvec.tmp. Ky vong 0.
            pout << "TCPROBE leftover=" << VectorCache::countForKey(key) << "\n";
            pout.flush();
            pout << "TCPROBE_DONE\n";
            pout.flush();
            QCoreApplication::quit();
        });
        const int rc = app.exec();
        // 0927 LƯỢT 11: TCPROBE_DONE đã in xong ⇒ thoát nhanh, y hệt app thật
        // (nếu không, probe này đi qua ~MainWindow và không còn phản ánh đường
        // thoát đã vá).
        trThoatNhanh(rc, &w);
        return rc;
    }

    // ══════════════════════════════════════════════════════════════════════
    // 0927 BƯỚC 1 — HARNESS TEXT NHIỀU DÒNG (SPEC_TEXT_NHIEU_DONG_0927 §3).
    // Khối RIÊNG, không sửa probe cũ. Cùng kiểu các --*-probe sẵn có:
    // nhận tham số dòng lệnh, in kết quả ra stdout, trả 0/2.
    // Mục đích: người chấm kiểm KHÔNG cần chuột vẫn nghiệm thu được.

    // ── Dem so dong /AP that cua mot FreeText CUA TA (dung chung 2 probe) ──
    // Doc lai chinh file vua luu, lay /AP cua annot, dem so cap <hex> Tj.
    // Dem theo CACH DUY NHAT ma nguoi cham dung: so dong theo toa do y cua
    // renderer doc lap. O day chi dem lenh Tj ⇒ phai khop voi renderer.
    // 🔴 0927 LƯỢT 3 — DẸP BẾ TREO 60 s. onSaveFile() báo lỗi bằng QMessageBox
    // MODAL; probe bơm processEvents nên hộp thoại bật lên và treo vô hạn
    // (đúng triệu chứng CEO thấy: log có [inote] SetAP ok nhưng không có dòng
    // FTMULTI). Ở đây ta đóng MỌI hộp thoại modal sau mỗi vòng bơm event, để
    // lỗi hiện ra rồi đi tiếp thay vì treo. Chỉ dùng trong harness.
    auto _dismissModals = []() {
        for (int guard = 0; guard < 8; ++guard) {
            QWidget* w = QApplication::activeModalWidget();
            if (!w) break;
            if (auto* mb = qobject_cast<QMessageBox*>(w)) {
                QTextStream e(stderr);
                e << "HARNESS_MODAL " << mb->text().toUtf8().constData() << "\n";
                e.flush();
            }
            w->close();
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        }
    };
    auto _ftApLineCount = [](const QString& path, int pageIndex, int annotIndex,
                             QString* apOut) -> int {
        PdfDocument::libAddRef();
        int lines = 0;
        {
            PdfDocument doc;
            if (!doc.open(path)) { PdfDocument::libRelease(); return -1; }
            {
                QMutexLocker lock(&s_pdfiumMutex);
                FPDF_PAGE p = FPDF_LoadPage(doc.raw(), pageIndex);
                if (p) {
                    // annotIndex < 0 ⇒ tự dò annot FreeText đầu tiên trên trang
                    // (probe 0927: sau khi lưu, chỉ số có thể đổi vì /Annots
                    // được QPDF viết lại — không được hard-code 0).
                    int idx = annotIndex;
                    if (idx < 0) {
                        const int n = FPDFPage_GetAnnotCount(p);
                        for (int i = 0; i < n; ++i) {
                            FPDF_ANNOTATION c = FPDFPage_GetAnnot(p, i);
                            if (!c) continue;
                            const bool isFreeText =
                                (FPDFAnnot_GetSubtype(c) == FPDF_ANNOT_FREETEXT);
                            FPDFPage_CloseAnnot(c);
                            if (isFreeText) { idx = i; break; }
                        }
                        if (idx < 0) { FPDF_ClosePage(p); PdfDocument::libRelease(); return -2; }
                    }
                    FPDF_ANNOTATION a = FPDFPage_GetAnnot(p, idx);
                    if (a) {
                        unsigned long len = FPDFAnnot_GetAP(a, FPDF_ANNOT_APPEARANCEMODE_NORMAL,
                                                           nullptr, 0);
                        QString ap;
                        if (len > 1) {
                            std::vector<unsigned short> buf(len / 2 + 1, 0);
                            FPDFAnnot_GetAP(a, FPDF_ANNOT_APPEARANCEMODE_NORMAL,
                                            reinterpret_cast<FPDF_WCHAR*>(buf.data()), len);
                            ap = QString::fromUtf16(
                                reinterpret_cast<const char16_t*>(buf.data()));
                        }
                        if (apOut) *apOut = ap;
                        // Số dòng /AP = số lệnh <hex> Tj trong khối BT/ET
                        // (mỗi dòng 1 Tj theo đường dựng mới). Đây là con số
                        // phải KHỚP với renderer độc lập đếm theo toạ độ y.
                        {
                            int inBT = 0;
                            for (const QString& rawLn : ap.split(QLatin1Char('\n'))) {
                                const QString t = rawLn.trimmed();
                                if (t.startsWith(QLatin1String("BT"))) inBT = 1;
                                else if (t.startsWith(QLatin1String("ET"))) inBT = 0;
                                else if (inBT && t.endsWith(QLatin1String("Tj"))) ++lines;
                            }
                        }
                        FPDFPage_CloseAnnot(a);
                    }
                    FPDF_ClosePage(p);
                }
            }
        }
        PdfDocument::libRelease();
        return lines;
    };
    // usage: --ftmulti-probe <in.pdf> <out.pdf>
    // Tao tren trang 1 mot FreeText CUA TA, o rong 200pt, chu 3 dong cung +
    // 1 doan phai tu ngat mem, luu ra out.pdf, in
    //   FTMULTI lines=<so dong /AP> rect=<w>x<h>
    if (argc >= 4 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--ftmulti-probe")) {
        auto stripQ = [](QString v) {
            if (v.startsWith(QLatin1Char('"')) && v.endsWith(QLatin1Char('"')))
                return v.mid(1, v.size() - 2);
            return v;
        };
        const QString inPath  = stripQ(QString::fromLocal8Bit(argv[2]));
        const QString outPath = stripQ(QString::fromLocal8Bit(argv[3]));
        QTextStream out(stdout);
        if (!QFile::exists(inPath)) {
            out << "FTMULTI FAIL: file dau vao khong ton tai: " << inPath << "\n";
            out.flush(); return 2;
        }
        QFile::remove(outPath);
        if (!QFile::copy(inPath, outPath)) {
            out << "FTMULTI FAIL: khong copy duoc sang " << outPath << "\n";
            out.flush(); return 2;
        }
        // 🔴 0927 LƯỢT 3: nhớ KÍCH THƯỚC + checksum tệp SAU khi copy, để sau
        // khi lưu mà báo "ok" thì ta biết thật sự tệp có đổi. Lượt 2 báo
        // guiSave ok=true trong khi out.pdf vẫn y hệt tệp gốc (2928 byte) —
        // dấu hiệu lớn nhất của việc "thành công giả". Giờ ta tự đối chiếu.
        const qint64 sizeBefore = QFileInfo(outPath).size();
        QFile::remove(outPath + ".prebak");
        QFile::copy(outPath, outPath + ".prebak");
        // 3 ngắt cứng + đoạn phải ngắt mềm vì vượt bề rộng ô 200pt.
        // UTF-8 THẬT (không \uXXXX: QStringLiteral không giải escape unicode).
        const QString multi = QString::fromUtf8(
            "Dòng một\n"
            "Dòng hai tiếng Việt có dấu\n"
            "Dòng ba rất dài để buộc phải tự ngắt mềm vì vượt bề rộng ô hai trăm điểm");
        const float  fontSize = 12.0f;
        const double boxW = 200.0, boxH = 40.0;
        double rectW = boxW, rectH = boxH;
        int lines = 0;
        // 🔴 0927 LƯỢT 2: đi qua MainWindow + đường Save THẬT (onSaveFile),
        // đúng như lúc người dùng bấm Ctrl+S. Lượt trước probe tự mở
        // PdfDocument rồi gọi mgr.saveDocument() — KHÔNG phải đường đó, nên
        // không materialize pagesNeedGenerate, không giải phóng handle
        // PDFium, không replaceFileAtomically ⇒ tệp ra không giống tệp thật.
        MainWindow w;
        w.resize(1280, 900);
        w.show();
        QCoreApplication::processEvents();
        w.openFile(outPath);
        for (int i = 0; i < 240; ++i) {          // chờ trang 1 vẽ xong
            QCoreApplication::processEvents();
            QThread::msleep(25);
            _dismissModals();
            if (i > 40 && w.probePageCount() > 0) break;
        }
        {
            auto* tb = w.currentTabForProbe();
            if (!tb || !tb->annotMgr) {
                out << "FTMULTI FAIL: khong mo duoc tab\n"; out.flush(); return 1;
            }
            const QRectF fit = trFreeTextFitRect(
                QRectF(60.0, 600.0, boxW, boxH), multi, fontSize);
            rectW = fit.width(); rectH = fit.height();
            if (!tb->annotMgr->createInlineNote(0, fit, multi, QStringLiteral("ftmulti"),
                                                false, QColor(0, 0, 0), fontSize)) {
                out << "FTMULTI FAIL: createInlineNote=" << tb->annotMgr->lastError() << "\n";
                out.flush(); return 1;
            }
        }
        QString saveErr;
        const bool saved = w.probeSaveViaGuiAsync(&saveErr);
        const qint64 sizeAfter = QFileInfo(outPath).size();
        qWarning().noquote() << "[ftmulti-probe] guiSave ok=" << saved
                             << " err=" << saveErr
                             << " sizeBefore=" << sizeBefore
                             << " sizeAfter=" << sizeAfter;
        if (!saved) {
            out << "FTMULTI FAIL: save qua onSaveFile that bai: " << saveErr << "\n";
            out.flush(); return 1;
        }
        // 🔴 0927 LƯỢT 3: TỰ ĐÓNG cửa sổ TRƯỚC khi đọc lại tệp. onSaveFile
        // gọi loadTabFile mở lại tệp ⇒ PDFium còn giữ handle trên out.pdf;
        // đọc bằng doc thứ hai lúc đó có thể đọc trúng dữ liệu CŨ trong
        // cache. Đóng hẳn MainWindow (giải phóng handle + pool) rồi MỞ LẠI
        // tệp đã lưu bằng PDFium MỚI, đếm số lệnh <hex> Tj trong /AP/N.
        w.close();
        for (int i = 0; i < 80; ++i) { QCoreApplication::processEvents(); QThread::msleep(25); }
        QString apText;
        lines = _ftApLineCount(outPath, 0, -1, &apText);
        const bool hasAp = !apText.isEmpty();
        // Đối chiếu tệp THẬT SỰ đã đổi (phòng "thành công giả" lần nữa).
        const bool changed = (sizeAfter != sizeBefore) || (QFileInfo(outPath + ".prebak").size() != sizeAfter);
        // 🔴 0927 LƯỢT 3 — ĐỌC LẠI BẰNG ĐÚNG ĐƯỜNG MỞ CỦA APP, và trả lời
        // câu hỏi mục 3: "hình trên màn hình lấy từ overlay hay từ /AP?"
        // Ta mở MainWindow thật, nạp tệp ĐÃ LƯU, đọc /Contents mà app đọc được,
        // rồi vẽ CHÍNH overlay của app (drawFreeTextOverlay) ra QImage và ĐẾM
        // SỐ DÒNG CÓ NÉT ĐEN. Đếm ở tầng app, không phải bằng trình đọc ngoài.
        int reopenLines = -1, reopenBlackPx = 0, paintByOverlay = -1;
        {
            MainWindow w2;
            w2.resize(1280, 900);
            w2.show();
            QCoreApplication::processEvents();
            w2.openFile(outPath);
            for (int i = 0; i < 240; ++i) {
                QCoreApplication::processEvents();
                QThread::msleep(25);
                if (i > 40 && w2.probePageCount() > 0) break;
            }
            auto* tb2 = w2.currentTabForProbe();
            if (tb2 && tb2->annotMgr) {
                const QList<AnnotInfo> list = tb2->annotMgr->loadPage(0);
                QString reopened;
                for (const AnnotInfo& ai : list)
                    if (ai.type == QLatin1String("FreeText") && !ai.text.isEmpty())
                        reopened = ai.text;
                reopenLines = trWrapFreeText(reopened, 192.0,
                                             QFontMetricsF(trDejaVuFontAtPixelSize(12.0))).size();
                // paintByOverlay của annot vừa đọc: quyết định chữ do
                // overlay hay do ảnh nền (raster) vẽ. Dùng CHÍNH hàm app dùng.
                bool capable2 = true;
                const QList<AnnotVisual> vis =
                    tb2->annotMgr->loadPageVisuals(0, &capable2);
                for (const AnnotVisual& av : vis)
                    if (!av.text.isEmpty()) paintByOverlay = av.paintByOverlay ? 1 : 0;
                // Vẽ overlay thật của app rồi đếm nét đen theo từng dòng y.
                QImage img(420, 200, QImage::Format_ARGB32);
                img.fill(Qt::white);
                {
                    QPainter pp(&img);
                    drawFreeTextOverlay(pp, QRectF(10, 10, 200, 77.8438), reopened,
                                        12.0f, 1.0, QColor(0, 0, 0));
                }
                for (int y = 0; y < img.height(); ++y) {
                    bool rowHasInk = false;
                    for (int x = 0; x < img.width() && !rowHasInk; ++x)
                        if (qGray(img.pixel(x, y)) < 128) rowHasInk = true;
                    if (rowHasInk) ++reopenBlackPx;
                }
            }
            w2.close();
            for (int i = 0; i < 60; ++i) { QCoreApplication::processEvents(); QThread::msleep(25); }
        }
        out << "FTMULTI lines=" << lines << " hasAP=" << (hasAp ? 1 : 0)
            << " rect=" << rectW << "x" << rectH
            << " saved=" << (saved ? 1 : 0)
            << " changed=" << (changed ? 1 : 0)
            << " size=" << sizeAfter
            << " reopenLines=" << reopenLines
            << " paintByOverlay=" << paintByOverlay
            << " overlayInkRows=" << reopenBlackPx
            << "\n";
        out.flush();
        QFile::remove(outPath + ".prebak");
        return (lines >= 4 && hasAp && changed) ? 0 : 1;  // 3 ngắt cứng + >=1 mềm
    }

    // usage: --textdlg-probe <in.pdf> [out.pdf]
    // Mo MainWindow + hộp nhập Text NHƯ LÚC NGƯỜI DÙNG KÉO Ô (rect cố định
    // trang 1). Người chấm gõ phím THẬT: Dong 1{ENTER}Dong 2^{ENTER}
    // rồi bấm OK. Probe chờ hộp thoại đóng, rồi in ra log + stdout:
    //   TEXTDLG contents=<...> lines=<n>
    if (argc >= 3 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--textdlg-probe")) {
        auto stripQ2 = [](QString v) {
            if (v.startsWith(QLatin1Char('"')) && v.endsWith(QLatin1Char('"')))
                return v.mid(1, v.size() - 2);
            return v;
        };
        const QString inPath = stripQ2(QString::fromLocal8Bit(argv[2]));
        const QString savePath = (argc >= 4) ? stripQ2(QString::fromLocal8Bit(argv[3]))
                                            : inPath + QStringLiteral(".textdlg.pdf");
        QTextStream out(stdout);
        if (!QFile::exists(inPath)) {
            out << "TEXTDLG FAIL: file dau vao khong ton tai: " << inPath << "\n";
            out.flush(); return 2;
        }
        QFile::remove(savePath);
        if (!QFile::copy(inPath, savePath)) {
            out << "TEXTDLG FAIL: khong copy duoc sang " << savePath << "\n";
            out.flush(); return 2;
        }

        MainWindow w;
        w.resize(1280, 900);
        w.show();
        QCoreApplication::processEvents();
        w.openFile(savePath);
        for (int i = 0; i < 240; ++i) {           // cho trang 1 ve xong
            QCoreApplication::processEvents();
            QThread::msleep(25);
            if (i > 40 && w.probePageCount() > 0) break;
        }
        // Mở hộp nhập Text ĐÚNG đường GUI: bắn tín hiệu textBoxRequested y
        // như lúc người dùng kéo ô trên trang 1 (rect cố định, 200pt).
        auto* tab = w.currentTabForProbe();
        if (!tab || !tab->view) {
            out << "TEXTDLG FAIL: khong mo duoc tab\n"; out.flush(); return 1;
        }
        // 🔴 0927 LƯỢT 3 — CÁCH ĐÚNG: bắn tín hiệu rồi bơm QEventLoop THẬT để
        // hộp NoteInputDialog (mở bằng QDialog::exec() — vòng event lồng) hiện
        // lên và nhận phím như lúc người dùng ngồi gõ tay.
        // KHÔNG dùng Qt::DirectConnection ở đây: invokeMethod theo TÊN TÍN HIỆU
        // với DirectConnection sẽ phát lại chính tín hiệu đó (đệ quy vô hạn).
        // invokeMethod mặc định (Queued) là đúng; chỉ cần vòng chờ bơm event thật.
        // 🔴 0927 LƯỢT 4 — SỬA LỖI TREO (đo 27/09: chạy >100 s không in TEXTDLG).
        // NGUYÊN NHÂN: processEvents() gọi ở dưới CHẠY VÀO exec() LỒNG của
        // NoteInputDialog (MainWindow mở hộp bằng dlg.exec()). exec() lồng chặn
        // processEvents() ⇒ đoạn "tạo waitLoop" ở dưới KHÔNG BAO GIỜ chạy. Hộp
        // hiện (người chấm gõ được) nhưng sau khi bấm OK, poll chưa từng thấy
        // modal nên `sawDialog` mãi = false ⇒ waitLoop chờ trọn 600 s rồi mới
        // thoát. Đúng triệu chứng: treo, không in TEXTDLG.
        //
        // CÁCH SỬA: dựng waitLoop + timer TRƯỚC, rồi mới bắn tín hiệu. Nested
        // exec() lồng vào waitLoop là cách bình thường: hộp hiện, nhận phím thật,
        // và khi nó đóng control quay lại waitLoop ⇒ poll thấy modal biến mất ⇒
        // quit. MainWindow đã tạo xong annot trước khi ta đọc lại.
        const bool autoType = !qEnvironmentVariableIsEmpty("TORREADER_TEXTDLG_AUTOTYPE");
        {
            QEventLoop waitLoop;
            // Người chấm gõ tay: 600 s. Tự điền: 30 s là đủ, hết là treo ⇒ báo
            // FAIL + thoát thay vì treo vô hạn.
            QTimer::singleShot(autoType ? 30000 : 600000, &waitLoop, &QEventLoop::quit);
            bool sawDialog = false;
            QTimer poll;
            poll.setInterval(100);
            // 🔴 KHÔNG dựa vào activeModalWidget(): trong vòng exec() LỒNG trên
            // Windows nó có thể NULL ⇒ poll không bao giờ thấy hộp ⇒ treo (đã đo
            // 27/09). Dò trực tiếp cửa sổ con đang MỞ thay vì hỏi "modal không".
            QObject::connect(&poll, &QTimer::timeout, [&]() {
                // Dò hộp nhập nhiều dòng: có QPlainTextEdit + tiêu đề chứa
                // "text" (MainWindow đặt "Add text"). isVisible() có thể chưa
                // đúng lúc trong vòng exec() lồng ⇒ KHÔNG dựa vào nó.
                QWidget* dlgNow = nullptr;
                for (QWidget* w : QApplication::topLevelWidgets()) {
                    if (!w->isWindow()) continue;
                    if (!w->findChild<QPlainTextEdit*>()) continue;
                    if (!w->windowTitle().contains(QStringLiteral("text"), Qt::CaseInsensitive)) continue;
                    dlgNow = w; break;
                }
                if (dlgNow) sawDialog = true;
                else if (sawDialog) { waitLoop.quit(); return; }
                if (!dlgNow || !autoType) return;
                QPlainTextEdit* ed = dlgNow->findChild<QPlainTextEdit*>();
                if (!ed) { qWarning() << "[textdlg] khong tim thay QPlainTextEdit"; return; }
                ed->setFocus();
                // Đi đúng đường sự kiện bàn phím: Enter = XUỐNG DÒNG, đúng như
                // người dùng gõ tay. Dùng QKeyEvent có text() chứ không dùng
                // setPlainText — phải chứng minh bàn phím thật chạy.
                auto tap = [ed](QEvent::Type ty, int key, Qt::KeyboardModifiers m, const QString& t) {
                    QApplication::sendEvent(ed, new QKeyEvent(ty, key, m, t));
                };
                for (QChar c : QStringLiteral("Dong 1")) {
                    tap(QEvent::KeyPress, 0, Qt::NoModifier, QString(c));
                    tap(QEvent::KeyRelease, 0, Qt::NoModifier, QString(c));
                }
                tap(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier, QStringLiteral("\r"));
                tap(QEvent::KeyRelease, Qt::Key_Return, Qt::NoModifier, QStringLiteral("\r"));
                for (QChar c : QStringLiteral("Dong 2")) {
                    tap(QEvent::KeyPress, 0, Qt::NoModifier, QString(c));
                    tap(QEvent::KeyRelease, 0, Qt::NoModifier, QString(c));
                }
                qWarning().noquote() << "[textdlg] da go 2 dong, text=" << ed->toPlainText();
                // Ctrl+Enter = OK (QShortcut bắt ở mức cửa sổ).
                tap(QEvent::KeyPress, Qt::Key_Return, Qt::ControlModifier, QString());
                tap(QEvent::KeyRelease, Qt::Key_Return, Qt::ControlModifier, QString());
            });
            poll.start();

            fprintf(stdout, "TEXTDLG: hay go 'Dong 1{ENTER}Dong 2^{ENTER}' roi bam OK\n");
            fflush(stdout);

            // Bắn tín hiệu SAU khi waitLoop đã sẵn sàng. Queued (mặc định) là
            // đúng: DirectConnection sẽ phát lại chính tín hiệu đó (đệ quy vô hạn).
            QMetaObject::invokeMethod(tab->view, "textBoxRequested",
                                      Q_ARG(int, 0),
                                      Q_ARG(QRectF, QRectF(60.0, 600.0, 200.0, 40.0)));
            waitLoop.exec();
            poll.stop();
            if (!sawDialog) {
                fprintf(stdout, "TEXTDLG FAIL: hop thoai khong mo (het thoi gian)\n");
                fflush(stdout);
                return 1;
            }
        }
        for (int i = 0; i < 40; ++i) { QCoreApplication::processEvents(); QThread::msleep(25); }
        for (int i = 0; i < 40; ++i) { QCoreApplication::processEvents(); QThread::msleep(25); }
        for (int i = 0; i < 40; ++i) { QCoreApplication::processEvents(); QThread::msleep(25); }

        // Đọc /Contents + số dòng /AP. Lấy HAI bản: bản app đọc được (đã
        // chuẩn hoá \n) và bản THÔ trong file (phải có \r) — để chấm thấy
        // ngay cả hai vế của đề 5.
        QString contentsRaw, contentsApp;
        int lines = 0;
        {
            auto* tb = w.currentTabForProbe();
            if (tb && tb->annotMgr) {
                const QList<AnnotInfo> list = tb->annotMgr->loadPage(0);
                for (const AnnotInfo& ai : list)
                    if (ai.type == QLatin1String("FreeText") && !ai.text.isEmpty())
                        contentsApp = ai.text;
            }
        }
        // 🔴 0927 LƯỢT 3: LƯU QUA ĐÚNG ĐƯỜNG CỦA NÚT SAVE (onSaveFile) — có
        // materialize pagesNeedGenerate, giải phóng handle PDFium, vá /AP rồi
        // replaceFileAtomically. Gọi saveDocument() trực tiếp (lượt trước) KHÔNG
        // đi qua các bước đó ⇒ tệp ra không giống tệp người dùng thật sự lưu.
        QString saveErr;
        const bool saved = w.probeSaveViaGuiAsync(&saveErr);
        if (!saved) {
            fprintf(stdout, "TEXTDLG FAIL: save that bai: %s\n",
                    saveErr.toUtf8().constData());
            fflush(stdout);
        }
        w.close();   // đóng hẳn trước khi đọc lại tệp, giải phóng handle
        for (int i = 0; i < 80; ++i) { QCoreApplication::processEvents(); QThread::msleep(25); }
        QString apText;
        lines = _ftApLineCount(savePath, 0, -1, &apText);
        const bool hasAp = !apText.isEmpty();
        {   // /Contents THÔ trong file (đọc PDFium không qua readAnnotString).
            PdfDocument::libAddRef();
            PdfDocument d2;
            if (d2.open(savePath)) {
                QMutexLocker lk(&s_pdfiumMutex);
                FPDF_PAGE p2 = FPDF_LoadPage(d2.raw(), 0);
                if (p2) {
                    // 🔴 0927 LƯỢT 3: tự dò annot FreeText, không hard-code 0 —
                    // /Annots do QPDF viết lại nên thứ tự có thể đổi.
                    FPDF_ANNOTATION a2 = nullptr;
                    const int n2 = FPDFPage_GetAnnotCount(p2);
                    for (int i2 = 0; i2 < n2; ++i2) {
                        FPDF_ANNOTATION c2 = FPDFPage_GetAnnot(p2, i2);
                        if (!c2) continue;
                        const bool isFree = (FPDFAnnot_GetSubtype(c2) == FPDF_ANNOT_FREETEXT);
                        FPDFPage_CloseAnnot(c2);
                        if (isFree) { a2 = FPDFPage_GetAnnot(p2, i2); break; }
                    }
                    if (a2) {
                        unsigned long n = FPDFAnnot_GetStringValue(a2, "Contents", nullptr, 0);
                        if (n > 2) {
                            std::vector<char16_t> b2(n / 2 + 1, 0);
                            FPDFAnnot_GetStringValue(a2, "Contents",
                                                     reinterpret_cast<FPDF_WCHAR*>(b2.data()), n);
                            contentsRaw = QString::fromUtf16(b2.data());
                        }
                        FPDFPage_CloseAnnot(a2);
                    }
                    FPDF_ClosePage(p2);
                }
            }
            PdfDocument::libRelease();
        }
        auto flatOf = [](QString v) {
            v.replace(QLatin1Char('\r'), QLatin1String("\\r"));
            v.replace(QLatin1Char('\n'), QLatin1String("\\n"));
            return v;
        };
        // 🔴 0927 LUOT 4 (VIEC B): in DUNG khuon yeu cau cua owner —
        //   TEXTDLG contents=<escape \n> lines=<n> hasAP=<0|1>
        // đồng thứ tự rồi, luôn in ra ca hai dấng (stdout + log).
        const QString flat = flatOf(contentsRaw);
        fprintf(stdout, "TEXTDLG contents=%s lines=%d hasAP=%d\n",
                flat.toUtf8().constData(), lines, hasAp ? 1 : 0);
        fflush(stdout);
        qWarning().noquote() << "TEXTDLG contents=" << flat
                             << "appRead=" << flatOf(contentsApp)
                             << "lines=" << lines << "hasAP=" << (hasAp ? 1 : 0);
        return (lines >= 2 && hasAp && !contentsRaw.isEmpty()) ? 0 : 1;
    }

    // usage: --ftngoai-edit-probe <in.pdf> <out.pdf> <page> <index> <text>
    // 🔴 0927 LUOT 6 / VIEC 1 (SPEC 0927 BUOC 2) - sua FreeText cua PHAN MEM
    // KHAC bang DUNG DUONG CHUOT PHAI nguoi dung (chuot phai -> "Edit text…" ->
    // hop nhap nhieu dong -> OK), LU bang DUNG DUONG Save that, roi DOC LAI
    // tep da luu de in ket qua. KHONG tu khai "dat" - chi in so do.
    // `text` dung "\n" LITERAL (hai ky tu) cho xuong dong.
    // In: FTNGOAI ok=<0|1> lines=<n> hasAP=<0|1> origKept=<0|1> rcRemoved=<0|1>
    // Ca am: go len annot Ink ngoai => FTNGOAI ok=0 lyDo=<...>
    if (argc >= 7 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--ftngoai-edit-probe")) {
        auto stripQ7 = [](QString v) {
            if (v.startsWith(QLatin1Char('"')) && v.endsWith(QLatin1Char('"')))
                return v.mid(1, v.size() - 2);
            return v;
        };
        const QString inPath  = stripQ7(QString::fromLocal8Bit(argv[2]));
        const QString outPath = stripQ7(QString::fromLocal8Bit(argv[3]));
        const int pageIndex   = QString::fromLocal8Bit(argv[4]).toInt();
        const int listIndex   = QString::fromLocal8Bit(argv[5]).toInt();
        // "\n" LITERAL -> xuong dong that. Dung QStringLiteral("\\n") de tranh
        // trinh bien dich C++ an chieu "\n" thanh newline.
        QString newText = QString::fromUtf8(argv[6]);
        newText.replace(QLatin1String("\\n"), QLatin1String("\n"));
        QTextStream out(stdout);
        if (!QFile::exists(inPath)) {
            out << "FTNGOAI ok=0 lyDo=khong ton tai tep dau vao\n"; out.flush(); return 2;
        }
        QFile::remove(outPath);
        if (!QFile::copy(inPath, outPath)) {
            out << "FTNGOAI ok=0 lyDo=khong copy duoc sang tep dau ra\n"; out.flush(); return 2;
        }

        // ---- ĐO LOẠI ANNOT mục tiêu TRƯỚC khi mở app (đọc tệp gốc, không
        // tin lời). Cần biết annot có phải FreeText ngoài không để kết luận
        // đúng (ca âm Ink/Square/Line/PolyLine phải bị TỪ CHỐI).
        QString tgtType;
        bool tgtForeign = false, tgtIsFreeText = false;
        {
            PdfDocument::libAddRef();
            PdfDocument d0;
            if (d0.open(inPath)) {
                QMutexLocker lk(&s_pdfiumMutex);
                FPDF_PAGE p0 = FPDF_LoadPage(d0.raw(), pageIndex);
                if (p0) {
                    FPDF_ANNOTATION a0 = FPDFPage_GetAnnot(p0, listIndex);
                    if (a0) {
                        switch (FPDFAnnot_GetSubtype(a0)) {
                            case FPDF_ANNOT_FREETEXT: tgtType = "FreeText"; tgtIsFreeText = true; break;
                            case FPDF_ANNOT_INK:      tgtType = "Ink";      break;
                            case FPDF_ANNOT_SQUARE:   tgtType = "Square";   break;
                            case FPDF_ANNOT_LINE:     tgtType = "Line";     break;
                            case FPDF_ANNOT_TEXT:     tgtType = "Note";     break;
                            default:                  tgtType = "Khac";     break;
                        }
                        tgtForeign = !(FPDFAnnot_HasKey(a0, "TRUID") || FPDFAnnot_HasKey(a0, "TRID"));
                        FPDFPage_CloseAnnot(a0);
                    }
                    FPDF_ClosePage(p0);
                }
            }
            PdfDocument::libRelease();
        }

        MainWindow w;
        w.resize(1280, 900);
        w.show();
        QCoreApplication::processEvents();
        w.openFile(outPath);
        for (int i = 0; i < 240; ++i) {
            QCoreApplication::processEvents();
            QThread::msleep(25);
            _dismissModals();
            if (i > 40 && w.probePageCount() > 0) break;
        }
        // ---- ĐI ĐÚNG ĐƯỜNG CHUOT PHAI: sửa qua probe gọi editSelectedAnnot.
        QString guiErr;
        const QString r = w.probeEditAnnotViaGui(pageIndex, listIndex, newText, &guiErr);
        for (int i = 0; i < 60; ++i) { QCoreApplication::processEvents(); QThread::msleep(25); _dismissModals(); }
        if (!r.isEmpty()) {
            out << "FTNGOAI ok=0 lyDo=" << guiErr << " (loai=" << tgtType
                << ")\n"; out.flush();
            w.close();
            for (int i = 0; i < 40; ++i) { QCoreApplication::processEvents(); QThread::msleep(25); }
            QFile::remove(outPath);   // that bai => khong de lai rac
            return 1;
        }
        // ---- LƯU bằng DUNG ĐƯỜNG Save thật (onSaveFile), không mgr.saveDocument.
        QString saveErr;
        const bool saved = w.probeSaveViaGuiAsync(&saveErr);
        w.close();
        for (int i = 0; i < 80; ++i) { QCoreApplication::processEvents(); QThread::msleep(25); }
        if (!saved) {
            out << "FTNGOAI ok=0 lyDo=save that bai: " << saveErr << "\n"; out.flush();
            QFile::remove(outPath);
            return 1;
        }
        // ---- ĐỌC LẠI tệp đã lưu: /Contents, /AP, /TR_AP_ORIG, /RC.
        int savedLines = 0;
        bool savedHasAp = false, origKept = false, rcRemoved = false, foundFreeText = false;
        QString savedContents;
        {
            PdfDocument::libAddRef();
            PdfDocument d2;
            if (d2.open(outPath)) {
                QMutexLocker lk(&s_pdfiumMutex);
                FPDF_PAGE p2 = FPDF_LoadPage(d2.raw(), pageIndex);
                if (p2) {
                    const int n2 = FPDFPage_GetAnnotCount(p2);
                    for (int i2 = 0; i2 < n2; ++i2) {
                        FPDF_ANNOTATION a2 = FPDFPage_GetAnnot(p2, i2);
                        if (!a2) continue;
                        if (FPDFAnnot_GetSubtype(a2) != FPDF_ANNOT_FREETEXT) { FPDFPage_CloseAnnot(a2); continue; }
                        // Tim FreeText nào co /TR_CONTENTS_ORIG (= cai da sua).
                        if (!FPDFAnnot_HasKey(a2, "TR_CONTENTS_ORIG")) { FPDFPage_CloseAnnot(a2); continue; }
                        foundFreeText = true;
                        origKept = FPDFAnnot_HasKey(a2, "TR_AP_ORIG") ? true : false;
                        rcRemoved = FPDFAnnot_HasKey(a2, "RC") ? false : true;
                        unsigned long n2c = FPDFAnnot_GetStringValue(a2, "Contents", nullptr, 0);
                        if (n2c > 2) {
                            std::vector<char16_t> b2c(n2c / 2 + 1, 0);
                            FPDFAnnot_GetStringValue(a2, "Contents",
                                reinterpret_cast<FPDF_WCHAR*>(b2c.data()), n2c);
                            savedContents = QString::fromUtf16(b2c.data());
                        }
                        FPDFPage_CloseAnnot(a2);
                        if (foundFreeText) break;
                    }
                    FPDF_ClosePage(p2);
                }
            }
            PdfDocument::libRelease();
        }
        // So dong /Contents (chu hoa \r -> \n nhu app doc) va dong /AP that.
        {
            QString t = savedContents;
            t.replace(QLatin1Char('\r'), QLatin1Char('\n'));
            savedLines = t.count(QLatin1Char('\n')) + 1;
        }
        QString apTxt;
        _ftApLineCount(outPath, pageIndex, -1, &apTxt);
        savedHasAp = !apTxt.isEmpty();
        const int apLines = _ftApLineCount(outPath, pageIndex, -1, nullptr);
        const bool ok = foundFreeText && savedHasAp && origKept && rcRemoved;
        auto flatS7 = [](QString v) {
            v.replace(QLatin1Char('\r'), QLatin1String("\\r"));
            v.replace(QLatin1Char('\n'), QLatin1String("\\n"));
            return v;
        };
        // 🔎 0927 LUOT 7: do khop khung DOC LAI TU /AP DA LUU (khong dung
        // so tinh luc dung). rectBefore = /Rect cua tep DAU VAO.
        double rectBefore[4] = {0, 0, 0, 0};
        {
            // Doc /Rect GOC tu tep dau vao (de in rectBefore).
            PdfDocument::libAddRef();
            PdfDocument d0m;
            if (d0m.open(inPath)) {
                QMutexLocker lk(&s_pdfiumMutex);
                FPDF_PAGE p0m = FPDF_LoadPage(d0m.raw(), pageIndex);
                if (p0m) {
                    FPDF_ANNOTATION a0m = FPDFPage_GetAnnot(p0m, listIndex);
                    if (a0m && FPDFAnnot_GetSubtype(a0m) == FPDF_ANNOT_FREETEXT) {
                        FS_RECTF r0{};
                        if (FPDFAnnot_GetRect(a0m, &r0)) {
                            // 🔎 0927 LUOT 7 (sau review): PHẢI cùng thứ tự
                            // với rectAfter (thô /Rect = x0,y0,x1,y1, y LÊN).
                            // FS_RECTF là (left, top, right, bottom) với top =
                            // maxY ⇒ hoán [0]=x0 [1]=y0 [2]=x1 [3]=y1.
                            rectBefore[0] = qMin(r0.left, r0.right);
                            rectBefore[1] = qMin(r0.top, r0.bottom);
                            rectBefore[2] = qMax(r0.left, r0.right);
                            rectBefore[3] = qMax(r0.top, r0.bottom);
                        }
                        FPDFPage_CloseAnnot(a0m);
                    }
                    FPDF_ClosePage(p0m);
                }
            }
            PdfDocument::libRelease();
        }
        ForeignApFit fitM;
        const bool fitOk = trMeasureForeignApFit(outPath, pageIndex, -1, &fitM);
        auto rectStr = [](const double* r) {
            return QString::number(r[0], 'f', 2) + QLatin1Char(',') +
                   QString::number(r[1], 'f', 2) + QLatin1Char(',') +
                   QString::number(r[2], 'f', 2) + QLatin1Char(',') +
                   QString::number(r[3], 'f', 2);
        };
        out << "FTNGOAI ok=" << (ok ? 1 : 0)
            << " lines=" << savedLines
            << " hasAP=" << (savedHasAp ? 1 : 0)
            << " origKept=" << (origKept ? 1 : 0)
            << " rcRemoved=" << (rcRemoved ? 1 : 0)
            << " apLines=" << apLines
            << " contents=" << flatS7(savedContents).toUtf8().constData()
            << " rectBefore=" << rectStr(rectBefore)
            << " rectAfter=" << (fitOk ? rectStr(fitM.rectAfter) : QStringLiteral("0,0,0,0"))
            << " maxLineW=" << QString::number(fitOk ? fitM.maxLineW : 0.0, 'f', 2)
            << " innerW="   << QString::number(fitOk ? fitM.innerW   : 0.0, 'f', 2)
            << " textH="    << QString::number(fitOk ? fitM.textH    : 0.0, 'f', 2)
            << " innerH="   << QString::number(fitOk ? fitM.innerH   : 0.0, 'f', 2)
            << " clipped="  << (fitOk ? fitM.clipped : 0)
            << "\n";
        out.flush();
        fprintf(stdout, "FTNGOAI ok=%d lines=%d hasAP=%d origKept=%d rcRemoved=%d\n",
                ok ? 1 : 0, savedLines, savedHasAp ? 1 : 0, origKept ? 1 : 0, rcRemoved ? 1 : 0);
        fflush(stdout);
        return ok ? 0 : 1;
    }

    // usage: --sidebar-edit-probe <in.pdf> <out.pdf>
    // 🔴 0927 LƯỢT 4 (VIỆC C) — nghiệm thu ô SỬA COMMENT ở sidebar:
    //   1) tạo FreeText CỦA TA 1 dòng "Ngan" + FreeText 3 dòng (có dấu)
    //   2) mở MainWindow thật, tab Comments
    //   3) đo: hàng 1 editable tại chỗ = 1, hàng 2 readOnly tại chỗ + nút ⤢
    //   4) mở popup hàng 2 bằng ĐÚNG ĐƯỜNG bấm (click giả lập) → in popup/preLines
    //   5) đặt chữ 4 dòng vào popup rồi accept() → lưu → đọc lại → in saved
    // KHÔNG tự khai "đạt" thay người chấm: chỉ in số đo.
    if (argc >= 4 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--sidebar-edit-probe")) {
        auto stripS = [](QString v) {
            if (v.startsWith(QLatin1Char('"')) && v.endsWith(QLatin1Char('"')))
                return v.mid(1, v.size() - 2);
            return v;
        };
        const QString inPath  = stripS(QString::fromLocal8Bit(argv[2]));
        const QString outPath = stripS(QString::fromLocal8Bit(argv[3]));
        QTextStream out(stdout);
        if (!QFile::exists(inPath)) {
            out << "SIDEBAR FAIL: file dau vao khong ton tai: " << inPath << "\n";
            out.flush(); return 2;
        }
        QFile::remove(outPath);
        if (!QFile::copy(inPath, outPath)) {
            out << "SIDEBAR FAIL: khong copy duoc sang " << outPath << "\n";
            out.flush(); return 2;
        }
        // Hàng 1 = chữ NGẮN 1 dòng (sửa tại chỗ được).
        // Hàng 2 = chữ 3 DÒNG có dấu (bắt buộc chỉ-hiển-thị + nút ⤢).
        const QString shortTxt = QString::fromUtf8("Ngan");
        const QString multiTxt = QString::fromUtf8(
            "Dong mot tieng Viet co dau\nDong hai cung co dau\nDong ba ket thuc");
        const QString new4Lines = QString::fromUtf8(
            "Sua dong 1\nSua dong 2 co dau\nSua dong 3\nSua dong 4 het");

        MainWindow w;
        w.resize(1280, 900);
        w.show();
        QCoreApplication::processEvents();
        w.openFile(outPath);
        for (int i = 0; i < 240; ++i) {
            QCoreApplication::processEvents();
            QThread::msleep(25);
            _dismissModals();
            if (i > 40 && w.probePageCount() > 0) break;
        }
        // Tạo 2 FreeText CỦA TA trên trang 1 qua đúng đường app (annotMgr).
        int idxShort = -1, idxMulti = -1;
        {
            auto* tb = w.currentTabForProbe();
            if (!tb || !tb->annotMgr) {
                out << "SIDEBAR FAIL: khong mo duoc tab\n"; out.flush(); return 1;
            }
            const float fs = 12.0f;
            if (!tb->annotMgr->createInlineNote(
                    0, trFreeTextFitRect(QRectF(60, 700, 220, 30), shortTxt, fs),
                    shortTxt, QStringLiteral("sb"), false, QColor(0, 0, 0), fs)) {
                out << "SIDEBAR FAIL: tao FreeText 1 dong that bai\n"; out.flush(); return 1;
            }
            if (!tb->annotMgr->createInlineNote(
                    0, trFreeTextFitRect(QRectF(60, 640, 220, 30), multiTxt, fs),
                    multiTxt, QStringLiteral("sb"), false, QColor(0, 0, 0), fs)) {
                out << "SIDEBAR FAIL: tao FreeText 3 dong that bai\n"; out.flush(); return 1;
            }
            const QList<AnnotInfo> lst = tb->annotMgr->loadPage(0);
            for (const AnnotInfo& ai : lst) {
                if (ai.type != QLatin1String("FreeText")) continue;
                if (idxShort < 0 && !ai.text.contains(QLatin1Char('\n'))) idxShort = ai.indexInPage;
                if (idxMulti < 0 &&  ai.text.contains(QLatin1Char('\n'))) idxMulti = ai.indexInPage;
            }
        }
        // Mở tab Comments THẬT (nút sidebar), rồi nạp danh sách chú thích.
        w.probeSelectSidebarTab(2);
        for (int i = 0; i < 60; ++i) { QCoreApplication::processEvents(); QThread::msleep(25); }
        w.probeRefreshComments(0);
        for (int i = 0; i < 60; ++i) { QCoreApplication::processEvents(); QThread::msleep(25); }

        auto* panel = w.probeThumbPanel();
        if (!panel) { out << "SIDEBAR FAIL: khong lay duoc sidebar\n"; out.flush(); return 1; }
        out << "SIDEBAR rows=" << panel->probeCommentRowCount()
            << " idxShort=" << idxShort << " idxMulti=" << idxMulti << "\n";
        out.flush();

        // ── ĐO HÀNG 1: chữ ngắn 1 dòng ⇒ sửa TẠI CHỖ được (đề 1).
        const auto r1 = panel->probeCommentRow(0, idxShort);
        // ── ĐO HÀNG 2: chữ nhiều dòng ⇒ CHỈ-HIỂN-THỊ + nút ⤢ (đề 2 + 3).
        const auto r2 = panel->probeCommentRow(0, idxMulti);

        out << "SIDEBAR row1 exists=" << (r1.exists ? 1 : 0)
            << " editableInPlace=" << (r1.editableInPlace ? 1 : 0)
            << " tooltipAll=" << (r1.tooltipHasAllText ? 1 : 0) << "\n";
        out << "SIDEBAR row2 exists=" << (r2.exists ? 1 : 0)
            << " readOnlyInPlace=" << (r2.displayOnly ? 1 : 0)
            << " hasExpandBtn=" << (r2.hasExpandBtn ? 1 : 0)
            << " tooltipAll=" << (r2.tooltipHasAllText ? 1 : 0)
            << " shownText=" << r2.text.toUtf8().constData() << "\n";
        out.flush();

        // ── MỞ POPUP hàng 2 bằng ĐÚNG ĐƯỜNG bấm: QPushButton::click() trên nút ⤢.
        const bool opened = panel->probeOpenCommentPopup(0, idxMulti, -1, QString::fromUtf8("\xE2\xA4\xA2"));
        int preLines = -1;
        if (opened) {
            const auto rp = panel->probeCommentRow(0, idxMulti);
            preLines = rp.popupLines;
        }
        out << "SIDEBAR popup=" << (opened ? 1 : 0) << " preLines=" << preLines << "\n";
        out.flush();
        if (!opened) {
            out << "SIDEBAR FAIL: khong mo duoc popup\n"; out.flush(); return 1;
        }
        // ── Đặt chữ 4 DÒNG rồi accept() đúng đường OK ⇒ emit commentTextEdited.
        if (!panel->probeSetPopupTextAndAccept(new4Lines)) {
            out << "SIDEBAR FAIL: khong dat duoc text vao popup\n"; out.flush(); return 1;
        }
        for (int i = 0; i < 40; ++i) { QCoreApplication::processEvents(); QThread::msleep(25); }

        // ── LƯU qua đúng đường nút Save (onSaveFile), rồi đóng để nhả handle.
        QString saveErr;
        const bool saved = w.probeSaveViaGuiAsync(&saveErr);
        if (!saved) {
            out << "SIDEBAR FAIL: save that bai: " << saveErr << "\n"; out.flush(); return 1;
        }
        w.close();
        for (int i = 0; i < 80; ++i) { QCoreApplication::processEvents(); QThread::msleep(25); }

        // ── ĐỌC LẠI /Contents + /AP từ tệp đã lưu (tầng app, không phải tin lời).
        int savedLines = 0;
        bool savedHasAp = false;
        QString savedContents;
        {
            PdfDocument::libAddRef();
            PdfDocument d2;
            if (d2.open(outPath)) {
                QMutexLocker lk(&s_pdfiumMutex);
                FPDF_PAGE p2 = FPDF_LoadPage(d2.raw(), 0);
                if (p2) {
                    const int n2 = FPDFPage_GetAnnotCount(p2);
                    for (int i2 = 0; i2 < n2; ++i2) {
                        FPDF_ANNOTATION c2 = FPDFPage_GetAnnot(p2, i2);
                        if (!c2) continue;
                        const bool isFree = (FPDFAnnot_GetSubtype(c2) == FPDF_ANNOT_FREETEXT);
                        FPDFPage_CloseAnnot(c2);
                        if (!isFree) continue;
                        FPDF_ANNOTATION a2 = FPDFPage_GetAnnot(p2, i2);
                        if (!a2) continue;
                        // Doc /Contents goc trong file (PDFium, KHONG qua ham
                        // tinh cua app) roi chuan hoa \r -> \n nhu app doc.
                        unsigned long n2c = FPDFAnnot_GetStringValue(a2, "Contents", nullptr, 0);
                        if (n2c > 2) {
                            std::vector<char16_t> b2c(n2c / 2 + 1, 0);
                            FPDFAnnot_GetStringValue(a2, "Contents",
                                                     reinterpret_cast<FPDF_WCHAR*>(b2c.data()), n2c);
                            QString t = QString::fromUtf16(b2c.data());
                            t.replace(QLatin1Char('\r'), QLatin1Char('\n'));
                            if (t.contains(QLatin1Char('\n'))) { savedContents = t; FPDFPage_CloseAnnot(a2); break; }
                        }
                        FPDFPage_CloseAnnot(a2);
                    }
                    FPDF_ClosePage(p2);
                }
            }
            PdfDocument::libRelease();
            savedLines = savedContents.count(QLatin1Char('\n')) + 1;
        }
        QString apTxt;
        _ftApLineCount(outPath, 0, -1, &apTxt);
        savedHasAp = !apTxt.isEmpty();
        auto flatS = [](QString v) {
            v.replace(QLatin1Char('\r'), QLatin1String("\\r"));
            v.replace(QLatin1Char('\n'), QLatin1String("\\n"));
            return v;
        };
        out << "SIDEBAR saved lines=" << savedLines
            << " hasAP=" << (savedHasAp ? 1 : 0)
            << " contents=" << flatS(savedContents).toUtf8().constData() << "\n";
        out.flush();
        return 0;
    }

    // usage: --sidebar-popup-hold <in.pdf> [giay] [dark]
    // 🔴 0927 LUOT 5 (LOI 3) - giu popup mo <giay> giay de CEO CHUP ANH.
    // 🔴 0927 LUOT 6 / VIEC 2: them tham so TUY CHON "dark" de BAT
    // Dark Mode truoc khi mo popup (CEO chup ca hai nen de so). Tham so nao
    // nhan dang theo TEN, nen cau lenh cu co van chay: --sidebar-popup-hold
    // <in.pdf> <giay> van y het nhu truoc.
    // Giong het --sidebar-edit-probe, KHAC o cho: sau khi mo popup hang nhieu
    // dong (bang DUNG duong bam nut, QPushButton::click) thi GIU NGUYEN popup
    // mot khoang thoi gian cho den khi lenh het - dung QTimer, KHONG sleep
    // (sleep se lam 1 event loop khong chay => cua so dong va khong ve).
    // In ra: SIDEBAR hold popupGeom=<x,y,w,h> rowGeom=<x,y,w,h> screen=<w,h>
    // Roi huy popup va thoat app.
    if (argc >= 3 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--sidebar-popup-hold")) {
        auto stripH = [](QString v) {
            if (v.startsWith(QLatin1Char('"')) && v.endsWith(QLatin1Char('"')))
                return v.mid(1, v.size() - 2);
            return v;
        };
        const QString inPath = stripH(QString::fromLocal8Bit(argv[2]));
        // Vong 2 (aider-review): holdSec <= 0 se quit TRUOC khi cua so ve =>
        // anh chup ra man hinh trong. San toi thieu 1 giay cho event loop.
        // 🔴 0927 LUOT 6: tach theo TEN cac tham so tuy chon de cau
        // "--sidebar-popup-hold <pdf> 5" cu khong doi y nghia. Chi nhan "dark"
        // (khong phan biet hoa thuong) — thu khong ro thi im, co gi se bao loi.
        double holdSec = 5.0;
        bool wantDark = false;
        for (int ai = 3; ai < argc; ++ai) {
            const QString a = stripH(QString::fromLocal8Bit(argv[ai])).trimmed();
            if (a.compare(QLatin1String("dark"), Qt::CaseInsensitive) == 0) wantDark = true;
            else {
                bool okNum = false;
                const double v = a.toDouble(&okNum);
                if (okNum) holdSec = v;
            }
        }
        holdSec = qMax(1.0, holdSec);
        QTextStream out(stdout);
        if (!QFile::exists(inPath)) {
            out << "SIDEBAR FAIL: file dau vao khong ton tai: " << inPath << "\n";
            out.flush(); return 2;
        }
        // Ban sao rieng: KHONG ghi vao tep dau vao (CEO cap anh, khong sua).
        const QString workPath = inPath + QStringLiteral(".holdwork.pdf");
        QFile::remove(workPath);
        // 0927 LUOT 5: xoa ban lam viec khi app CHET GIUA CHUNG (dong cua so
        // / may tat) — tai cua so khong chay. Xoa file nay tren phan nhanh
        // trong %TEMP% cua chinh may CEO dang dung, KHONG phai tep goc.
        struct HoldWorkGuard {
            QString p;
            ~HoldWorkGuard() { if (!p.isEmpty()) QFile::remove(p); }
        } holdGuard{ workPath };
        if (!QFile::copy(inPath, workPath)) {
            out << "SIDEBAR FAIL: khong copy duoc sang " << workPath << "\n";
            out.flush(); return 2;
        }
        // Hang nhieu dong - dung 3 dong co dau nhu phan nghiem thu truoc.
        const QString multiTxt = QString::fromUtf8(
            "Dong mot tieng Viet co dau\nDong hai cung co dau\nDong ba ket thuc");
        const QString shortTxt = QString::fromUtf8("Ngan");

        MainWindow w;
        w.resize(1280, 900);
        w.show();
        QCoreApplication::processEvents();
        // 🔴 0927 LUOT 6 / VIEC 2: bat Dark Mode TRUOC khi mo tep va truoc
        // khi mo popup, de popup lay dung theme ma CEO dang chup.
        if (wantDark && !w.probeSetDarkMode(true)) {
            out << "SIDEBAR FAIL: khong bat duoc Dark Mode\n"; out.flush(); return 1;
        }
        w.openFile(workPath);
        for (int i = 0; i < 240; ++i) {
            QCoreApplication::processEvents();
            QThread::msleep(25);
            _dismissModals();
            if (i > 40 && w.probePageCount() > 0) break;
        }
        int idxMulti = -1;
        {
            auto* tb = w.currentTabForProbe();
            if (!tb || !tb->annotMgr) {
                out << "SIDEBAR FAIL: khong mo duoc tab\n"; out.flush(); return 1;
            }
            const float fs = 12.0f;
            if (!tb->annotMgr->createInlineNote(
                    0, trFreeTextFitRect(QRectF(60, 700, 220, 30), shortTxt, fs),
                    shortTxt, QStringLiteral("sb"), false, QColor(0, 0, 0), fs) ||
                !tb->annotMgr->createInlineNote(
                    0, trFreeTextFitRect(QRectF(60, 640, 220, 30), multiTxt, fs),
                    multiTxt, QStringLiteral("sb"), false, QColor(0, 0, 0), fs)) {
                out << "SIDEBAR FAIL: tao FreeText that bai\n"; out.flush(); return 1;
            }
            const QList<AnnotInfo> lst = tb->annotMgr->loadPage(0);
            for (const AnnotInfo& ai : lst) {
                if (ai.type != QLatin1String("FreeText")) continue;
                if (idxMulti < 0 && ai.text.contains(QLatin1Char('\n'))) idxMulti = ai.indexInPage;
            }
        }
        w.probeSelectSidebarTab(2);
        for (int i = 0; i < 60; ++i) { QCoreApplication::processEvents(); QThread::msleep(25); }
        w.probeRefreshComments(0);
        for (int i = 0; i < 60; ++i) { QCoreApplication::processEvents(); QThread::msleep(25); }

        auto* panel = w.probeThumbPanel();
        if (!panel) {
            out << "SIDEBAR FAIL: khong lay duoc sidebar\n"; out.flush(); return 1;
        }
        // MO POPUP bang DUNG DUONG BAM NUT (QPushButton::click), nhu nguoi dung.
        if (!panel->probeOpenCommentPopup(0, idxMulti)) {
            out << "SIDEBAR FAIL: khong mo duoc popup\n"; out.flush(); return 1;
        }
        // Vong 2 (aider-review "KHONG CHAC"): openCommentPopup() goi dlg->move()
        // theo sizeHint TRUOC khi widget duoc polish/layout xong, nen geometry
        // doc o day co the lech so voi hinh CEO chup. Bom vong event de layout
        // chay cho xong ROI moi lay toa do.
        for (int i = 0; i < 6; ++i) { QCoreApplication::processEvents(); QThread::msleep(30); }
        QRect pg, rg;
        if (!panel->probePopupGeometry(&pg, &rg)) {
            out << "SIDEBAR FAIL: popup khong con mo\n"; out.flush(); return 1;
        }
        QScreen* scr = QApplication::primaryScreen();
        const QSize sc = scr ? scr->size() : QSize(0, 0);
        out << "SIDEBAR hold dark=" << (wantDark ? 1 : 0)
            << " popupGeom=" << pg.x() << "," << pg.y() << "," << pg.width() << "," << pg.height()
            << " rowGeom=" << rg.x() << "," << rg.y() << "," << rg.width() << "," << rg.height()
            << " screen=" << sc.width() << "," << sc.height() << "\n";
        out.flush();

        // QTimer chay het event loop => cua so con ve, chu khong dung lai.
        QTimer::singleShot(static_cast<int>(holdSec * 1000.0), qApp, []() {
            QCoreApplication::quit();
        });
        const int rc = app.exec();
        panel->closeCommentPopupIfOpen();
        return rc;   // holdGuard xoa ban lam viec
    }

    // usage: --markup-mouse-probe <pdf> [page1Based=1]
    // 🔴 LƯỢT 39p (Sonnet — PROBE TEST-ONLY, KHONG doi hanh vi app): tai hien DUNG
    // duong CHUOT THAT nguoi dung di khi ve markup o Single view ("View Quality &
    // Edit Comments", tuc probeSetFastMode(false) — noi DUY NHAT PdfGpuView nhan
    // mouseMoveEvent de ve markup, xem PdfGpuView::mousePressEvent/mouseMoveEvent/
    // mouseReleaseEvent). Muc dich: CHUNG MINH hay BAC BO loi "sau khi ve markup,
    // overlay khong duoc dung lai" (MainWindow::refreshAnnotVisuals, cong `coAnh`
    // — xem MainWindow.cpp.bak-r39-0930 cho logic TRUOC sua).
    // Gui QMouseEvent qua QApplication::sendEvent toi probeCurrentView() — KHONG
    // goi tat probeCreateNote/probeCreateText — de di DUNG duong tao shapeCommit/
    // textBoxRequested ma nguoi dung that di qua.
    // In (khuon co dinh, doc bang script):
    //   MOUSEPROBE step=<n> tool=<ten> annots=<probeAnnotCount> visuals=<probeVisualCount(page)>
    //               ok=<0|1> t=<0s|3s|10s>
    //   MOUSEPROBE select tool=<ten> selected=<0|1>
    //   MOUSEPROBE step=stress tool=Rectangle6x annots=<n> visuals=<n> ok=<0|1>
    //   MOUSEPROBE step=afterzoom tool=- annots=<n> visuals=<n> ok=<0|1>
    //   MOUSEPROBE: PASS   |   MOUSEPROBE: FAIL <ly do buoc dau tien that bai>
    if (argc >= 3 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--markup-mouse-probe")) {
        auto stripQ3 = [](QString v) {
            if (v.startsWith(QLatin1Char('"')) && v.endsWith(QLatin1Char('"')))
                return v.mid(1, v.size() - 2);
            return v;
        };
        const QString pdfPath = stripQ3(QString::fromLocal8Bit(argv[2]));
        const int page1Based = (argc >= 4) ? QString::fromLocal8Bit(argv[3]).toInt() : 1;
        const int pageIdx = qMax(0, page1Based - 1);
        if (!_probeRequireFile(pdfPath, "file PDF")) return 2;

        QTextStream out(stdout);
        bool overallOk = true;
        QString failReason;
        auto noteFail = [&](const QString& r) {
            if (overallOk) { overallOk = false; failReason = r; }
        };

        MainWindow w;
        w.resize(1600, 1000);
        w.show();
        QCoreApplication::processEvents();

        auto spin = [](int ms) {
            for (int i = 0; i < qMax(1, ms / 25); ++i) {
                QCoreApplication::processEvents();
                QThread::msleep(25);
            }
        };

        w.openFile(pdfPath);
        // Buoc 1: cho tai lieu mo + trang hien ra (toi da 240s — file MEP
        // file MEP mau nang 255MB, 60s khong du theo ghi chu
        // --owner3tab-probe ve file nay).
        {
            const qint64 deadline = QDateTime::currentMSecsSinceEpoch() + 240000;
            while (QDateTime::currentMSecsSinceEpoch() < deadline) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
                QThread::msleep(25);
                _dismissModals();
                auto* tb0 = w.probeCurrentTab();
                if (tb0 && tb0->doc && tb0->doc->isOpen() && w.probePageCount() > 0) break;
            }
        }
        {
            auto* tb0 = w.probeCurrentTab();
            if (!tb0 || !tb0->doc || !tb0->doc->isOpen()) {
                out << "MOUSEPROBE: FAIL tai lieu khong mo duoc trong 60s\n"; out.flush();
                return 2;
            }
        }
        spin(2000);   // trang render lan dau

        // Buoc 2: Single view ("View Quality & Edit Comments") — noi DUY NHAT
        // PdfGpuView nhan chuot de ve markup moi (Continuous KHONG ve markup moi).
        w.probeSetFastMode(false);
        spin(1500);
        w.probeSetPage(pageIdx);
        spin(1500);

        auto* view = qobject_cast<PdfGpuView*>(w.probeCurrentView());
        if (!view) {
            out << "MOUSEPROBE: FAIL khong lay duoc PdfGpuView (probeCurrentView null)\n"; out.flush();
            return 2;
        }
        const QSizeF pageSz = view->pageSizePt();
        if (pageSz.isEmpty()) {
            out << "MOUSEPROBE: FAIL pageSizePt rong — trang chua san sang\n"; out.flush();
            return 2;
        }
        const double W = pageSz.width(), H = pageSz.height();

        // sendClick: gui press -> N move (giu LeftButton) -> release toi DUNG
        // widget cua PdfGpuView, toa do quy doi bang pdfToWidget (page-local,
        // Y-down — CUNG he quy chieu ma PdfGpuView::widgetToPdf dung noi bo,
        // KHONG phai PDF-Y-len-tu-day; xem PdfGpuView::mouseReleaseEvent).
        auto sendClick = [&](QPointF startPagePt, QPointF endPagePt, int moves) {
            const QPointF a = view->pdfToWidget(startPagePt);
            const QPointF b = view->pdfToWidget(endPagePt);
            const QPointF g0 = view->mapToGlobal(a.toPoint());
            QMouseEvent press(QEvent::MouseButtonPress, a, g0,
                              Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(view, &press);
            for (int i = 1; i <= moves; ++i) {
                const QPointF p = a + (b - a) * (double(i) / moves);
                const QPointF g = view->mapToGlobal(p.toPoint());
                QMouseEvent mv(QEvent::MouseMove, p, g,
                              Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
                QApplication::sendEvent(view, &mv);
                QCoreApplication::processEvents();
            }
            const QPointF gb = view->mapToGlobal(b.toPoint());
            QMouseEvent release(QEvent::MouseButtonRelease, b, gb,
                                Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(view, &release);
        };

        // 4 cong cu: id = static_cast<int>(PdfGpuView::ViewTool) (xem PdfGpuView.h:40
        // va MainWindow::probeSelectAnnotTool). Rectangle=4, Cloud=6, Line=2, FreeText=7.
        struct ToolDef { int id; const char* name; QPointF a, b; };
        const QVector<ToolDef> tools = {
            { 4, "Rectangle", QPointF(0.08 * W, 0.10 * H), QPointF(0.25 * W, 0.22 * H) },
            { 6, "Cloud",     QPointF(0.35 * W, 0.10 * H), QPointF(0.52 * W, 0.22 * H) },
            { 2, "Line",      QPointF(0.62 * W, 0.10 * H), QPointF(0.80 * W, 0.22 * H) },
            { 7, "FreeText",  QPointF(0.08 * W, 0.30 * H), QPointF(0.30 * W, 0.36 * H) },
        };
        QVector<QPointF> centers;

        int step = 0;
        for (const ToolDef& td : tools) {
            ++step;
            w.probeSelectAnnotTool(td.id);
            spin(200);

            if (td.id == 7) {
                // FreeText: mouseReleaseEvent goi textBoxRequested -> MainWindow mo
                // NoteInputDialog::exec() (VONG EVENT LONG). Phai dung vong poll SAN
                // SANG TRUOC khi gui su kien release (cung ky thuat --textdlg-probe
                // da chung minh chay duoc o file nay, xem khoi --textdlg-probe o tren):
                // timer duoc Qt xu ly NGAY CA khi dang trong exec() long.
                QTimer poll;
                poll.setInterval(80);
                bool sawDialog = false, typed = false, committed = false;
                int polls = 0;
                QObject::connect(&poll, &QTimer::timeout, [&]() {
                    if (++polls > 150) {   // 12s tran — dung de treo vo han
                        for (QWidget* tw : QApplication::topLevelWidgets()) {
                            if (auto* d = qobject_cast<QDialog*>(tw)) {
                                if (d->isWindow() && d->findChild<QPlainTextEdit*>()
                                    && d->windowTitle().contains(QStringLiteral("text"), Qt::CaseInsensitive)) {
                                    d->reject();
                                    break;
                                }
                            }
                        }
                        poll.stop();
                        return;
                    }
                    QWidget* dlgNow = nullptr;
                    for (QWidget* tw : QApplication::topLevelWidgets()) {
                        if (!tw->isWindow()) continue;
                        if (!tw->findChild<QPlainTextEdit*>()) continue;
                        if (!tw->windowTitle().contains(QStringLiteral("text"), Qt::CaseInsensitive)) continue;
                        dlgNow = tw; break;
                    }
                    if (!dlgNow) return;
                    sawDialog = true;
                    if (typed) return;
                    auto* ed = dlgNow->findChild<QPlainTextEdit*>();
                    if (!ed) return;
                    ed->setFocus();
                    auto tap = [ed](QEvent::Type ty, int key, Qt::KeyboardModifiers mo, const QString& t) {
                        QApplication::sendEvent(ed, new QKeyEvent(ty, key, mo, t));
                    };
                    // Go that: "abc" <Enter=xuong dong> "def" roi Ctrl+Enter = OK
                    // (QShortcut cua NoteInputDialog, xem NoteInputDialog.h).
                    for (QChar c : QStringLiteral("abc")) {
                        tap(QEvent::KeyPress, 0, Qt::NoModifier, QString(c));
                        tap(QEvent::KeyRelease, 0, Qt::NoModifier, QString(c));
                    }
                    tap(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier, QStringLiteral("\r"));
                    tap(QEvent::KeyRelease, Qt::Key_Return, Qt::NoModifier, QStringLiteral("\r"));
                    for (QChar c : QStringLiteral("def")) {
                        tap(QEvent::KeyPress, 0, Qt::NoModifier, QString(c));
                        tap(QEvent::KeyRelease, 0, Qt::NoModifier, QString(c));
                    }
                    typed = true;
                    tap(QEvent::KeyPress, Qt::Key_Return, Qt::ControlModifier, QString());
                    tap(QEvent::KeyRelease, Qt::Key_Return, Qt::ControlModifier, QString());
                    committed = true;
                });
                poll.start();
                sendClick(td.a, td.b, 4);   // release nay se mo QDialog::exec() long
                poll.stop();
                if (!sawDialog) {
                    out << "MOUSEPROBE step=" << step << " tool=FreeText FAIL hop nhap khong mo\n";
                    noteFail(QStringLiteral("FreeText: hop nhap khong mo (bo qua rieng FreeText)"));
                } else if (!committed) {
                    out << "MOUSEPROBE step=" << step << " tool=FreeText FAIL khong go/commit duoc\n";
                    noteFail(QStringLiteral("FreeText: khong go/commit duoc hop nhap"));
                }
            } else {
                sendClick(td.a, td.b, 4);
            }
            centers.append((td.a + td.b) / 2.0);
            spin(300);

            auto printCheck = [&](const char* tSuffix) {
                // LƯỢT 40: dùng per-page count thay vì toàn bộ document
                const int annotsN = w.probeAnnotCountPerPage(pageIdx);
                const int visualsN = w.probeVisualCount(pageIdx);
                const bool okN = (visualsN >= annotsN);
                out << "MOUSEPROBE step=" << step << " tool=" << td.name
                    << " annots=" << annotsN << " visuals=" << visualsN
                    << " ok=" << (okN ? 1 : 0) << " t=" << tSuffix << "\n";
                out.flush();
                if (!okN) noteFail(QStringLiteral("step %1 tool=%2 t=%3 visuals(%4)<annots(%5)")
                                       .arg(step).arg(QLatin1String(td.name)).arg(QLatin1String(tSuffix)).arg(visualsN).arg(annotsN));
            };
            printCheck("0s");
            spin(3000);
            printCheck("3s");
            spin(7000);   // tong ~10s ke tu luc ve xong
            printCheck("10s");
        }

        // Buoc chon: chuyen Pan (id=0), bam giua tung markup vua tao, doc co
        // duoc CHON hay khong qua getter chi-doc probeHasSelection() (LUOT 39p).
        // 🔴 LƯỢT 40 (fix): yêu cầu bấm lần 1 PHẢI chọn được — không còn retry.
        // Hàm onAnnotPick giờ dùng visuals fallback khi lock ban, nên không nên
        // bị bỏ sót click nữa. Nếu vẫn sót thì là lỗi thực, cần fix thêm.
        w.probeSelectAnnotTool(0);
        spin(1500);
        for (int i = 0; i < centers.size() && i < tools.size(); ++i) {
            auto clickCenter = [&]() {
                const QPointF wpt = view->pdfToWidget(centers[i]);
                const QPointF g = view->mapToGlobal(wpt.toPoint());
                QMouseEvent press(QEvent::MouseButtonPress, wpt, g, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                QApplication::sendEvent(view, &press);
                QMouseEvent release(QEvent::MouseButtonRelease, wpt, g, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
                QApplication::sendEvent(view, &release);
            };
            clickCenter();
            spin(300);
            bool selectedFirst = w.probeHasSelection();
            out << "MOUSEPROBE select tool=" << tools[i].name << " selected_first=" << (selectedFirst ? 1 : 0) << "\n";
            out.flush();
            if (!selectedFirst) noteFail(QStringLiteral("select tool=%1 khong chon duoc LAN 1 (fix chưa hết)")
                                              .arg(QLatin1String(tools[i].name)));
        }

        // Buoc nhoi (nhu owner mo ta): 6 markup nua that nhanh, roi zoom in/out
        // that (probeSetZoomSingle — cung API nut +/- Single dung), roi dem lai.
        w.probeSelectAnnotTool(4);   // Rectangle
        spin(100);
        for (int i = 0; i < 6; ++i) {
            const double rowY = 0.45 + (i % 3) * 0.10;
            const double colX = 0.08 + (i / 3) * 0.45;
            sendClick(QPointF(colX * W, rowY * H), QPointF(colX * W + 0.15 * W, rowY * H + 0.08 * H), 3);
            spin(100);
        }
        spin(500);
        {
            // LƯỢT 40: dùng per-page count
            const int annotsN = w.probeAnnotCountPerPage(pageIdx);
            const int visualsN = w.probeVisualCount(pageIdx);
            const bool okN = (visualsN >= annotsN);
            out << "MOUSEPROBE step=stress tool=Rectangle6x annots=" << annotsN
                << " visuals=" << visualsN << " ok=" << (okN ? 1 : 0) << "\n";
            out.flush();
            if (!okN) noteFail(QStringLiteral("stress visuals(%1)<annots(%2)").arg(visualsN).arg(annotsN));
        }

        w.probeSetZoomSingle(2.0);
        spin(1500);
        w.probeSetZoomSingle(1.0);
        spin(1500);
        {
            // LƯỢT 40: dùng per-page count
            const int annotsN = w.probeAnnotCountPerPage(pageIdx);
            const int visualsN = w.probeVisualCount(pageIdx);
            const bool okN = (visualsN >= annotsN);
            out << "MOUSEPROBE step=afterzoom tool=- annots=" << annotsN
                << " visuals=" << visualsN << " ok=" << (okN ? 1 : 0) << "\n";
            out.flush();
            if (!okN) noteFail(QStringLiteral("afterzoom visuals(%1)<annots(%2)").arg(visualsN).arg(annotsN));
        }

        if (overallOk) out << "MOUSEPROBE: PASS\n";
        else out << "MOUSEPROBE: FAIL " << failReason << "\n";
        out.flush();
        return overallOk ? 0 : 1;
    }

    g_trRealAppPath = true;   // LƯỢT 36: đường app thật — thoát như cũ, KHÔNG fast-kill
    MainWindow window;
    window.setWindowTitle("TorReader PDF");
    window.resize(1280, 800);
    window.show();

    // Open file passed via command line (e.g. drag-to-exe)
    // Use QCoreApplication::arguments() (Qt uses GetCommandLineW on Windows,
    // preserving Unicode) instead of argv, which is ANSI codepage (CP1258) and
    // breaks Vietnamese two-sign filenames like "TRIẾN".
    // 0927 LƯỢT 12: Qt trên Windows TỰ DỰNG LẠI argv từ GetCommandLineW ⇒ dù đã bỏ
    // cờ khỏi argv ở đầu main, nó VẪN còn ở arguments(); lọc ở đây để cửa sổ vẫn mở
    // đúng tệp. Không có cờ ⇒ removeAll là no-op, hành vi y như cũ.
    QStringList cliArgs = QCoreApplication::arguments();
    cliArgs.removeAll(QLatin1String("--pool-lock"));     // cờ đã gỡ — vẫn lọc cho tệp cũ
    cliArgs.removeAll(QLatin1String("--pool-song-par"));  // cờ tái hiện của LƯỢT 12
    // 0927 LƯỢT 11: bỏ MỌI cờ trong bảng 0927 (--no-*, --thoat-cham) chứ không chỉ
    // --pool-lock — nếu không, `--thoat-cham` còn trong arguments() và app cứ
    // thử mở một tệp tên "--thoat-cham". Cùng cơ chế với chỗ bỏ cờ khỏi argv.
    {
        int nFlags = 0;
        const TrBisectFlag* flags = trBisectFlags(&nFlags);
        for (int f = 0; f < nFlags; ++f) cliArgs.removeAll(QLatin1String(flags[f].cli));
    }
    if (cliArgs.size() > 1)
        window.openFile(cliArgs.at(1));
#endif

    // 0927 LƯỢT 11 — điểm đặt THOÁT NHANH của app thật. exec() trả về ⇔ cửa sổ đã
    // đóng và closeEvent đã chạy hết (nếu user bấm Cancel thì cửa sổ không đóng và
    // exec() không trả về) ⇒ mọi việc bắt buộc xong, an toàn để huỷ tiến trình.
    // `window` là biến cục bộ ⇒ lệnh này CỐ TÌNH chạy TRƯỚC hàm huỷ của nó.
    const int rc = app.exec();
    trThoatNhanh(rc, &window);
    return rc;
}

// 🔴 LƯỢT 36 (mục 0) — wrapper: probe về tới đây là CÒN task nền sống ⇒ giết tiến
// trình sau khi flush, không chạy hàm huỷ (xem g_trRealAppPath ở đầu file).
int main(int argc, char* argv[]) {
    const int rc = trMainImpl(argc, argv);
    if (g_trRealAppPath) return rc;   // app thật: trThoatNhanh đã xử lý (hoặc --thoat-cham)
    fflush(nullptr);                  // stdout+stderr của probe → file .txt của bat
    QSettings().sync();               // settings probe vừa đổi (dark mode…) không mất
    g_logFile.flush();
    g_logFile.close();
#ifdef Q_OS_WIN
    if (!::TerminateProcess(::GetCurrentProcess(), static_cast<UINT>(rc))) ::_exit(rc);
#else
    _exit(rc);
#endif
    return rc;   // không tới được
}
