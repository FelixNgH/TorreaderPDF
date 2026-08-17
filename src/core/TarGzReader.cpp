// KHONG duoc build tu 2026-08-14: OCR da dong kem, bo co che tai roi. Giu lai phong khi lam goi ngon ngu.
#include "TarGzReader.h"
#include <QFile>
#include <QDir>
#include <zlib.h>
#include <cstring>
#include <algorithm>

namespace {

// Kiem ten an toan: bo tien to "./" thuong co trong tar, roi tu choi
// "..", bat dau bang / hoac \, chua ":" hoac ky tu dieu khien. Mot file
// tar doc hai co the ghi de bat ky dau tren may.
static bool safeName(const QByteArray& raw, QString* out) {
    QByteArray n = raw;
    while (n.startsWith("./")) n = n.mid(2);
    while (n.startsWith(".\\")) n = n.mid(2);
    if (n.isEmpty()) return false;
    if (n.contains("..")) return false;
    if (n.startsWith('/') || n.startsWith('\\')) return false;
    if (n.contains(':')) return false;
    for (char c : n)
        if (static_cast<unsigned char>(c) < 0x20) return false;
    *out = QString::fromUtf8(n);
    return true;
}

// Doc kich thuoc bat phan tu header[124..135] (ket thuc o NUL/space).
static qint64 octalSize(const char* p, int len) {
    qint64 v = 0;
    for (int i = 0; i < len; ++i) {
        const char c = p[i];
        if (c < '0' || c > '7') break;
        v = v * 8 + (c - '0');
    }
    return v;
}

struct TarState {
    QFile* out = nullptr;    // file dang ghi
    qint64 dataLeft = 0;     // du lieu file con lai
    qint64 padLeft = 0;      // pad toi boi so 512 con lai
    char header[512];
    int headerGot = 0;
    bool eof = false;
};

// Xu ly mot header 512 byte. Mo file / tao thu muc cho muc hien tai.
static bool processHeader(TarState& st, const QString& destDir, QString* err) {
    bool allZero = true;
    for (char c : st.header) {
        if (c != 0) { allZero = false; break; }
    }
    if (allZero) { st.eof = true; return true; }

    QByteArray nameField(st.header, 100);
    QByteArray prefix(st.header + 345, 155);
    // Cat ky tu NUL / khoang trang cuoi CUA TUNG truong rieng le — neu noi prefix
    // (day NUL o muc ten ngan) truoc khi cat, NUL nam giua se vuong vao kiem
    // ky tu dieu khien ben duoi.
    while (!nameField.isEmpty() && (nameField.endsWith('\0') || nameField.endsWith(' '))) nameField.chop(1);
    while (!prefix.isEmpty() && (prefix.endsWith('\0') || prefix.endsWith(' '))) prefix.chop(1);
    QByteArray full = prefix;
    if (!full.isEmpty()) full.append('/');
    full.append(nameField);
    if (full.isEmpty()) return true;

    QString name;
    if (!safeName(full, &name)) {
        if (err) *err = QStringLiteral("Unsafe path in archive: ") + QString::fromUtf8(full);
        return false;
    }

    const qint64 size = octalSize(st.header + 124, 12);
    const char type = st.header[156];
    const qint64 padded = (size + 511) / 512 * 512;

    if (st.out) { st.out->close(); delete st.out; st.out = nullptr; }

    if (type == '5') {
        if (!QDir().mkpath(destDir + QLatin1Char('/') + name)) {
            if (err) *err = QStringLiteral("Cannot create directory: ") + name;
            return false;
        }
        st.dataLeft = st.padLeft = 0;
        return true;
    }

    if (type != '0' && type != '\0') {
        // Cac loai khac (link, device, ...): bo qua du lieu
        st.dataLeft = 0;
        st.padLeft = padded;
        return true;
    }

    const QString path = destDir + QLatin1Char('/') + name;
    const int slash = path.lastIndexOf(QLatin1Char('/'));
    if (slash > 0 && !QDir().mkpath(path.left(slash))) {
        if (err) *err = QStringLiteral("Cannot create folder for: ") + name;
        return false;
    }
    auto* f = new QFile(path);
    if (!f->open(QIODevice::WriteOnly)) {
        delete f;
        if (err) *err = QStringLiteral("Cannot write file: ") + name;
        return false;
    }
    st.out = f;
    st.dataLeft = size;
    st.padLeft = padded - size;
    return true;
}

// Nap du lieu da giai nen vao parser tar.
static bool feedTar(TarState& st, const char* data, qint64 len, const QString& destDir, QString* err) {
    qint64 pos = 0;
    while (pos < len && !st.eof) {
        if (st.dataLeft > 0) {
            const qint64 n = qMin(st.dataLeft, len - pos);
            if (st.out && st.out->write(data + pos, n) != n) {
                if (err) *err = QStringLiteral("Write failed");
                return false;
            }
            pos += n;
            st.dataLeft -= n;
            if (st.dataLeft == 0 && st.out) {
                st.out->close();
                delete st.out;
                st.out = nullptr;
            }
            continue;
        }
        if (st.padLeft > 0) {
            const qint64 n = qMin(st.padLeft, len - pos);
            pos += n;
            st.padLeft -= n;
            continue;
        }
        if (st.headerGot < 512) {
            const int need = 512 - st.headerGot;
            const int n = static_cast<int>(qMin<qint64>(need, len - pos));
            memcpy(st.header + st.headerGot, data + pos, n);
            st.headerGot += n;
            pos += n;
            if (st.headerGot == 512) {
                st.headerGot = 0;
                if (!processHeader(st, destDir, err)) return false;
            }
            continue;
        }
        break;
    }
    return true;
}

} // namespace

bool TarGzReader::extract(const QString& archivePath, const QString& destDir, QString* err) {
    QFile gz(archivePath);
    if (!gz.open(QIODevice::ReadOnly)) {
        if (err) *err = QStringLiteral("Cannot open archive: ") + archivePath;
        return false;
    }
    z_stream z;
    memset(&z, 0, sizeof(z));
    if (inflateInit2(&z, 15 + 16) != Z_OK) {   // 15 + 16 = gzip
        if (err) *err = QStringLiteral("zlib init failed");
        return false;
    }
    TarState st;
    char in[131072];
    char out[65536];
    int ret = Z_OK;
    bool ok = true;
    while (ret != Z_STREAM_END) {
        const qint64 inN = gz.read(in, qint64(sizeof(in)));
        if (inN < 0) {
            ok = false;
            if (err) *err = QStringLiteral("Read error");
            break;
        }
        z.next_in = reinterpret_cast<Bytef*>(in);
        z.avail_in = static_cast<uInt>(inN);
        do {
            z.next_out = reinterpret_cast<Bytef*>(out);
            z.avail_out = static_cast<uInt>(sizeof(out));
            ret = inflate(&z, Z_NO_FLUSH);
            if (ret != Z_OK && ret != Z_STREAM_END) {
                ok = false;
                if (err) *err = QStringLiteral("Corrupt gzip data");
                break;
            }
            const qint64 produced = qint64(sizeof(out)) - qint64(z.avail_out);
            if (produced > 0 && !feedTar(st, out, produced, destDir, err)) {
                ok = false;
                break;
            }
        } while (ret != Z_STREAM_END && z.avail_out == 0);
        if (!ok) break;
        if (ret != Z_STREAM_END && inN == 0) {   // file het ma stream chua ket thuc
            ok = false;
            if (err) *err = QStringLiteral("Truncated archive");
            break;
        }
    }
    if (st.out) { st.out->close(); delete st.out; st.out = nullptr; }
    inflateEnd(&z);
    return ok;
}
