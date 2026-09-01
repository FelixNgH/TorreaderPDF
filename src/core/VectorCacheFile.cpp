#include "VectorCacheFile.h"
#include "VectorLayer.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStorageInfo>
#include <QElapsedTimer>
#include <QDebug>
#include <algorithm>

namespace VectorCache {

QString pathFor(const QString& pdfPath, quint64 pdfHash, int pageIndex) {
    // Chot an toan: thieu khoa thi khong co duong dan hop le — khong duoc de
    // bat ky luong nao sinh ten file cache khong co dinh danh (dung sai du lieu).
    if (pdfPath.isEmpty() || pdfHash == 0) return QString();
    return QDir::temp().filePath(
        QFileInfo(pdfPath).fileName() + "_" +
        QString::number(pdfHash, 16) + "_p" + QString::number(pageIndex) + ".torvec");
}

bool tryLoad(VectorLayer& out, const QString& pdfPath, quint64 pdfHash, int pageIndex) {
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
    QFile::remove(p);   // QFile::rename khong ghi de tren Windows
    if (!QFile::rename(tmp, p)) {
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

} // namespace VectorCache
