#pragma once

#include <QString>
#include <QList>
#include <QByteArray>
#include <fpdfview.h>
#include <fpdf_signature.h>

struct SignatureInfo {
    QString signerName;
    QString reason;
    QString signingTime;
    bool cryptoValid = false;
    bool chainTrusted = false;
    QString errorMsg;
};

struct SignParams {
    QString pfxPath;
    QString password;
    QString reason;
    QString location;
};

class PdfSigner {
public:
    static QList<SignatureInfo> readAndVerify(FPDF_DOCUMENT doc, const QString& filePath);
    static bool verifyCms(const QByteArray& pkcs7Der, const QByteArray& signedBytes, SignatureInfo& info);
    static bool signDocument(const QString& inputPath, const QString& outputPath,
                             const SignParams& params, QString& errorMsg);
};
