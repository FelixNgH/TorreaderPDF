// KHONG duoc build tu 2026-08-14: OCR da dong kem, bo co che tai roi. Giu lai phong khi lam goi ngon ngu.
#include "OcrPackage.h"
#include "TarGzReader.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QSettings>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonParseError>

// Ghi thu mot file tam roi xoa de do writable. KHONG tin QFileInfo::isWritable()
// vi tren Windows no noi doi.
static bool testWritable(const QString& dir) {
    QDir().mkpath(dir);
    if (!QDir(dir).exists()) return false;
    const QString probe = dir + QLatin1String("/.writetest");
    QFile f(probe);
    if (!f.open(QIODevice::WriteOnly)) return false;
    f.write("x", 1);
    f.close();
    return QFile::remove(probe);
}

static bool isCloudFolder(const QString& path) {
    const QString p = path.toLower();
    return p.contains("onedrive") || p.contains("dropbox")
        || p.contains("google drive") || p.contains("icloud");
}

QString OcrPackage::preferredRoot(bool* writable) {
    // Nguoi dung co the chon thu muc khac (noi dung giao dien) -> uu tien override
    const QString override = QSettings().value(QLatin1String("ocr/root")).toString();
    if (!override.isEmpty()) {
        if (writable) *writable = testWritable(override);
        return override;
    }
    QString root = QCoreApplication::applicationDirPath() + QLatin1String("/ocr");
    if (testWritable(root)) {
        if (writable) *writable = true;
        return root;
    }
    root = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
         + QLatin1String("/ocr");
    if (writable) *writable = testWritable(root);
    return root;
}

// Kiem manifest.json + tung file (ton tai + dung dung luong). CHUA kiem sha256
// (cham, de buoc 2 lam luc tai). Tra false khi manifest khong hop le.
static bool validateManifest(const QString& root, QString* name, QString* version,
                             qint64* sizeOnDisk, QString* problem) {
    QFile mf(root + QLatin1String("/manifest.json"));
    if (!mf.open(QIODevice::ReadOnly)) {
        if (problem) *problem = QStringLiteral("manifest.json is missing or unreadable");
        return false;
    }
    QJsonParseError parseErr;
    const QJsonDocument doc = QJsonDocument::fromJson(mf.readAll(), &parseErr);
    if (parseErr.error != QJsonParseError::NoError || !doc.isObject()) {
        if (problem) *problem = QStringLiteral("manifest.json is not valid JSON");
        return false;
    }
    const QJsonObject obj = doc.object();
    const QString nm   = obj.value(QLatin1String("name")).toString();
    const QString ver  = obj.value(QLatin1String("version")).toString();
    const QJsonArray files = obj.value(QLatin1String("files")).toArray();
    if (nm.isEmpty() || ver.isEmpty() || files.isEmpty()) {
        if (problem) *problem = QStringLiteral("manifest.json is missing name/version/files");
        return false;
    }
    qint64 total = 0;
    for (const QJsonValue& v : files) {
        const QJsonObject f = v.toObject();
        const QString path = f.value(QLatin1String("path")).toString();
        const qint64 bytes = static_cast<qint64>(f.value(QLatin1String("bytes")).toDouble());
        if (path.isEmpty() || path.contains(QLatin1String("..")) || path.startsWith(QLatin1Char('/'))) {
            if (problem) *problem = QStringLiteral("manifest.json: bad file path");
            return false;
        }
        const QFileInfo fi(root + QLatin1Char('/') + path);
        if (!fi.exists()) {
            if (problem) *problem = QStringLiteral("file missing: ") + path;
            return false;
        }
        if (fi.size() != bytes) {
            if (problem) *problem = QStringLiteral("size mismatch: %1 (on disk %2, manifest %3)")
                                        .arg(path).arg(fi.size()).arg(bytes);
            return false;
        }
        total += fi.size();
    }
    if (name) *name = nm;
    if (version) *version = ver;
    if (sizeOnDisk) *sizeOnDisk = total;
    return true;
}

OcrPackage::Info OcrPackage::detect() {
    Info info;
    info.root = preferredRoot(&info.rootWritable);
    info.inCloudFolder = isCloudFolder(info.root);

    if (!QFileInfo::exists(info.root + QLatin1String("/manifest.json"))) {
        // Chua cai: khong co loi gi, chi don gian la chua co goi
        info.problem.clear();
        return info;
    }
    QString name, version, problem;
    qint64 size = 0;
    if (validateManifest(info.root, &name, &version, &size, &problem)) {
        info.installed = true;
        info.name = name;
        info.version = version;
        info.sizeOnDisk = size;
    } else {
        info.problem = problem;
    }
    return info;
}

bool OcrPackage::installFromArchive(const QString& archivePath, QString* err) {
    if (!QFileInfo::exists(archivePath)) {
        if (err) *err = QStringLiteral("Archive not found: ") + archivePath;
        return false;
    }
    const QString root = preferredRoot(nullptr);
    if (root.isEmpty()) {
        if (err) *err = QStringLiteral("Cannot determine package folder.");
        return false;
    }
    // Giai nen vao staging CANH root (root.staging), kiem manifest, xong moi
    // doi ten vao cho that. Goi cu van nguyen ven neu giai nen/kiem loi.
    const QString staging = root + QLatin1String(".staging");
    QDir sdir(staging);
    if (sdir.exists() && !sdir.removeRecursively()) {
        if (err) *err = QStringLiteral("Cannot clean staging folder: ") + staging;
        return false;
    }
    if (!QDir().mkpath(staging)) {
        if (err) *err = QStringLiteral("Cannot create staging folder: ") + staging;
        return false;
    }
    QString extractErr;
    if (!TarGzReader::extract(archivePath, staging, &extractErr)) {
        sdir.removeRecursively();
        if (err) *err = QStringLiteral("Extract failed: ") + extractErr;
        return false;
    }
    QString name, version, problem;
    qint64 size = 0;
    if (!validateManifest(staging, &name, &version, &size, &problem)) {
        sdir.removeRecursively();
        if (err) *err = problem;
        return false;
    }
    // Huy goi cu, doi staging vao cho that
    QDir rdir(root);
    if (rdir.exists() && !rdir.removeRecursively()) {
        sdir.removeRecursively();
        if (err) *err = QStringLiteral("Cannot remove old package folder.");
        return false;
    }
    if (!QDir().rename(staging, root)) {
        if (sdir.exists()) sdir.removeRecursively();
        if (err) *err = QStringLiteral("Cannot move staged package into place.");
        return false;
    }
    return true;
}
