#pragma once

#include <QString>
#include <QList>
#include <QByteArray>
#include <QRectF>
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
    int     pageIndex = -1;
    QRectF  rectPt;
    int     textR = 0, textG = 0, textB = 0;
    double  stampFont = 0.0;
    bool    fillBg = false;
    int     fillR = 255, fillG = 245, fillB = 200;
};

class PdfSigner {
public:
    static QList<SignatureInfo> readAndVerify(FPDF_DOCUMENT doc, const QString& filePath);
    static bool verifyCms(const QByteArray& pkcs7Der, const QByteArray& signedBytes, SignatureInfo& info);
    static bool signDocument(const QString& inputPath, const QString& outputPath,
                             const SignParams& params, QString& errorMsg);
};
