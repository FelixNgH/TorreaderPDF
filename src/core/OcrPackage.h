// KHONG duoc build tu 2026-08-14: OCR da dong kem, bo co che tai roi. Giu lai phong khi lam goi ngon ngu.
#pragma once
#include <QString>

// Dinh vi va nhan dien goi OCR (KHONG mang, KHONG OCR o buoc nay).
class OcrPackage {
public:
    // JSON mo ta ban OCR moi nhat. Tam thoi tro toi Releases cua repo app.
    static constexpr const char* kOcrReleaseInfoUrl =
        "https://github.com/felixngh/torreader/releases/download/ocr-latest/ocr-release.json";

    struct Info {
        bool     installed = false;
        QString  name;              // ten goi doc tu manifest.json
        QString  root;              // thu muc goc cua goi (duong dan THAT, luon co gia tri)
        bool     rootWritable = false;
        bool     inCloudFolder = false;   // OneDrive/Dropbox/Google Drive/iCloud
        QString  version;           // doc tu manifest.json, rong neu chua cai
        qint64   sizeOnDisk = 0;
        QString  problem;           // mo ta loi neu goi hong, rong neu binh thuong
    };
    static Info detect();                    // khong bao gio nem exception
    static QString preferredRoot(bool* writable);  // canh file chay -> lui ve AppDataLocation
    static bool installFromArchive(const QString& zipPath, QString* err);  // giai nen + kiem manifest
};
