// KHONG duoc build tu 2026-08-14: OCR da dong kem, bo co che tai roi. Giu lai phong khi lam goi ngon ngu.
#pragma once
#include <QString>

// Giai nen goi OCR (.tar.gz) bang zlib gzip + bo doc tar ustar tu viet.
// KHONG dung QZipReader (Qt 6.10 Debian tat QT_FEATURE_zip) va KHONG them
// thu vien ngoai (zlib da co san).
class TarGzReader {
public:
    // Giai nen archivePath (.tar.gz) vao destDir (da ton tai).
    // TU CHOI ten khong an toan (chua "..", bat dau bang / hoac \,
    // chua ":" hoac ky tu dieu khien) -> tra false, khong ghi ra ngoai destDir.
    static bool extract(const QString& archivePath, const QString& destDir, QString* err);
};
