#include <QApplication>
#include <QStyleFactory>
#include <QThreadPool>
#include <QThread>
#include <QFontDatabase>
#include <QFile>
#include <QTextStream>
#include <QDateTime>
#include <QMutex>
#include <QDebug>
#include <QLibrary>
#include <QVector>
#include <QHash>
#include <QStringList>
#include <algorithm>
#include <functional>
#ifndef TORREADER_NO_PDFIUM
#include "ui/MainWindow.h"
#include "core/PdfEditor.h"
#include "core/PdfDocument.h"
#include "core/PdfRenderer.h"
#include "core/PdfSigner.h"
#include "core/TextSearch.h"
#include "core/PdfCoords.h"
#include "annotations/AnnotationManager.h"
#include "annotations/AnnotationLayer.h"
#include "annotations/AnnotationTypes.h"
#include "core/ThumbnailRenderPool.h"
#include "core/TileCacheFile.h"
#include <fpdfview.h>
#include <fpdf_edit.h>
#include <fpdf_annot.h>
#include <fpdf_progressive.h>
#include <fpdf_text.h>
extern QMutex s_pdfiumMutex;
#endif
#include "dwf/DWFLoader.h"
#include "viewer/DWFViewer.h"
#include "viewer/DWFMainWindow.h"
#include "viewer/DWFParse.h"
#include "rules/RuleEngine.h"
#include "core/Reporter.h"
#include "ai/AICopilot.h"
#ifdef _WIN32
#include <psapi.h>
#endif
#include <QCoreApplication>
#include <QEventLoop>
#include <QDir>
#include <QImage>
#include <QMap>
#include <QFileDialog>
#include <QListWidget>
#include <QPushButton>
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

static QFile   g_logFile;
static QMutex  g_logMutex;
static bool    g_logAll = false;

static void logHandler(QtMsgType type, const QMessageLogContext&, const QString& msg) {
    if (!g_logAll && !msg.startsWith("[")) return;
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
    QByteArray logEnv = qgetenv("TORREADER_LOG");
    if (!logEnv.isEmpty()) {
        g_logAll = true;
        g_logFile.setFileName(QString::fromLocal8Bit(logEnv));
        g_logFile.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Append);
    } else {
        g_logFile.setFileName("/tmp/torreader_debug.txt");
        g_logFile.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate);
    }
    qInstallMessageHandler(logHandler);
    QApplication::setHighDpiScaleFactorRoundingPolicy(
        Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);

    QApplication app(argc, argv);
    app.setApplicationName("TorReader PDF");
    app.setApplicationVersion(FELIXPDF_VERSION);
    qDebug() << "[gate] app version =" << FELIXPDF_VERSION;
    app.setOrganizationName("Loc Nguyen Huy");
    app.setOrganizationDomain("torreader.cloud");

    // DWF AI headless CLI mode
    if (argc >= 3 && QString::fromLocal8Bit(argv[1]) == "--dwf-info") {
        QTextStream out(stdout);
        DWFLoader loader;
        bool ok = loader.loadFile(QString::fromLocal8Bit(argv[2]));
        if (ok) {
            auto meta = loader.getMetadata();
            qDebug() << "DWF loaded:" << argv[2];
            qDebug() << "  ObjectID:" << meta.objectId;
            qDebug() << "  Version:" << meta.version;
            qDebug() << "  Sections:" << meta.sectionCount;
            qDebug() << "  Title:" << meta.projectName;
            qDebug() << "  Author:" << meta.author;
            qDebug() << "DWF_INFO_OK";
            out << "DWF loaded: " << argv[2] << "\n";
            out << "  ObjectID: " << meta.objectId << "\n";
            out << "  Version: " << meta.version << "\n";
            out << "  Sections: " << meta.sectionCount << "\n";
            out << "  Title: " << meta.projectName << "\n";
            out << "  Author: " << meta.author << "\n";
            // parse W2D geometry → print for headless verification
            {
                DWFParse geoParser;
                if (geoParser.loadDWF(QString::fromLocal8Bit(argv[2]))) {
                    auto w2d = geoParser.parseW2DGeometry();
                    int n = w2d.totalObjects;
                    if (n > 0) {
                        int x0 = (int)w2d.bbox.left();
                        int y0 = (int)w2d.bbox.top();
                        int x1 = (int)w2d.bbox.right();
                        int y1 = (int)w2d.bbox.bottom();
                        out << "  Geometry: sheets=" << w2d.sheets.size()
                            << " objects=" << n
                            << " total=" << w2d.totalPrimitives
                            << " bbox=" << x0 << "," << y0 << "," << x1 << "," << y1 << "\n";
                        int limit = qMin(w2d.sheets.size(), 60);
                        for (int i = 0; i < limit; ++i) {
                            const auto& s = w2d.sheets[i];
                            int sx0 = (int)s.bbox.left();
                            int sy0 = (int)s.bbox.top();
                            int sx1 = (int)s.bbox.right();
                            int sy1 = (int)s.bbox.bottom();
                            int imgCount = 0;
                            for (const auto& o : s.objects)
                                if (o.type == QStringLiteral("image")) ++imgCount;
                            out << "  Sheet " << s.sectionIndex << " \"" << s.name
                                << "\": objects=" << s.objects.size()
                                << " total=" << s.totalPrimitives
                                << " images=" << imgCount
                                << " bbox=" << sx0 << "," << sy0 << "," << sx1 << "," << sy1 << "\n";
                            if (s.sectionIndex < 6) {
                                for (const auto& o : s.objects)
                                    if (o.type == QStringLiteral("image"))
                                        out << "      IMG fmt=" << o.imageFormat
                                            << " " << o.imageCols << "x" << o.imageRows << "\n";
                            }
                        }
                    } else {
                        out << "  Geometry: sheets=0 objects=0\n";
                    }
                }
            }
            out << "DWF_INFO_OK\n";
            out.flush();
        } else {
            qWarning() << "DWF_INFO_FAIL";
            out << "DWF_INFO_FAIL\n";
            out.flush();
        }
        return ok ? 0 : 1;
    }

    // usage: --dwf-uitest <file.dwf> [out.png]  (bấm THỬ MỌI control GUI, kiểm hiệu ứng, ghi PASS/FAIL từng nút)
    if (argc >= 3 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--dwf-uitest")) {
        QString dwf = QString::fromLocal8Bit(argv[2]);
        QString outPng = (argc >= 4) ? QString::fromLocal8Bit(argv[3]) : QString();
        auto* win = new DWFMainWindow(dwf);
        win->setAttribute(Qt::WA_DontShowOnScreen, true);
        win->resize(1500, 950);
        win->show();
        auto pump = [](int n){ for (int i=0;i<n;++i){ QCoreApplication::processEvents(); QThread::msleep(30);} };
        pump(30);

        QFile rf(QCoreApplication::applicationDirPath() + QStringLiteral("/uitest_report.txt"));
        QTextStream rep(&rf);
        if (rf.open(QIODevice::WriteOnly | QIODevice::Text)) rep.setEncoding(QStringConverter::Utf8);
        int pass = 0, total = 0;
        auto CHECK = [&](const QString& name, bool ok){ ++total; if (ok) ++pass;
            if (rf.isOpen()) { rep << (ok?"PASS ":"FAIL ") << name << "\n"; rep.flush(); } };
        auto findAction = [&](const QString& sub)->QAction*{
            for (QAction* a : win->findChildren<QAction*>()) if (a->text().contains(sub)) return a; return nullptr; };
        auto findButton = [&](const QString& sub)->QPushButton*{
            for (QPushButton* b : win->findChildren<QPushButton*>()) if (b->text().contains(sub)) return b; return nullptr; };

        DWFViewer*   vw   = win->findChild<DWFViewer*>();
        DWFRenderer* rd   = vw ? vw->renderer() : nullptr;
        QListWidget* list = win->findChild<QListWidget*>();
        QComboBox*   combo= win->findChild<QComboBox*>();
        QDockWidget* dock = win->findChild<QDockWidget*>();
        QTextBrowser* out = win->findChild<QTextBrowser*>();
        CHECK(QStringLiteral("widgets-found"), vw && rd && list && combo && dock && out);

        // --- Markup tool buttons (Select/Cloud/Line/Arrow/Rect/Text) ---
        if (rd) {
            struct TT { const char* lbl; Tool tool; };
            TT tools[] = {{"Cloud",Tool::Cloud},{"Line",Tool::Line},{"Arrow",Tool::Arrow},
                          {"Rect",Tool::Rect},{"Text",Tool::Text},{"Select",Tool::Select}};
            for (auto& t : tools) {
                QAction* a = findAction(QString::fromLatin1(t.lbl));
                bool ok = a && (a->trigger(), pump(2), rd->currentTool()==t.tool);
                CHECK(QStringLiteral("tool-")+QString::fromLatin1(t.lbl), ok);
            }
        }
        // --- Zoom In / Out / Fit ---
        if (rd) {
            rd->fitView(); pump(3); double zf = rd->zoom();
            QAction* zi = findAction(QStringLiteral("Zoom In"));
            CHECK(QStringLiteral("zoom-in"), zi && (zi->trigger(), pump(3), rd->zoom() > zf));
            double z1 = rd->zoom();
            QAction* zo = findAction(QStringLiteral("Zoom Out"));
            CHECK(QStringLiteral("zoom-out"), zo && (zo->trigger(), pump(3), rd->zoom() < z1));
            rd->setZoom(rd->zoom()*3.0); pump(2); double zbig = rd->zoom();
            QAction* ft = findAction(QStringLiteral("Fit"));
            CHECK(QStringLiteral("fit"), ft && (ft->trigger(), pump(3), rd->zoom() != zbig));
        }
        // --- Sheet navigation Next / Prev / combo ---
        if (vw) {
            int s0 = vw->currentSheetIndex();
            QAction* nx = findAction(QStringLiteral("Next"));
            CHECK(QStringLiteral("next-sheet"), nx && (nx->trigger(), pump(6), vw->currentSheetIndex()!=s0));
            int s1 = vw->currentSheetIndex();
            QAction* pv = findAction(QStringLiteral("Prev"));
            CHECK(QStringLiteral("prev-sheet"), pv && (pv->trigger(), pump(6), vw->currentSheetIndex()!=s1));
            if (combo && combo->count() > 6) {
                int target = (vw->currentSheetIndex() + 5) % combo->count();
                combo->setCurrentIndex(target); pump(6);
                CHECK(QStringLiteral("sheet-combo"), vw->currentSheetIndex()==target);
            } else CHECK(QStringLiteral("sheet-combo"), false);
        }
        // --- AI Check → issues + dock ---
        QAction* aic = findAction(QStringLiteral("Check"));
        if (aic) { aic->trigger(); pump(15); }
        CHECK(QStringLiteral("ai-check-issues"), list && list->count() > 0);
        CHECK(QStringLiteral("ai-check-dock-visible"), dock && dock->isVisible());
        // --- Markup Undo / Clear (AI Check vẽ mây) ---
        if (rd) {
            int mc = rd->markupCount();
            CHECK(QStringLiteral("markups-added"), mc > 0);
            QAction* un = findAction(QStringLiteral("Undo"));
            CHECK(QStringLiteral("undo"), un && mc>0 && (un->trigger(), pump(3), rd->markupCount() < mc));
            QAction* cl = findAction(QStringLiteral("Clear"));
            CHECK(QStringLiteral("clear"), cl && (cl->trigger(), pump(3), rd->markupCount()==0));
        }
        // --- AI Panel toggle (ẩn/hiện dock) ---
        if (dock) {
            QAction* pan = findAction(QStringLiteral("Panel"));
            bool v0 = dock->isVisible();
            bool ok = false;
            if (pan) { pan->trigger(); pump(3); bool v1=dock->isVisible();
                       pan->trigger(); pump(3); bool v2=dock->isVisible();
                       ok = (v1!=v0) && (v2!=v1); }
            CHECK(QStringLiteral("ai-panel-toggle"), ok);
        }
        // --- Panel: Giải thích (AI) đơn (chờ Gemini; lỗi cũng tính là phản hồi) ---
        if (list && list->count() > 0 && out) {
            list->setCurrentRow(0); pump(2);
            QPushButton* ex = findButton(QStringLiteral("(AI)"));
            QString cur = out->toPlainText();
            int aiBefore = cur.count(QStringLiteral("AI:"));
            bool got = false;
            bool isErr = false;
            if (ex) { ex->click();
                QElapsedTimer t; t.start();
                while (t.elapsed() < 30000) { pump(3);
                    cur = out->toPlainText();
                    if (cur.contains(QStringLiteral("Lỗi AI")) || cur.contains(QStringLiteral("Chưa cấu hình"))) {
                        isErr = true; break;
                    }
                    if (cur.count(QStringLiteral("AI:")) > aiBefore) { got = true; break; }
                }
            }
            if (isErr) {
                CHECK(QStringLiteral("panel-explain"), false);
                if (rf.isOpen()) rep << "panel-explain: AI ERROR (no key or API fail), not a real bug\n";
            } else {
                CHECK(QStringLiteral("panel-explain"), got);
            }
        }
        // --- Panel: chat Gửi ---
        if (out) {
            QLineEdit* chat = win->findChild<QLineEdit*>();
            QPushButton* send = findButton(QStringLiteral("Gửi"));
            QString cur = out->toPlainText();
            int aiBefore = cur.count(QStringLiteral("AI:"));
            bool got = false;
            bool isErr = false;
            if (chat && send) { chat->setText(QStringLiteral("Bản vẽ này nói về gì?")); send->click();
                QElapsedTimer t; t.start();
                while (t.elapsed() < 30000) { pump(3);
                    cur = out->toPlainText();
                    if (cur.contains(QStringLiteral("Lỗi AI")) || cur.contains(QStringLiteral("Chưa cấu hình"))) {
                        isErr = true; break;
                    }
                    if (cur.count(QStringLiteral("AI:")) > aiBefore) { got = true; break; }
                }
            }
            if (isErr) {
                CHECK(QStringLiteral("panel-chat"), false);
                if (rf.isOpen()) rep << "panel-chat: AI ERROR (no key or API fail), not a real bug\n";
            } else {
                CHECK(QStringLiteral("panel-chat"), got);
            }
        }
        // --- Panel: Giải thích tất cả (kiểm khởi động queue) ---
        if (list && list->count() > 0 && out) {
            QPushButton* all = findButton(QStringLiteral("tất cả"));
            bool ok = false;
            if (all) { all->click(); pump(6); ok = out->toPlainText().contains(QStringLiteral("[1/")); }
            CHECK(QStringLiteral("panel-explain-all"), ok);
        }
        // --- Nút mở dialog (không tự bấm vì modal): kiểm tồn tại + enabled ---
        { QPushButton* exp = findButton(QStringLiteral(".md"));
          CHECK(QStringLiteral("export-btn-present"), exp != nullptr);
          QAction* op = findAction(QStringLiteral("Open"));
          CHECK(QStringLiteral("open-action-enabled"), op && op->isEnabled()); }
        // --- (a) Text markup thật: auto-answer QInputDialog modal ---
        if (rd) {
            QAction* textAct = findAction(QStringLiteral("Text"));
            int mcBefore = rd->markupCount();
            bool textCreated = false;
            if (textAct) {
                textAct->trigger(); pump(2);
                QTimer::singleShot(150, [&](){
                    for (QWidget* w : QApplication::topLevelWidgets()) {
                        QInputDialog* dlg = qobject_cast<QInputDialog*>(w);
                        if (dlg) { dlg->setTextValue(QStringLiteral("uitest-text")); dlg->accept(); break; }
                    }
                });
                QPoint cp = rd->rect().center();
                QMouseEvent press(QEvent::MouseButtonPress, cp, rd->mapToGlobal(cp), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                QApplication::sendEvent(rd, &press);
                QMouseEvent rel(QEvent::MouseButtonRelease, cp, rd->mapToGlobal(cp), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
                QApplication::sendEvent(rd, &rel);
                pump(10);
                textCreated = rd->markupCount() == mcBefore + 1;
            }
            CHECK(QStringLiteral("text-markup-created"), textCreated);
        }
        // --- (b) Select tool: pick + Delete xoá ---
        if (rd && vw) {
            QAction* rectAct = findAction(QStringLiteral("Rect"));
            bool selectDelOk = false;
            if (rectAct) {
                rectAct->trigger(); pump(2);
                QPoint p1 = rd->rect().center() - QPoint(40, 40);
                QPoint p2 = p1 + QPoint(80, 80);
                QMouseEvent rPress(QEvent::MouseButtonPress, p1, rd->mapToGlobal(p1), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                QApplication::sendEvent(rd, &rPress);
                QMouseEvent rMove(QEvent::MouseMove, p2, rd->mapToGlobal(p2), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                QApplication::sendEvent(rd, &rMove);
                QMouseEvent rRel(QEvent::MouseButtonRelease, p2, rd->mapToGlobal(p2), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
                QApplication::sendEvent(rd, &rRel);
                pump(5);
                int mc1 = rd->markupCount();
                QAction* selAct = findAction(QStringLiteral("Select"));
                if (selAct) {
                    selAct->trigger(); pump(2);
                    QMouseEvent sPress(QEvent::MouseButtonPress, p1, rd->mapToGlobal(p1), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                    QApplication::sendEvent(rd, &sPress);
                    QMouseEvent sRel(QEvent::MouseButtonRelease, p1, rd->mapToGlobal(p1), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
                    QApplication::sendEvent(rd, &sRel);
                    QKeyEvent keyEv(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier);
                    QApplication::sendEvent(vw, &keyEv);
                    pump(3);
                    selectDelOk = rd->markupCount() == mc1 - 1;
                }
            }
            CHECK(QStringLiteral("select-pick-delete"), selectDelOk);
        }
        // --- (c) Click chuột THẬT (không trigger()) ---
        if (rd) {
            QToolBar* tb = win->findChild<QToolBar*>();
            auto realClick = [&](QWidget* w){
                if (!w) return;
                QPoint c = w->rect().center();
                QMouseEvent press(QEvent::MouseButtonPress, c, w->mapToGlobal(c), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                QApplication::sendEvent(w, &press);
                QMouseEvent rel(QEvent::MouseButtonRelease, c, w->mapToGlobal(c), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
                QApplication::sendEvent(w, &rel);
            };
            QAction* cloudAct = findAction(QStringLiteral("Cloud"));
            QWidget* cloudBtn = tb && cloudAct ? tb->widgetForAction(cloudAct) : nullptr;
            int startCount = rd->markupCount();
            for (int i = 0; i < 2; ++i) {
                realClick(cloudBtn);
                pump(2);
                CHECK(QStringLiteral("real-click-cloud-tool-iter%1").arg(i), rd->currentTool() == Tool::Cloud);
                QPoint pA = rd->rect().center() + QPoint(-50 + i * 30, -50);
                QPoint pB = rd->rect().center() + QPoint(50 + i * 30, 50);
                QMouseEvent cPress(QEvent::MouseButtonPress, pA, rd->mapToGlobal(pA), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                QApplication::sendEvent(rd, &cPress);
                QMouseEvent cRel(QEvent::MouseButtonRelease, pA, rd->mapToGlobal(pA), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
                QApplication::sendEvent(rd, &cRel);
                pump(1);
                QMouseEvent cPress2(QEvent::MouseButtonPress, pB, rd->mapToGlobal(pB), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                QApplication::sendEvent(rd, &cPress2);
                QMouseEvent cRel2(QEvent::MouseButtonRelease, pB, rd->mapToGlobal(pB), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
                QApplication::sendEvent(rd, &cRel2);
                pump(1);
                QMouseEvent dbl(QEvent::MouseButtonDblClick, pB, rd->mapToGlobal(pB), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                QApplication::sendEvent(rd, &dbl);
                pump(3);
            }
            CHECK(QStringLiteral("real-click-repeat-registers"), rd->markupCount() == startCount + 2);
        }

        if (rf.isOpen()) { rep << "UITEST " << pass << "/" << total << " passed\n"; rep.flush(); }
        if (!outPng.isEmpty()) win->grab().save(outPng, "PNG");
        fprintf(stderr, "UITEST %d/%d\n", pass, total);
        return 0;
    }

    // usage: --dwf-objects <file.dwf>  (verify: trích object BIM từ ObjectDefinition, dump objects_report.txt)
    if (argc >= 3 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--dwf-objects")) {
        DWFParse parser;
        if (!parser.loadDWF(QString::fromLocal8Bit(argv[2]))) { fprintf(stderr, "OBJECTS_FAIL load\n"); return 1; }
        auto sheets = parser.enumerateSheets();
        QFile rf(QCoreApplication::applicationDirPath() + QStringLiteral("/objects_report.txt"));
        QTextStream rep(&rf);
        int grandTotal = 0, bimLike = 0;
        if (rf.open(QIODevice::WriteOnly | QIODevice::Text)) {
            rep.setEncoding(QStringConverter::Utf8);
            int shownSections = 0;
            for (const auto& sh : sheets.sheets) {
                auto objs = parser.extractObjects(sh.sectionIndex);
                grandTotal += objs.size();
                for (const auto& o : objs)
                    for (auto it = o.props.constBegin(); it != o.props.constEnd(); ++it)
                        if (it.key().contains(QStringLiteral("width"), Qt::CaseInsensitive)
                            || it.key().contains(QStringLiteral("area"), Qt::CaseInsensitive)
                            || it.key().contains(QStringLiteral("height"), Qt::CaseInsensitive)) { ++bimLike; break; }
                if (!objs.isEmpty() && shownSections < 3) {
                    rep << "=== SECTION " << sh.sectionIndex << " : objects=" << objs.size() << " ===\n";
                    int shownObj = 0;
                    for (const auto& o : objs) {
                        rep << "  [" << o.kind << "] id=" << o.id << "\n";
                        for (auto it = o.props.constBegin(); it != o.props.constEnd(); ++it)
                            rep << "      " << it.key() << " = " << it.value().toString() << "\n";
                        if (++shownObj >= 3) { rep << "  ... (còn " << (objs.size()-shownObj) << " object)\n"; break; }
                    }
                    ++shownSections;
                }
            }
            rep << "OBJECTS_OK total_objects=" << grandTotal << " bim_like_props=" << bimLike << "\n";
            rep.flush();
        }
        fprintf(stderr, "OBJECTS_OK total=%d bim_like=%d\n", grandTotal, bimLike);
        return 0;
    }

    // usage: --dwf-props <file.dwf>  (probe: dump property tờ thật từ descriptor ra props_report.txt cạnh exe)
    if (argc >= 3 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--dwf-props")) {
        DWFParse parser;
        if (!parser.loadDWF(QString::fromLocal8Bit(argv[2]))) { fprintf(stderr, "PROPS_FAIL load\n"); return 1; }
        auto sheets = parser.extractSheetProperties();
        QFile rf(QCoreApplication::applicationDirPath() + QStringLiteral("/props_report.txt"));
        QTextStream rep(&rf);
        int totalProps = 0;
        if (rf.open(QIODevice::WriteOnly | QIODevice::Text)) {
            rep.setEncoding(QStringConverter::Utf8);
            int shown = 0;
            for (const auto& m : sheets) {
                rep << "=== SECTION " << m.value(QStringLiteral("__index__")).toInt()
                    << " : " << m.value(QStringLiteral("__section__")).toString() << " ===\n";
                for (auto it = m.constBegin(); it != m.constEnd(); ++it) {
                    if (it.key().startsWith(QStringLiteral("__"))) continue;
                    rep << "  " << it.key() << " = " << it.value().toString() << "\n";
                    ++totalProps;
                }
                if (++shown >= 4) { rep << "... (còn " << (sheets.size() - shown) << " section)\n"; break; }
            }
            rep << "PROPS_OK sections=" << sheets.size() << " props_total=" << totalProps << "\n";
            rep.flush();
        }
        fprintf(stderr, "PROPS_OK sections=%d\n", (int)sheets.size());
        return 0;
    }

    // usage: --dwf-guitest <file.dwf> [out.png]  (tự lái GUI: AI Check → chọn issue → Giải thích → chờ Gemini)
    // Test end-to-end wiring của AICopilotPanel headless (offscreen). Ghi guitest_report.txt cạnh exe.
    if (argc >= 3 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--dwf-guitest")) {
        QString dwf = QString::fromLocal8Bit(argv[2]);
        QString outPng = (argc >= 4) ? QString::fromLocal8Bit(argv[3]) : QString();
        auto* win = new DWFMainWindow(dwf);
        win->setAttribute(Qt::WA_DontShowOnScreen, true);
        win->resize(1500, 950);
        win->show();
        for (int i = 0; i < 30; ++i) { QCoreApplication::processEvents(); QThread::msleep(40); }

        QFile rf(QCoreApplication::applicationDirPath() + QStringLiteral("/guitest_report.txt"));
        QTextStream rep(&rf);
        if (rf.open(QIODevice::WriteOnly | QIODevice::Text)) rep.setEncoding(QStringConverter::Utf8);

        // 1) Trigger action "AI Check"
        QAction* aiCheck = nullptr;
        for (QAction* a : win->findChildren<QAction*>())
            if (a->text() == QLatin1String("AI Check")) { aiCheck = a; break; }
        if (!aiCheck) { if (rf.isOpen()) rep << "GUITEST_FAIL no-aicheck-action\n"; rep.flush();
                        fprintf(stderr, "GUITEST_FAIL no-aicheck-action\n"); return 1; }
        aiCheck->trigger();
        for (int i = 0; i < 20; ++i) { QCoreApplication::processEvents(); QThread::msleep(40); }

        // 2) Issue list populated?
        QListWidget* list = win->findChild<QListWidget*>();
        int issueCount = list ? list->count() : -1;
        if (rf.isOpen()) rep << "issues_in_panel=" << issueCount << "\n";
        if (!list || issueCount <= 0) { if (rf.isOpen()) rep << "GUITEST_FAIL no-issues\n"; rep.flush();
                                        fprintf(stderr, "GUITEST_FAIL no-issues\n"); return 1; }
        list->setCurrentRow(0);
        QCoreApplication::processEvents();

        // 3) Click "Giải thích (AI)"
        QPushButton* explainBtn = nullptr;
        for (QPushButton* b : win->findChildren<QPushButton*>())
            if (b->text().contains(QStringLiteral("Giải thích"))) { explainBtn = b; break; }
        if (!explainBtn) { if (rf.isOpen()) rep << "GUITEST_FAIL no-explain-btn\n"; rep.flush();
                           fprintf(stderr, "GUITEST_FAIL no-explain-btn\n"); return 1; }
        bool btnEnabled = explainBtn->isEnabled();
        if (rf.isOpen()) rep << "explain_btn_enabled=" << (btnEnabled ? "yes" : "no") << "\n";
        explainBtn->click();

        // 4) Chờ Gemini trả lời (panel append "**AI:**") tối đa 30s
        QTextBrowser* out = win->findChild<QTextBrowser*>();
        QElapsedTimer t; t.start();
        bool gotReply = false, gotErr = false;
        while (t.elapsed() < 30000) {
            QCoreApplication::processEvents();
            QThread::msleep(100);
            if (out) {
                QString txt = out->toPlainText();
                if (txt.contains(QStringLiteral("Lỗi AI")) || txt.contains(QStringLiteral("Chưa cấu hình"))) {
                    gotErr = true; break;
                }
                if (txt.contains(QStringLiteral("AI:"))) { gotReply = true; break; }
            }
        }
        QString panelText = out ? out->toPlainText() : QStringLiteral("<no textbrowser>");
        // che key nếu lỡ xuất hiện
        panelText.replace(QRegularExpression(QStringLiteral("AIza[A-Za-z0-9_-]+")), QStringLiteral("<KEY>"));
        if (rf.isOpen()) {
            rep << "got_reply=" << (gotReply ? "yes" : "no") << " got_err=" << (gotErr ? "yes" : "no") << "\n";
            rep << "--- PANEL OUTPUT ---\n" << panelText << "\n";
            rep << ((gotReply && !gotErr) ? "GUITEST_OK\n" : gotErr ? "GUITEST_AI_ERR\n" : "GUITEST_TIMEOUT\n");
            rep.flush();
        }
        // 5) Navigation: mô phỏng click item 0 → focusOnIssue → renderer viewport/zoom phải đổi
        {
            DWFViewer* vw = win->findChild<DWFViewer*>();
            if (vw && vw->renderer() && list->count() > 0) {
                QRectF vpBefore = vw->renderer()->viewport();
                double zBefore = vw->renderer()->zoom();
                QRect ir = list->visualItemRect(list->item(0));
                QPointF cpt = ir.center();
                QPointF gpt = list->viewport()->mapToGlobal(ir.center());
                QMouseEvent press(QEvent::MouseButtonPress, cpt, gpt, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                QApplication::sendEvent(list->viewport(), &press);
                QMouseEvent rel(QEvent::MouseButtonRelease, cpt, gpt, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
                QApplication::sendEvent(list->viewport(), &rel);
                for (int i = 0; i < 12; ++i) { QCoreApplication::processEvents(); QThread::msleep(30); }
                QRectF vpAfter = vw->renderer()->viewport();
                double zAfter = vw->renderer()->zoom();
                bool navChanged = (vpAfter != vpBefore) || (qAbs(zAfter - zBefore) > 1e-6);
                if (rf.isOpen()) { rep << "nav_changed=" << (navChanged ? "yes" : "no")
                                       << " zoom " << zBefore << "->" << zAfter << "\n"; rep.flush(); }
            }
        }
        if (!outPng.isEmpty()) win->grab().save(outPng, "PNG");
        fprintf(stderr, (gotReply && !gotErr) ? "GUITEST_OK issues=%d\n" : gotErr ? "GUITEST_AI_ERR issues=%d\n" : "GUITEST_TIMEOUT issues=%d\n", issueCount);
        return 0;
    }

    // usage: --dwf-aiexplain <file.dwf> [sheetIndex]  (headless, gọi Gemini giải thích issue)
    if (argc >= 3 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--dwf-aiexplain")) {
        QString dwf = QString::fromLocal8Bit(argv[2]);
        auto* viewer = new DWFViewer();
        viewer->setAttribute(Qt::WA_DontShowOnScreen, true);
        viewer->resize(1600, 1000);
        viewer->show();
        QCoreApplication::processEvents();
        if (!viewer->loadFile(dwf)) { fprintf(stderr, "AIEXPLAIN_FAIL load\n"); return 1; }
        QCoreApplication::processEvents();
        auto issues = viewer->runAICheck();
        QCoreApplication::processEvents();
        // WIN32 GUI-subsystem exe: stdout không capture qua cmd redirect → ghi report ra file để verify.
        // Dùng applicationDirPath() (xác định) vì runtime cwd khi launch qua interop không đáng tin.
        QFile repFile(QCoreApplication::applicationDirPath() + QStringLiteral("/aiexplain_report.txt"));
        QTextStream rep(&repFile);
        if (repFile.open(QIODevice::WriteOnly | QIODevice::Text))
            rep.setEncoding(QStringConverter::Utf8);
        AICopilot copilot;
        if (!copilot.hasApiKey()) {
            if (repFile.isOpen()) { rep << "AIEXPLAIN_SKIP no-key issues=" << issues.size() << "\n"; rep.flush(); }
            fprintf(stderr, "AIEXPLAIN_SKIP no-key issues=%d\n", (int)issues.size());
            return 0;
        }
        int done = 0;
        int cap = qMin(issues.size(), 3);
        for (int i = 0; i < cap; ++i) {
            const Issue& iss = issues[i];
            QString rag = AICopilot::retrieveContext(iss.ruleId);
            QString result; bool ok = false;
            QEventLoop loop;
            QObject::connect(&copilot, &AICopilot::reply, [&](const QString& md){ result = md; ok = true; loop.quit(); });
            QObject::connect(&copilot, &AICopilot::failed, [&](const QString& e){ result = e; ok = false; loop.quit(); });
            copilot.explainIssue(iss, QStringLiteral("sheet"), rag);
            loop.exec();
            QObject::disconnect(&copilot, nullptr, nullptr, nullptr);
            if (repFile.isOpen()) {
                rep << "=== ISSUE " << i << " [" << iss.ruleId << "] rag=" << (rag.isEmpty() ? "no" : "yes") << " ===\n"
                    << iss.description << "\nAI: " << result << "\n\n";
                rep.flush();
            }
            printf("=== ISSUE %d [%s] ===\n%s\nAI: %s\n\n",
                   i, iss.ruleId.toUtf8().constData(), iss.description.toUtf8().constData(),
                   result.toUtf8().constData());
            if (ok) ++done;
        }
        if (repFile.isOpen()) { rep << "AIEXPLAIN_OK count=" << done << "/" << cap << "\n"; rep.flush(); }
        fprintf(stderr, "AIEXPLAIN_OK count=%d/%d\n", done, cap);
        return 0;
    }

    // DWF AI rule-check: detect issues and draw markups, optionally save snapshot
    // usage: --dwf-aicheck <file.dwf> [out.png] [W] [H]
    if (argc >= 3 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--dwf-aicheck")) {
        QString dwf = QString::fromLocal8Bit(argv[2]);
        QString out = (argc >= 4) ? QString::fromLocal8Bit(argv[3]) : QString();
        int W = (argc >= 5) ? QString::fromLocal8Bit(argv[4]).toInt() : 1600;
        int H = (argc >= 6) ? QString::fromLocal8Bit(argv[5]).toInt() : 1000;
        if (W < 100) W = 1600;
        if (H < 100) H = 1000;

        auto* viewer = new DWFViewer();
        viewer->setAttribute(Qt::WA_DontShowOnScreen, true);
        viewer->resize(W, H);
        viewer->show();
        QCoreApplication::processEvents();
        if (!viewer->loadFile(dwf)) {
            fprintf(stderr, "AICHECK_FAIL load\n");
            return 1;
        }
        QCoreApplication::processEvents();
        auto issues = viewer->runAICheck();
        QCoreApplication::processEvents();
        for (const auto& iss : issues) {
            printf("[%s] conf=%.2f | %s\n",
                   iss.ruleId.toUtf8().constData(),
                   iss.aiConfidence,
                   iss.description.toUtf8().constData());
        }
        if (!out.isEmpty()) {
            QPixmap pm = viewer->grab();
            bool saved = pm.save(out, "PNG");
            fprintf(stdout, saved ? "SNAP_OK\n" : "SNAP_SAVE_FAIL\n");
        }
        fprintf(stderr, "AICHECK_OK issues=%d\n", (int)issues.size());
        return 0;
    }

    // DWF headless snapshot: grab the full window including toolbar
    // usage: --dwf-snap <file.dwf> <out.png> [W] [H] [zoom] [fx] [fy] [sheet]
    if (argc >= 4 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--dwf-snap")) {
        QString dwf = QString::fromLocal8Bit(argv[2]);
        QString out = QString::fromLocal8Bit(argv[3]);
        int W = (argc >= 5) ? QString::fromLocal8Bit(argv[4]).toInt() : 1920;
        int H = (argc >= 6) ? QString::fromLocal8Bit(argv[5]).toInt() : 1200;
        if (W < 100) W = 1920;
        if (H < 100) H = 1200;
        auto* win = new DWFMainWindow(QString());
        win->setAttribute(Qt::WA_DontShowOnScreen, true);
        win->resize(W, H);
        win->show();
        QCoreApplication::processEvents();
        if (!win->viewer()->loadFile(dwf)) {
            fprintf(stderr, "SNAP_FAIL load\n");
            return 1;
        }
        QCoreApplication::processEvents();
        if (argc >= 10) {   // optional sheet index
            int s = QString::fromLocal8Bit(argv[9]).toInt();
            win->viewer()->goToSheet(s);
            QCoreApplication::processEvents();
        }
        if (argc >= 7) {   // [zoom] [fx] [fy]
            double zoom = QString::fromLocal8Bit(argv[6]).toDouble();
            double fx = (argc >= 8) ? QString::fromLocal8Bit(argv[7]).toDouble() : 0.5;
            double fy = (argc >= 9) ? QString::fromLocal8Bit(argv[8]).toDouble() : 0.5;
            if (zoom > 0) win->viewer()->snapView(zoom, fx, fy);
            QCoreApplication::processEvents();
        }
        QPixmap pm = win->grab();
        bool ok = pm.save(out, "PNG");
        fprintf(stdout, ok ? "SNAP_OK %s (%dx%d)\n" : "SNAP_SAVE_FAIL\n",
                out.toLocal8Bit().constData(), W, H);
        return ok ? 0 : 1;
    }

    // DWF text dump: print all text objects of the richest sheet (x y rot | text) for diagnosis
    if (argc >= 3 && QString::fromLocal8Bit(argv[1]) == "--dwf-textdump") {
        DWFParse gp;
        if (!gp.loadDWF(QString::fromLocal8Bit(argv[2]))) { fprintf(stderr, "load fail\n"); return 1; }
        auto w2d = gp.parseW2DGeometry();
        int best = -1; int bestN = -1;
        for (int i = 0; i < w2d.sheets.size(); ++i)
            if (w2d.sheets[i].objects.size() > bestN) { bestN = w2d.sheets[i].objects.size(); best = i; }
        if (best < 0) { printf("no sheet\n"); return 0; }
        int nText = 0;
        for (const auto& o : w2d.sheets[best].objects) {
            if (o.type == QStringLiteral("text") && !o.points.isEmpty()) {
                printf("%.0f %.0f r%.0f L%d:%s v%d | %s\n", o.points[0].x(), o.points[0].y(),
                       o.rotation, o.layerNum, o.layerName.toUtf8().constData(), o.visible?1:0, o.text.toUtf8().constData());
                ++nText;
            }
        }
        fprintf(stderr, "TEXTDUMP sheet=%d texts=%d\n", best, nText);
        {
            QMap<int,int> layerCount, layerHidden;
            QMap<int,QString> layerNames;
            for (const auto& o : w2d.sheets[best].objects) {
                layerCount[o.layerNum]++;
                if (!o.visible) layerHidden[o.layerNum]++;
                if (!o.layerName.isEmpty() && !layerNames.contains(o.layerNum))
                    layerNames[o.layerNum] = o.layerName;
            }
            for (auto it = layerCount.begin(); it != layerCount.end(); ++it) {
                int ln = it.key();
                fprintf(stderr, "LAYER %d name=%s objs=%d hidden=%d\n",
                        ln, layerNames.value(ln).toUtf8().constData(),
                        it.value(), layerHidden.value(ln));
            }
        }
        return 0;
    }

    // DWF interactive viewer mode
    if (argc >= 3 && QString::fromLocal8Bit(argv[1]) == "--dwf-view") {
        app.setStyle(QStyleFactory::create("Fusion"));
        auto* win = new DWFMainWindow(QString::fromLocal8Bit(argv[2]));
        win->show();
        return app.exec();
    }

    // DWF rule engine test mode
    if (argc >= 3 && QString::fromLocal8Bit(argv[1]) == "--dwf-rules") {
        QTextStream out(stdout);
        QString dwfPath = QString::fromLocal8Bit(argv[2]);
        QString rulesPath = (argc >= 4)
            ? QString::fromLocal8Bit(argv[3])
            : QDir("yaml/rules.yaml").absolutePath();

        // Load DWF for context (non-fatal if fails)
        DWFLoader loader;
        if (loader.loadFile(dwfPath)) {
            auto meta = loader.getMetadata();
            out << "DWF: \"" << meta.projectName << "\"  ID: " << meta.objectId
                << "  v" << meta.version << "  sections: " << meta.sectionCount << "\n";
        } else {
            out << "DWF: " << dwfPath << " (unable to load, rules test continues)\n";
        }

        RuleEngine engine;
        if (!engine.loadRules(rulesPath)) {
            out << "DWF_RULES_FAIL — cannot load rules from " << rulesPath << "\n";
            return 1;
        }
        out << "Rules loaded from: " << rulesPath << "\n";

        auto issues = engine.runChecks();
        out << "Issues found: " << issues.size() << "\n";
        for (const auto& iss : issues) {
            const char* sev = iss.severity == Severity::Critical ? "CRIT"
                            : iss.severity == Severity::Error    ? "ERR"
                            : iss.severity == Severity::Warning  ? "WARN" : "INFO";
            out << "  [" << sev << "] " << iss.ruleId
                << " | " << iss.objectId << " — " << iss.description << "\n";
            if (!iss.suggestion.isEmpty())
                out << "          -> " << iss.suggestion << "\n";
        }
        out << "DWF_RULES_OK\n";
        out.flush();
        return 0;
    }

    // DWF report generation test mode
    if (argc >= 3 && QString::fromLocal8Bit(argv[1]) == "--dwf-report") {
        QTextStream out(stdout);
        QString dwfPath = QString::fromLocal8Bit(argv[2]);
        QString outputDir = (argc >= 4)
            ? QString::fromLocal8Bit(argv[3])
            : QDir::currentPath();
        QString rulesPath = (argc >= 5)
            ? QString::fromLocal8Bit(argv[4])
            : QDir("yaml/rules.yaml").absolutePath();
        QString jsonPath = QDir(outputDir).absoluteFilePath("report.json");
        QString pdfPath  = QDir(outputDir).absoluteFilePath("report.pdf");

        // Load DWF for context
        DWFLoader loader;
        if (loader.loadFile(dwfPath)) {
            auto meta = loader.getMetadata();
            out << "DWF: \"" << meta.projectName << "\"  ID: " << meta.objectId << "\n";
        }

        RuleEngine engine;
        if (!engine.loadRules(rulesPath)) {
            out << "DWF_REPORT_FAIL — cannot load rules from " << rulesPath << "\n";
            return 1;
        }

        auto issues = engine.runChecks();

        // Text report
        Reporter reporter;
        QString report = reporter.generateReport(issues);
        out << report;

        // JSON export
        bool jsonOk = reporter.exportToJSON(issues, jsonPath);
        out << "JSON export: " << (jsonOk ? "OK" : "FAIL") << " -> " << jsonPath << "\n";

        // PDF export
        bool pdfOk = reporter.exportToPDF(issues, pdfPath);
        out << "PDF export: " << (pdfOk ? "OK" : "FAIL") << " -> " << pdfPath << "\n";

        out << "DWF_REPORT_OK\n";
        out.flush();
        return 0;
    }

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
            mgr.moveNote(0, moveIndex, 100.0, 0.0);
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
                    QRectF dispRect = pdfRectToDisp(pdfRect, dispW, dispH, rot);

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
#endif

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
                    FPDF_BOOL strokeFlag = FALSE;
                    if (!FPDFPath_GetDrawMode(obj, &s.fillMode, &strokeFlag)) { s.fillMode = 0; strokeFlag = FALSE; }
                    s.hasStroke = (strokeFlag != FALSE);
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

    MainWindow window;
    window.setWindowTitle("TorReader");
    window.resize(1280, 800);
    window.show();

    // Open file passed via command line (e.g. drag-to-exe)
    if (argc > 1)
        window.openFile(QString::fromLocal8Bit(argv[1]));
#endif

#ifdef TORREADER_NO_PDFIUM
    app.setStyle(QStyleFactory::create("Fusion"));
    QString startFile = (argc > 1) ? QString::fromLocal8Bit(argv[1]) : QString();
    if (startFile.isEmpty()) {
        startFile = QFileDialog::getOpenFileName(nullptr,
            "Open DWF File", QString(), "DWF Files (*.dwf *.dwfx)");
    }
    auto* win = new DWFMainWindow(startFile);
    win->setWindowTitle(QStringLiteral("TorReader DWF Viewer"));
    win->resize(1280, 800);
    win->show();
#endif

    return app.exec();
}
