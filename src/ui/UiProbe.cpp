#include "UiProbe.h"
#include "OcrPanel.h"

#include <QAbstractButton>
#include <QComboBox>
#include <QColor>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QFrame>
#include <QImage>
#include <QLabel>
#include <QMetaObject>
#include <QPixmap>
#include <QPushButton>
#include <QRect>
#include <QSet>
#include <QStatusBar>
#include <QStringList>
#include <QTabBar>
#include <QTextStream>
#include <QToolBar>
#include <QToolButton>
#include <QWidget>
#include <QFile>

namespace {

// Mau DIEM ANH THAT: grab widget roi doc pixel o (w/2, 2) — mep tren giua,
// tranh chu o tam. Day la dau ra sau khi Qt ve xong, khong phai khuon QSS.
QString pixColorOf(QWidget* w) {
    if (!w || w->width() <= 0 || w->height() <= 0) return QStringLiteral("n/a");
    const QPixmap pm = w->grab();
    if (pm.isNull()) return QStringLiteral("n/a");
    const QImage img = pm.toImage();
    if (img.isNull()) return QStringLiteral("n/a");
    const qreal dpr = pm.devicePixelRatio() > 0 ? pm.devicePixelRatio() : 1.0;
    const int x = qBound(0, int((w->width() / 2) * dpr), img.width() - 1);
    const int y = qBound(0, int(2 * dpr), img.height() - 1);
    return img.pixelColor(x, y).name();   // #RRGGBB
}

// Mau o vien tren cung (y=0) — de kiem nut co vien nhin thay hay khong
// (SPec OCRPANEL POLISH loi 3: nut phu bi ep thanh chu thuong khi QSS chung
// dat border:1px solid transparent).
QString edgeColorOf(QWidget* w) {
    if (!w || w->width() <= 0 || w->height() <= 0) return QStringLiteral("n/a");
    const QPixmap pm = w->grab();
    if (pm.isNull()) return QStringLiteral("n/a");
    const QImage img = pm.toImage();
    if (img.isNull()) return QStringLiteral("n/a");
    const qreal dpr = pm.devicePixelRatio() > 0 ? pm.devicePixelRatio() : 1.0;
    const int x = qBound(0, int((w->width() / 2) * dpr), img.width() - 1);
    const int y = qBound(0, int(0 * dpr), img.height() - 1);
    return img.pixelColor(x, y).name();
}

// Mau TRONG ANH CUA SO: doc tu anh grab cua ca cua so tai vi tri cua widget.
// Can co: widget khong tu ve nen (background:transparent) thi w->grab() tra ve
// vung CHUA VE (ra #000000) — khong phai mau owner nhin thay. Doc tu anh cua
// so moi ra mau da tron xong. Day la so de so toolbar voi sidebar.
QString winColorOf(QWidget* w, QWidget* root, const QImage& winImg) {
    if (!w || !root || winImg.isNull() || !w->isVisible()) return QStringLiteral("n/a");
    if (w->width() <= 0 || w->height() <= 0) return QStringLiteral("n/a");
    const QPoint p = w->mapTo(root, QPoint(w->width() / 2, 2));
    const qreal dpr = winImg.devicePixelRatio() > 0 ? winImg.devicePixelRatio() : 1.0;
    const int x = int(p.x() * dpr), y = int(p.y() * dpr);
    if (x < 0 || y < 0 || x >= winImg.width() || y >= winImg.height())
        return QStringLiteral("n/a");
    return winImg.pixelColor(x, y).name();
}

QString describeWidget(QWidget* w, QWidget* root = nullptr, const QImage& winImg = QImage()) {
    if (!w) return QString();
    const QString name = w->objectName().isEmpty() ? QStringLiteral("(none)") : w->objectName();
    QString checked = QStringLiteral("-");
    if (auto* b = qobject_cast<QAbstractButton*>(w))
        checked = b->isChecked() ? QStringLiteral("1") : QStringLiteral("0");
    const QRect g = w->geometry();
    QStringList parts;
    QString lblText;
    if (auto* lbl = qobject_cast<QLabel*>(w)) {
        lblText = lbl->text();
        lblText = lblText.simplified();
        if (lblText.size() > 40) lblText = lblText.left(37) + QStringLiteral("...");
    }
    parts << name
          << QString::fromLatin1(w->metaObject()->className())
          << QStringLiteral("geom=%1,%2,%3,%4").arg(g.x()).arg(g.y()).arg(g.width()).arg(g.height())
          << QStringLiteral("visible=%1").arg(w->isVisible() ? 1 : 0)
          << QStringLiteral("enabled=%1").arg(w->isEnabled() ? 1 : 0)
          << QStringLiteral("checked=%1").arg(checked)
          << QStringLiteral("lbl=%1").arg(lblText.isEmpty() ? QStringLiteral("-") : lblText)
          << QStringLiteral("pix=%1").arg(pixColorOf(w))
          << QStringLiteral("edge=%1").arg(edgeColorOf(w))
          << QStringLiteral("text=%1").arg(w->palette().color(w->foregroundRole()).name())
          << QStringLiteral("win=%1").arg(winColorOf(w, root, winImg));
    // QComboBox: liet ke tung muc kem trang thai enabled (nghiem thu spec OCR
    // langpack: so muc + muc nao disabled khi thieu traineddata).
    if (auto* cb = qobject_cast<QComboBox*>(w)) {
        QStringList items;
        for (int i = 0; i < cb->count(); ++i) {
            const QModelIndex mi = cb->model()->index(i, 0);
            const bool enabled = (cb->model()->flags(mi) & Qt::ItemIsEnabled);
            items << QStringLiteral("[%1]%2=%3")
                         .arg(i)
                         .arg(cb->itemData(i).toString())
                         .arg(enabled ? QStringLiteral("enabled")
                                      : QStringLiteral("disabled"));
        }
        parts << QStringLiteral("items=%1").arg(cb->count())
              << QStringLiteral("items_list=") + items.join(QLatin1String(";"));
    }
    return parts.join(QStringLiteral(" | "));
}

// Qt co phan giai duoc chuoi mau kieu QSS khong (QColor thuan, khong qua QSS).
QString qcolorInfo(const QString& expr) {
    const QColor c(expr);
    return QStringLiteral("QColor(\"%1\") isValid=%2 argb=%3")
        .arg(expr)
        .arg(c.isValid() ? 1 : 0)
        .arg(c.isValid() ? c.name(QColor::HexArgb) : QStringLiteral("-"));
}

// DO THUC NGHIEM: Qt QSS co hieu rgba() alpha thap phan hay khong.
// Widget cha nen trang, widget con dat background = colorExpr, roi doc pixel that.
QString qssRgbaProbe(const QString& colorExpr) {
    QWidget host;
    host.resize(40, 40);
    host.setStyleSheet(QStringLiteral("QWidget{ background:#FFFFFF; }"));
    auto* child = new QWidget(&host);
    child->setObjectName(QStringLiteral("rgbaProbe"));
    child->setGeometry(0, 0, 40, 40);
    child->setStyleSheet(QStringLiteral("QWidget#rgbaProbe{ background:%1; }").arg(colorExpr));
    const QPixmap pm = host.grab();
    const QImage img = pm.isNull() ? QImage() : pm.toImage();
    if (img.isNull())
        return QStringLiteral("qss background:%1 -> pix=n/a").arg(colorExpr);
    const int x = qBound(0, img.width() / 2, img.width() - 1);
    const int y = qBound(0, img.height() / 2, img.height() - 1);
    return QStringLiteral("qss background:%1 -> pix=%2")
        .arg(colorExpr, img.pixelColor(x, y).name());
}

void appendSection(QStringList& out, const QString& title) {
    out << QString() << QStringLiteral("-- ") + title + QStringLiteral(" --");
}

}  // namespace

QString UiProbe::dumpWidgets(QWidget* root, const ThemeTokens& t, bool dark) {
    if (!root) return QStringLiteral("uiprobe: root is null\n");
    QStringList out;

    out << QStringLiteral("# uiprobe dump | %1 | window=%2x%3 | theme=%4")
               .arg(QDateTime::currentDateTime().toString(Qt::ISODate))
               .arg(root->width()).arg(root->height())
               .arg(dark ? QStringLiteral("dark") : QStringLiteral("light"));
    out << QStringLiteral("# format: objectName | class | geom=x,y,w,h | visible | enabled | "
                          "checked | "
                          "pix=<w->grab() tai (w/2,2)> | text=<palette foreground> | "
                          "win=<mau tai cung diem trong anh CA CUA SO>");
    out << QStringLiteral("# luu y: widget khong tu ve nen (background:transparent) thi pix ra "
                          "#000000 vi vung do CHUA VE — doc cot win de biet mau owner nhin thay.");
    // Grab ca cua so MOT lan de doc mau da tron cho tung widget (cot win=).
    const QImage winImg = root->grab().toImage();

    // 1. QToolBar chinh + moi QToolButton con cua no (day la moc so sanh).
    appendSection(out, QStringLiteral("toolbar + toolbuttons"));
    const QList<QToolBar*> bars = root->findChildren<QToolBar*>();
    if (bars.isEmpty()) out << QStringLiteral("(khong tim thay QToolBar)");
    for (QToolBar* bar : bars) {
        out << describeWidget(bar, root, winImg);
        for (QToolButton* b : bar->findChildren<QToolButton*>())
            out << QStringLiteral("  ") + describeWidget(b, root, winImg);
    }

    // 2. 5 nut sidebar: QPushButton co objectName "sidebarTab" (ThumbnailPanel).
    appendSection(out, QStringLiteral("sidebar tabs (objectName=sidebarTab)"));
    const QList<QPushButton*> allPush = root->findChildren<QPushButton*>();
    int sidebarCount = 0;
    for (QPushButton* b : allPush) {
        if (b->objectName() == QLatin1String("sidebarTab")) {
            out << describeWidget(b, root, winImg);
            ++sidebarCount;
        }
    }
    out << QStringLiteral("count sidebarTab=%1").arg(sidebarCount);

    // 3. QWidget ten sidebarTabGrid.
    appendSection(out, QStringLiteral("sidebarTabGrid"));
    const QList<QWidget*> grids = root->findChildren<QWidget*>(QStringLiteral("sidebarTabGrid"));
    if (grids.isEmpty()) out << QStringLiteral("(khong tim thay sidebarTabGrid)");
    for (QWidget* g : grids) out << describeWidget(g, root, winImg);

    // 3b. Panel OCR (tab id 5, SPEC_OCR_TAB) — toan bo con de kiem visible=1.
    appendSection(out, QStringLiteral("ocr panel (OcrPanel)"));
    const QList<OcrPanel*> ocrPanels = root->findChildren<OcrPanel*>();
    if (ocrPanels.isEmpty()) {
        out << QStringLiteral("(khong tim thay OcrPanel)");
    } else {
        for (OcrPanel* op : ocrPanels) {
            out << describeWidget(op, root, winImg);
            for (QWidget* w : op->findChildren<QWidget*>())
                out << QStringLiteral("  ") + describeWidget(w, root, winImg);
        }
    }

    // 4. Luoi cong cu panel Comments (m_toolButtons, objectName=markupTool).
    appendSection(out, QStringLiteral("comments tool grid (objectName=markupTool)"));
    int markupCount = 0;
    for (QPushButton* b : allPush) {
        if (b->objectName() == QLatin1String("markupTool")) {
            out << describeWidget(b, root, winImg);
            ++markupCount;
        }
    }
    out << QStringLiteral("count markupTool=%1").arg(markupCount);

    // 5. QStatusBar, QTabBar thanh tab tai lieu, QFrame ocrBar (neu dang hien).
    appendSection(out, QStringLiteral("statusbar / doc tabbar / ocrBar"));
    for (QStatusBar* sb : root->findChildren<QStatusBar*>())
        out << describeWidget(sb, root, winImg);
    for (QTabBar* tb : root->findChildren<QTabBar*>())
        out << describeWidget(tb, root, winImg);
    const QList<QFrame*> ocrBars = root->findChildren<QFrame*>(QStringLiteral("ocrBar"));
    if (ocrBars.isEmpty()) {
        out << QStringLiteral("ocrBar: (khong ton tai)");
    } else {
        for (QFrame* f : ocrBars)
            out << (f->isVisible() ? describeWidget(f, root, winImg)
                                   : QStringLiteral("ocrBar: co nhung dang an (visible=0)"));
    }

    // 6. Bang token dang dung + mau sau khi Qt phan giai.
    appendSection(out, QStringLiteral("theme tokens"));
    out << QStringLiteral("theme=%1  bg=%2 bgAlt=%3 border=%4 focus=%5 selBg=%6 hoverBg=%7")
               .arg(dark ? QStringLiteral("dark") : QStringLiteral("light"),
                    QString::fromLatin1(t.bg), QString::fromLatin1(t.bgAlt),
                    QString::fromLatin1(t.border), QString::fromLatin1(t.focus),
                    QString::fromLatin1(t.selBg), QString::fromLatin1(t.hoverBg));
    out << qcolorInfo(QStringLiteral("rgba(15,74,133,0.28)"));
    out << qcolorInfo(QString::fromLatin1(t.selBg));
    out << qcolorInfo(QString::fromLatin1(t.hoverBg));
    out << qcolorInfo(QString::fromLatin1(t.focus));
    out << QStringLiteral("tham chieu: QColor(15,74,133,int(0.28*255)) argb=%1")
               .arg(QColor(15, 74, 133, int(0.28 * 255)).name(QColor::HexArgb));

    // 7. Qt QSS co hieu rgba() alpha thap phan? Do bang pixel, khong doan.
    appendSection(out, QStringLiteral("qss rgba() resolution test (nen trang #FFFFFF)"));
    out << qssRgbaProbe(QStringLiteral("rgba(15,74,133,0.28)"));
    out << qssRgbaProbe(QStringLiteral("rgba(15,74,133,71)"));
    out << qssRgbaProbe(QStringLiteral("rgba(15,74,133,28%)"));
    out << qssRgbaProbe(QStringLiteral("#0F4A85"));
    out << QStringLiteral("ky vong neu alpha 0.28 duoc hieu: tron 28 phan tram #0F4A85 tren trang = %1")
               .arg(QColor(int(15 * 0.28 + 255 * 0.72),
                           int(74 * 0.28 + 255 * 0.72),
                           int(133 * 0.28 + 255 * 0.72)).name());

    out << QString();
    return out.join(QLatin1Char('\n'));
}

bool UiProbe::snapshot(QWidget* root, const QString& pngPath, const QString& txtPath,
                       const ThemeTokens& t, bool dark,
                       QString* dumpOut, QString* errOut) {
    auto fail = [errOut](const QString& why) {
        if (errOut) *errOut = why;
        return false;
    };
    if (!root) return fail(QStringLiteral("root is null"));

    const QString dir = QFileInfo(pngPath).absolutePath();
    if (!dir.isEmpty() && !QDir().mkpath(dir))
        return fail(QStringLiteral("cannot create dir ") + dir);

    const QPixmap pm = root->grab();
    if (pm.isNull()) return fail(QStringLiteral("grab returned null"));
    const QImage img = pm.toImage();
    if (img.isNull()) return fail(QStringLiteral("grab image is null"));
    if (!img.save(pngPath, "PNG")) return fail(QStringLiteral("cannot save ") + pngPath);

    // Dem so mau khac nhau: anh TRANG TRON thi con so nay rat nho (bai hoc 08-14).
    QSet<QRgb> colors;
    for (int y = 0; y < img.height(); ++y) {
        for (int x = 0; x < img.width(); ++x) colors.insert(img.pixel(x, y));
        if (colors.size() > 5000) break;   // du de ket luan, khong quet tiep cho nhanh
    }

    QString dump = QStringLiteral("png=%1\ntxt=%2\nsize=%3x%4\ndistinctColors=%5\n")
                       .arg(pngPath, txtPath)
                       .arg(img.width()).arg(img.height())
                       .arg(colors.size());
    dump += dumpWidgets(root, t, dark);

    QFile f(txtPath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate))
        return fail(QStringLiteral("cannot write ") + txtPath);
    {
        QTextStream ts(&f);
        ts << dump;
    }
    f.close();

    if (dumpOut) *dumpOut = dump;
    return true;
}
