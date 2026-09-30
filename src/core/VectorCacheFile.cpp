#include "VectorCacheFile.h"
#include "VectorLayer.h"
#include "Bisect.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QSet>
#include <QStorageInfo>
#include <QElapsedTimer>
#include <QDebug>
#include <algorithm>

namespace VectorCache {

// ── 0927 LƯỢT 6: khoa ban ghi ────────────────────────────────────────────────
// key dang bi CAM: tai lieu vua rut cache, task ghi .torvec chua chay xong se bo
// file .tmp cua minh thay vi ghi lai. Khoa bao ca ca viec "cam" voi khoanh
// `QFile::rename(.tmp -> .torvec)` trong trySave ⇒ KHONG co cua so trong do task
// ghi tao lai file sau khi da xoa.
namespace {
QMutex   g_banMu;
QSet<QString> g_banned;
} // namespace

QString keyFor(const QString& pdfPath, quint64 pdfHash) {
    // Chot an toan: thieu khoa thi khong co duong dan hop le — khong duoc de
    // bat ky luong nao sinh ten file cache khong co dinh danh (dung sai du lieu).
    if (pdfPath.isEmpty() || pdfHash == 0) return QString();
    return QFileInfo(pdfPath).fileName() + "_" + QString::number(pdfHash, 16);
}

QString keyFromCachePath(const QString& torcachePath) {
    const QString f = QFileInfo(torcachePath).fileName();
    const QString suf = QStringLiteral(".torcache");
    if (!f.endsWith(suf, Qt::CaseInsensitive)) return QString();
    return f.left(f.size() - suf.size());
}

QString keyFromVecFileName(const QString& fileName) {
    QString n = fileName;
    // `chop(suf.size())` chu KHONG dem tay: ".torvec.tmp" dai 11 ky tu, dem sai
    // 1 ky tu thi con sot duoi ten ⇒ key sai ⇒ don mo coi im lang bo qua het.
    const QString sufTmp = QLatin1String(".torvec.tmp");
    const QString sufVec = QLatin1String(".torvec");
    if      (n.endsWith(sufTmp, Qt::CaseInsensitive)) n.chop(sufTmp.size());
    else if (n.endsWith(sufVec, Qt::CaseInsensitive)) n.chop(sufVec.size());
    else return QString();
    // cat tiep "_p<so trang>" — phai la 'p' + CHU SO ("_p0abc" la ten rac, khong
    // phai ten file cua ta). Sai o buoc nay ⇒ don mo coi im lang bo qua het.
    const int us = n.lastIndexOf(QLatin1Char('_'));
    if (us <= 0 || us + 2 >= n.size()) return QString();
    if (n.at(us + 1) != QLatin1Char('p') && n.at(us + 1) != QLatin1Char('P'))
        return QString();
    for (int i = us + 2; i < n.size(); ++i)
        if (!n.at(i).isDigit()) return QString();
    return n.left(us);
}

QString pathFor(const QString& pdfPath, quint64 pdfHash, int pageIndex) {
    const QString key = keyFor(pdfPath, pdfHash);
    if (key.isEmpty()) return QString();
    return QDir::temp().filePath(key + "_p" + QString::number(pageIndex) + ".torvec");
}

bool tryLoad(VectorLayer& out, const QString& pdfPath, quint64 pdfHash, int pageIndex) {
    // 0927 LƯỢT 10 (--no-vector): KHONG doc .torvec — chi raster. `out` giu nguyen
    // rong ⇒ moi noi goi (MainWindow :2108/:4439/:5092/:6288, ContinuousView :1719/:1823)
    // rơi xuống `layer->build(...)` ⇒ ma do la chính VectorLayer::build cung tra false.
    if (trNoVector()) return false;
    if (pdfPath.isEmpty() || pdfHash == 0) {
        qDebug().noquote() << "[torvec] SKIP page=" << pageIndex << "reason=nokey";
        return false;
    }
    const QString p = pathFor(pdfPath, pdfHash, pageIndex);
    if (!QFile::exists(p)) {
        qDebug().noquote() << "[torvec] MISS page=" << pageIndex;
        return false;
    }
    QElapsedTimer t; t.start();
    QFile f(p);
    if (!f.open(QIODevice::ReadOnly)) {
        qDebug().noquote() << "[torvec] MISS page=" << pageIndex << "(ko mo file)";
        return false;
    }
    const qint64 bytes = f.size();
    bool ok = out.loadFrom(f);
    f.close();
    if (!ok) {
        // File hong / sai phien ban / lech page. Xoa de khong tai lai mai.
        qDebug().noquote() << "[torvec] MISS page=" << pageIndex << "(file hong, xoa)";
        QFile::remove(p);
        return false;
    }
    // Khoa key kiem so trang khop. Lech thi xoa file hong.
    if (out.pageIndex() != pageIndex) {
        qDebug().noquote() << "[torvec] MISS page=" << pageIndex
                           << "(sai page trong file " << out.pageIndex() << ", xoa)";
        QFile::remove(p);
        return false;
    }
    qDebug().noquote() << "[torvec] HIT page=" << pageIndex
                       << "ms=" << t.elapsed() << "bytes=" << bytes;
    return true;
}

bool trySave(const VectorLayer& layer, const QString& pdfPath, quint64 pdfHash, int pageIndex) {
    if (pdfPath.isEmpty() || pdfHash == 0) {
        qDebug().noquote() << "[torvec] SKIP page=" << pageIndex << "reason=nokey";
        return false;
    }
    const QString key = keyFor(pdfPath, pdfHash);
    // 0927 LƯỢT 6: cache cua tai lieu nay vua bi rut (tab dong / thoat app) ⇒
    // cong viec ton tai 300 MB nay vo dung. Bo luon, khong ghi lai de tao rac.
    {
        QMutexLocker lk(&g_banMu);
        if (g_banned.contains(key)) {
            qDebug().noquote() << "[torvec] SKIP page=" << pageIndex << "reason=camGhi(cacheDaRut)";
            return false;
        }
    }
    if (!layer.isReady() || !layer.isComplete()) {
        qDebug().noquote() << "[torvec] SKIP page=" << pageIndex << "reason=notcomplete";
        return false;
    }
    const qint64 approx = layer.approxBytes();
    if (approx > kMaxPerPageBytes) {
        qDebug().noquote() << "[torvec] SKIP page=" << pageIndex
                           << "reason=toobig bytes=" << approx;
        return false;
    }
    const qint64 avail = QStorageInfo(QDir::tempPath()).bytesAvailable();
    // Khong lay duoc (bytesAvailable() < 0) => coi nhu KHONG DU va tu choi ghi.
    if (avail < 0 || avail < kMinFreeBytes) {
        qDebug().noquote() << "[torvec] SKIP page=" << pageIndex
                           << "reason=lowdisk avail=" << avail;
        return false;
    }

    const QString p   = pathFor(pdfPath, pdfHash, pageIndex);
    const QString tmp = p + ".tmp";
    QElapsedTimer t; t.start();
    {
        QFile f(tmp);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            if (!layer.saveTo(f) || !f.flush()) {
                f.close();
                QFile::remove(tmp);
                qDebug().noquote() << "[torvec] SKIP page=" << pageIndex << "reason=io";
                return false;
            }
        } else {
            QFile::remove(tmp);
            qDebug().noquote() << "[torvec] SKIP page=" << pageIndex << "reason=io";
            return false;
        }
    }
    // Atomic rename: ghi file tam roi doi ten — khong bao gio de lai file cut.
    // 0927 LƯỢT 6: KIEM CAM + RENAME TRONG CUNG MOT LAN KHOA. Neu tach ra:
    // purge co the ban + xoa xong roi task ghi tao lai file (rac), hoac nguoc lai
    // task ghi vua rename xong roi purge xoa. Giu khoa thi chi con mot thu tu.
    bool renamed = false;
    {
        QMutexLocker lk(&g_banMu);
        if (g_banned.contains(key)) {
            QFile::remove(tmp);           // ban ghi tam cua tao, khong doi lai thanh .torvec
            qDebug().noquote() << QString("[torvec] SKIP page=%1 reason=camGhi(cacheDaRut) xoaTam=1")
                                     .arg(pageIndex);
            return false;
        }
        QFile::remove(p);   // QFile::rename khong ghi de tren Windows
        renamed = QFile::rename(tmp, p);
    }
    if (!renamed) {
        QFile::remove(tmp);
        qDebug().noquote() << "[torvec] SKIP page=" << pageIndex
                           << "reason=io(rename) cache-lost";
        return false;
    }
    qDebug().noquote() << "[torvec] SAVE page=" << pageIndex
                       << "ms=" << t.elapsed() << "bytes=" << approx;
    enforceBudget();
    return true;
}

void enforceBudget() {
    const QDir dir(QDir::tempPath());
    const QFileInfoList fi = dir.entryInfoList(QStringList() << "*.torvec",
                                               QDir::Files | QDir::Readable, QDir::Name);
    // Sap theo lastModified tang dan (cu nhat truoc) — xoa tu cu nhat.
    QList<QFileInfo> sorted = fi;
    std::sort(sorted.begin(), sorted.end(),
              [](const QFileInfo& a, const QFileInfo& b) {
                  return a.lastModified() < b.lastModified();
              });
    qint64 total = 0;
    for (const QFileInfo& e : sorted) total += e.size();
    for (const QFileInfo& e : sorted) {
        if (total <= kMaxTotalBytes) break;
        const QString p = e.absoluteFilePath();
        qint64 sz = e.size();
        if (QFile::remove(p)) {
            total -= sz;
            qDebug().noquote() << "[torvec] EVICT" << QFileInfo(p).fileName()
                               << "bytes=" << sz;
        }
    }
}

void banKey(const QString& key) {
    if (key.isEmpty()) return;
    QMutexLocker lk(&g_banMu);
    g_banned.insert(key);
}

void unbanKey(const QString& key) {
    if (key.isEmpty()) return;
    QMutexLocker lk(&g_banMu);
    g_banned.remove(key);
}

int purgeKey(const QString& key, const char* reason) {
    if (key.isEmpty()) return 0;
    // CAM TRUOC khi xoa: task ghi .torvec chua chay xong (hoac chay sau lan rut
    // cache) se bo file .tmp cua no thay vi ghi lai file da bi xoa.
    banKey(key);
    const QDir dir(QDir::tempPath());
    // 2 mau: file .torvec va ban ghi tam cua task CHUA xong (.torvec.tmp).
    const QStringList names = dir.entryList({key + "_p*.torvec", key + "_p*.torvec.tmp"},
                                           QDir::Files);
    int okN = 0, errN = 0;
    for (const QString& name : names) {
        const QString p = dir.absoluteFilePath(name);
        QFile f(p);
        const bool ok = f.remove();
        if (ok) ++okN; else ++errN;
        qDebug().noquote() << QString("[torvec] XOA ok=%1 %2 ly do=%3 loi=%4")
                                 .arg(ok ? 1 : 0).arg(p)
                                 .arg(QString::fromLatin1(reason ? reason : "?"))
                                 .arg(ok ? QString() : f.errorString());
    }
    qDebug().noquote() << QString("[torvec] XOA tong=%1 loi=%2 key=%3 ly do=%4")
                             .arg(okN).arg(errN).arg(key)
                             .arg(QString::fromLatin1(reason ? reason : "?"));
    return okN;
}

int countForKey(const QString& key) {
    if (key.isEmpty()) return 0;
    const QDir dir(QDir::tempPath());
    // Moi thu ma mot file cua tai lieu do co the con lai tren dia: `.torcache`,
    // `.lock` banh sat no, `.torvec`, va ban ghi tam `.torvec.tmp`.
    // (`.torcache` khong ghi qua `.tmp` nen khong co mau `.torcache.tmp`.)
    return dir.entryList({key + ".torcache", key + ".lock",
                          key + "_p*.torvec", key + "_p*.torvec.tmp"}, QDir::Files).size();
}

} // namespace VectorCache
