#include "PdfSigner.h"
#include "PdfDocument.h"

#include <QFile>
#include <QSaveFile>
#include <QDebug>
#include <QDateTime>
#include <QMutexLocker>

#include <fpdfview.h>
#include <fpdf_signature.h>

#include <qpdf/QPDF.hh>
#include <qpdf/QPDFWriter.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>

#include <openssl/cms.h>
#include <openssl/x509.h>
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/pkcs12.h>
#include <openssl/evp.h>

#include <vector>
#include <memory>
#include <functional>

// ── OpenSSL RAII helpers ─────────────────────────────────────────────────────

template<typename T, auto FreeFn>
struct SslObj {
    T* p = nullptr;
    SslObj() = default;
    explicit SslObj(T* ptr) : p(ptr) {}
    ~SslObj() { if (p) FreeFn(p); }
    SslObj(const SslObj&) = delete;
    SslObj& operator=(const SslObj&) = delete;
    T** operator&() { return &p; }
    operator T*() const { return p; }
    T* operator->() const { return p; }
    T* release() { T* r = p; p = nullptr; return r; }
};

using Pkcs12Ptr   = SslObj<PKCS12, PKCS12_free>;
using EvpPkeyPtr  = SslObj<EVP_PKEY, EVP_PKEY_free>;
using X509Ptr     = SslObj<X509, X509_free>;
using BioPtr      = SslObj<BIO, BIO_free>;
using CmsPtr      = SslObj<CMS_ContentInfo, CMS_ContentInfo_free>;

struct StackX509Free {
    void operator()(STACK_OF(X509)* p) const {
        if (p) sk_X509_pop_free(p, X509_free);
    }
};
using StackX509Ptr = std::unique_ptr<STACK_OF(X509), StackX509Free>;

// ── PDFium load helper (file → memory → FPDF_LoadCustomDocument) ────────────

static int getBlockCallback(void* param, unsigned long position,
                            unsigned char* pBuf, unsigned long size) {
    auto* ba = static_cast<QByteArray*>(param);
    if (position + size > static_cast<unsigned long>(ba->size())) return 0;
    memcpy(pBuf, ba->constData() + position, size);
    return 1;
}

// Returns true if file has ≥1 signature. On error, errorMsg is set and returns false.
static bool checkExistingSignatures(const QString& path, QString& errorMsg)
{
    QMutexLocker lock(&PdfDocument::pdfiumGlobalMutex());

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        errorMsg = QStringLiteral("Cannot open file \"%1\" for reading").arg(path);
        return true; // treat as "has sig" to prevent signing on an unreadable file
    }
    QByteArray data = file.readAll();
    file.close();

    FPDF_FILEACCESS fa = {};
    fa.m_FileLen = static_cast<unsigned long>(data.size());
    fa.m_GetBlock = getBlockCallback;
    fa.m_Param = &data;

    FPDF_DOCUMENT doc = FPDF_LoadCustomDocument(&fa, nullptr);
    if (!doc) {
        unsigned long err = FPDF_GetLastError();
        errorMsg = QStringLiteral("Cannot load PDF for signature check (PDFium error %1)").arg(err);
        return true;
    }

    int sigCount = FPDF_GetSignatureCount(doc);
    FPDF_CloseDocument(doc);

    if (sigCount > 0) {
        errorMsg = QStringLiteral("File already contains a signature — signing again requires incremental update (not yet supported)");
        return true;
    }
    return false;
}

// ── Extract CN from a certificate — used for validation feedback ────────────

QString certCommonName(X509* cert)
{
    if (!cert) return {};
    X509_NAME* subj = X509_get_subject_name(cert);
    if (!subj) return {};
    char cn[256] = {};
    if (X509_NAME_get_text_by_NID(subj, NID_commonName, cn, sizeof(cn)) > 0)
        return QString::fromUtf8(cn);
    return {};
}

// ── readAndVerify ────────────────────────────────────────────────────────────

QList<SignatureInfo> PdfSigner::readAndVerify(FPDF_DOCUMENT doc, const QString& filePath)
{
    QList<SignatureInfo> result;

    int sigCount = FPDF_GetSignatureCount(doc);
    if (sigCount <= 0)
        return result;

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        SignatureInfo errInfo;
        errInfo.errorMsg = QStringLiteral("Cannot open file: %1").arg(filePath);
        result.append(errInfo);
        return result;
    }
    QByteArray fileData = file.readAll();
    file.close();

    for (int i = 0; i < sigCount; ++i) {
        SignatureInfo info;

        FPDF_SIGNATURE sig = FPDF_GetSignatureObject(doc, i);
        if (!sig) {
            info.errorMsg = QStringLiteral("FPDF_GetSignatureObject(%1) returned NULL").arg(i);
            result.append(info);
            continue;
        }

        // PKCS7 DER contents (two-call pattern)
        unsigned long need = FPDFSignatureObj_GetContents(sig, nullptr, 0);
        QByteArray pkcs7Der;
        if (need > 0 && need < 1024 * 1024) {
            pkcs7Der.resize(static_cast<int>(need));
            unsigned long written = FPDFSignatureObj_GetContents(sig, pkcs7Der.data(), need);
            if (written != need) {
                pkcs7Der.resize(static_cast<int>(written));
            }
        }

        // ByteRange: array of [offset, length] pairs (typically 2 pairs = 4 ints)
        unsigned long rangeInts = FPDFSignatureObj_GetByteRange(sig, nullptr, 0);
        int byteRange[4] = {};
        if (rangeInts >= 4) {
            FPDFSignatureObj_GetByteRange(sig, byteRange, 4);
        }

        // Reason (UTF-16LE)
        unsigned long reasonBytes = FPDFSignatureObj_GetReason(sig, nullptr, 0);
        if (reasonBytes > 0 && reasonBytes < 65536) {
            QByteArray buf(static_cast<int>(reasonBytes), '\0');
            FPDFSignatureObj_GetReason(sig, buf.data(), reasonBytes);
            int chars = static_cast<int>(reasonBytes / 2);
            info.reason = QString::fromUtf16(reinterpret_cast<const char16_t*>(buf.constData()), chars).trimmed();
        }

        // Time (ASCII, format D:YYYYMMDDHHMMSS+XX'YY')
        unsigned long timeBytes = FPDFSignatureObj_GetTime(sig, nullptr, 0);
        if (timeBytes > 0 && timeBytes < 256) {
            QByteArray buf(static_cast<int>(timeBytes), '\0');
            FPDFSignatureObj_GetTime(sig, buf.data(), timeBytes);
            info.signingTime = QString::fromLatin1(buf).trimmed();
        }

        // Build signed data from ByteRange segments
        QByteArray signedBytes;
        if (rangeInts >= 4) {
            qint64 o1 = byteRange[0];
            qint64 l1 = byteRange[1];
            qint64 o2 = byteRange[2];
            qint64 l2 = byteRange[3];
            if (o1 >= 0 && l1 > 0 && o1 + l1 <= fileData.size() &&
                o2 >= 0 && l2 > 0 && o2 + l2 <= fileData.size()) {
                signedBytes.append(fileData.constData() + o1, static_cast<int>(l1));
                signedBytes.append(fileData.constData() + o2, static_cast<int>(l2));
            }
        }

        if (!pkcs7Der.isEmpty() && !signedBytes.isEmpty()) {
            verifyCms(pkcs7Der, signedBytes, info);
        } else {
            info.errorMsg = pkcs7Der.isEmpty()
                ? QStringLiteral("Empty PKCS7 content")
                : QStringLiteral("Invalid byte range");
        }

        result.append(info);
    }

    return result;
}

// ── verifyCms ────────────────────────────────────────────────────────────────

bool PdfSigner::verifyCms(const QByteArray& pkcs7Der, const QByteArray& signedBytes, SignatureInfo& info)
{
    info.cryptoValid = false;
    info.chainTrusted = false;

    BioPtr cmsBio(BIO_new_mem_buf(pkcs7Der.constData(), static_cast<int>(pkcs7Der.size())));
    if (!cmsBio) {
        info.errorMsg = QStringLiteral("BIO_new_mem_buf failed for PKCS7 DER");
        return false;
    }

    CmsPtr cms(d2i_CMS_bio(cmsBio, nullptr));
    if (!cms) {
        unsigned long err = ERR_get_error();
        char buf[256];
        ERR_error_string_n(err, buf, sizeof(buf));
        info.errorMsg = QStringLiteral("d2i_CMS_bio failed: %1").arg(QString::fromLatin1(buf));
        return false;
    }

    BioPtr dataBio(BIO_new_mem_buf(signedBytes.constData(), static_cast<int>(signedBytes.size())));
    if (!dataBio) {
        info.errorMsg = QStringLiteral("BIO_new_mem_buf failed for signed data");
        return false;
    }

    int rc = CMS_verify(cms, nullptr, nullptr, dataBio, nullptr,
                        CMS_NO_SIGNER_CERT_VERIFY | CMS_BINARY);
    info.cryptoValid = (rc == 1);
    info.chainTrusted = false;

    if (rc == 1) {
        STACK_OF(X509)* signers = CMS_get0_signers(cms);
        if (signers && sk_X509_num(signers) > 0) {
            info.signerName = certCommonName(sk_X509_value(signers, 0));
        }
    } else {
        unsigned long err = ERR_get_error();
        char buf[256];
        ERR_error_string_n(err, buf, sizeof(buf));
        info.errorMsg = QStringLiteral("CMS_verify failed: %1").arg(QString::fromLatin1(buf));

        StackX509Ptr certs(CMS_get1_certs(cms));
        if (certs && sk_X509_num(certs.get()) > 0) {
            info.signerName = certCommonName(sk_X509_value(certs.get(), 0));
        }
    }

    return info.cryptoValid;
}

// ── signDocument ─────────────────────────────────────────────────────────────

bool PdfSigner::signDocument(const QString& inputPath, const QString& outputPath,
                              const SignParams& params, QString& errorMsg)
{
    // ════════════════════════════════════════════════════════════════════════
    // Step 0 — reject multi-sig
    // ════════════════════════════════════════════════════════════════════════
    if (checkExistingSignatures(inputPath, errorMsg))
        return false;

    // ════════════════════════════════════════════════════════════════════════
    // Step 1 — load PFX via OpenSSL
    // ════════════════════════════════════════════════════════════════════════
    QFile pfxFile(params.pfxPath);
    if (!pfxFile.open(QIODevice::ReadOnly)) {
        errorMsg = QStringLiteral("Cannot open PFX file \"%1\"").arg(params.pfxPath);
        return false;
    }
    QByteArray pfxData = pfxFile.readAll();
    pfxFile.close();

    BioPtr pfxBio(BIO_new_mem_buf(pfxData.constData(), static_cast<int>(pfxData.size())));
    if (!pfxBio.p) {
        errorMsg = QStringLiteral("BIO_new_mem_buf failed for PFX data");
        return false;
    }

    Pkcs12Ptr p12(d2i_PKCS12_bio(pfxBio, nullptr));
    if (!p12.p) {
        unsigned long err = ERR_get_error();
        char buf[256];
        ERR_error_string_n(err, buf, sizeof(buf));
        errorMsg = QStringLiteral("Failed to decode PFX: %1").arg(buf);
        return false;
    }

    EvpPkeyPtr pkey;
    X509Ptr cert;
    StackX509Ptr chain;

    {
        X509* rawCert = nullptr;
        EVP_PKEY* rawKey = nullptr;
        STACK_OF(X509)* rawChain = nullptr;
        QByteArray pwdUtf8 = params.password.toUtf8();
        int rc = PKCS12_parse(p12, pwdUtf8.constData(), &rawKey, &rawCert, &rawChain);
        if (rc != 1) {
            unsigned long err = ERR_get_error();
            char buf[256];
            ERR_error_string_n(err, buf, sizeof(buf));
            errorMsg = QStringLiteral("PKCS12_parse failed (wrong password?): %1").arg(buf);
            return false;
        }
        pkey.p = rawKey;
        cert.p = rawCert;
        chain.reset(rawChain);
    }

    if (!cert.p || !pkey.p) {
        errorMsg = QStringLiteral("PFX does not contain a private key and certificate");
        return false;
    }

    // ════════════════════════════════════════════════════════════════════════
    // Step 2 — build PDF signature skeleton via QPDF
    // ════════════════════════════════════════════════════════════════════════
    QFile inFile(inputPath);
    if (!inFile.open(QIODevice::ReadOnly)) {
        errorMsg = QStringLiteral("Cannot open input PDF \"%1\"").arg(inputPath);
        return false;
    }
    QByteArray inData = inFile.readAll();
    inFile.close();

    QPDF pdf;
    try {
        pdf.processMemoryFile(inputPath.toUtf8().constData(),
                              inData.constData(), inData.size());
    } catch (const std::exception& e) {
        errorMsg = QStringLiteral("QPDF processMemoryFile failed: %1").arg(e.what());
        return false;
    }

    // Signing time — Vietnam TZ +07:00 (D:YYYYMMDDHHmmSS+07'00')
    QString mStr = QString("D:%1+07'00'")
        .arg(QDateTime::currentDateTime().toString("yyyyMMddHHmmss"));

    // Create sig dictionary
    QPDFObjectHandle sigDict = pdf.makeIndirectObject(QPDFObjectHandle::newDictionary());
    sigDict.replaceKey("/Type", QPDFObjectHandle::newName("/Sig"));
    sigDict.replaceKey("/Filter", QPDFObjectHandle::newName("/Adobe.PPKlite"));
    sigDict.replaceKey("/SubFilter", QPDFObjectHandle::newName("/adbe.pkcs7.detached"));
    sigDict.replaceKey("/M", QPDFObjectHandle::newString(mStr.toUtf8().constData()));

    if (!params.reason.isEmpty())
        sigDict.replaceKey("/Reason", QPDFObjectHandle::newUnicodeString(params.reason.toStdString()));
    if (!params.location.isEmpty())
        sigDict.replaceKey("/Location", QPDFObjectHandle::newUnicodeString(params.location.toStdString()));

    // /Contents placeholder: 16384 zero bytes → QPDF writes as hex <0000…>
    sigDict.replaceKey("/Contents", QPDFObjectHandle::newString(std::string(16384, '\0')));

    // /ByteRange placeholder: [0 9999999999 9999999999 9999999999] (49 chars)
    QPDFObjectHandle byteRange = QPDFObjectHandle::newArray();
    byteRange.appendItem(QPDFObjectHandle::newInteger(0));
    byteRange.appendItem(QPDFObjectHandle::newInteger(9999999999LL));
    byteRange.appendItem(QPDFObjectHandle::newInteger(9999999999LL));
    byteRange.appendItem(QPDFObjectHandle::newInteger(9999999999LL));
    sigDict.replaceKey("/ByteRange", byteRange);

    // Widget annotation (invisible)
    QPDFObjectHandle widget = pdf.makeIndirectObject(QPDFObjectHandle::newDictionary());
    widget.replaceKey("/Type", QPDFObjectHandle::newName("/Annot"));
    widget.replaceKey("/Subtype", QPDFObjectHandle::newName("/Widget"));
    widget.replaceKey("/FT", QPDFObjectHandle::newName("/Sig"));
    widget.replaceKey("/T", QPDFObjectHandle::newUnicodeString("TorReaderSig1"));
    {
        QPDFObjectHandle rect = QPDFObjectHandle::newArray();
        rect.appendItem(QPDFObjectHandle::newInteger(0));
        rect.appendItem(QPDFObjectHandle::newInteger(0));
        rect.appendItem(QPDFObjectHandle::newInteger(0));
        rect.appendItem(QPDFObjectHandle::newInteger(0));
        widget.replaceKey("/Rect", rect);
    }
    widget.replaceKey("/F", QPDFObjectHandle::newInteger(132));
    widget.replaceKey("/V", sigDict);

    // Attach widget to first page's /Annots
    auto pages = QPDFPageDocumentHelper(pdf).getAllPages();
    if (pages.empty()) {
        errorMsg = QStringLiteral("PDF has no pages");
        return false;
    }
    QPDFObjectHandle firstPage = pages[0].getObjectHandle();
    widget.replaceKey("/P", firstPage);
    if (firstPage.hasKey("/Annots")) {
        QPDFObjectHandle annots = firstPage.getKey("/Annots");
        annots.appendItem(widget);
    } else {
        QPDFObjectHandle annots = pdf.makeIndirectObject(QPDFObjectHandle::newArray());
        annots.appendItem(widget);
        firstPage.replaceKey("/Annots", annots);
    }

    // AcroForm in Root
    QPDFObjectHandle root = pdf.getRoot();
    QPDFObjectHandle fieldRef = widget;
    if (root.hasKey("/AcroForm")) {
        QPDFObjectHandle acroForm = root.getKey("/AcroForm");
        QPDFObjectHandle fields = acroForm.getKey("/Fields");
        fields.appendItem(fieldRef);
        acroForm.replaceKey("/SigFlags", QPDFObjectHandle::newInteger(3));
    } else {
        QPDFObjectHandle acroForm = pdf.makeIndirectObject(QPDFObjectHandle::newDictionary());
        QPDFObjectHandle fields = QPDFObjectHandle::newArray();
        fields.appendItem(fieldRef);
        acroForm.replaceKey("/Fields", fields);
        acroForm.replaceKey("/SigFlags", QPDFObjectHandle::newInteger(3));
        root.replaceKey("/AcroForm", acroForm);
    }

    // Write via QPDFWriter — disable object streams (byte-surgery needs flat objects)
    QString tmpPath = outputPath + ".signing_tmp";
    try {
        QPDFWriter writer(pdf, tmpPath.toUtf8().constData());
        writer.setObjectStreamMode(qpdf_o_disable);
        writer.setStreamDataMode(qpdf_s_preserve);
        writer.write();
    } catch (const std::exception& e) {
        errorMsg = QStringLiteral("QPDFWriter failed: %1").arg(e.what());
        QFile::remove(tmpPath);
        return false;
    }

    // ════════════════════════════════════════════════════════════════════════
    // Step 3 — byte surgery
    // ════════════════════════════════════════════════════════════════════════
    QFile tmpFile(tmpPath);
    if (!tmpFile.open(QIODevice::ReadOnly)) {
        errorMsg = QStringLiteral("Cannot read QPDF output");
        QFile::remove(tmpPath);
        return false;
    }
    QByteArray rawOut = tmpFile.readAll();
    tmpFile.close();

    // 3a — Locate /Contents hex placeholder: < + 32768 zeros + >
    const QByteArray contentsPlaceholder = QByteArray("<") + QByteArray(32768, '0') + ">";
    qsizetype contentsStart = rawOut.indexOf(contentsPlaceholder);
    if (contentsStart < 0) {
        errorMsg = QStringLiteral("Cannot find /Contents placeholder in QPDF output");
        QFile::remove(tmpPath);
        return false;
    }
    // Verify it appears exactly once
    qsizetype contentsSecond = rawOut.indexOf(contentsPlaceholder, contentsStart + 1);
    if (contentsSecond >= 0) {
        errorMsg = QStringLiteral("Duplicate /Contents placeholder found");
        QFile::remove(tmpPath);
        return false;
    }

    qsizetype contentsEnd = contentsStart + contentsPlaceholder.size();

    // 3b — Compute real /ByteRange
    qint64 br1 = 0;
    qint64 br2 = contentsStart;
    qint64 br3 = contentsEnd;
    qint64 br4 = rawOut.size() - contentsEnd;

    // 3c — Locate and replace /ByteRange placeholder
    const QByteArray brPlaceholder("[0 9999999999 9999999999 9999999999]");
    qsizetype brPos = rawOut.indexOf(brPlaceholder);
    if (brPos < 0) {
        errorMsg = QStringLiteral("Cannot find /ByteRange placeholder in QPDF output");
        QFile::remove(tmpPath);
        return false;
    }
    qsizetype brSecond = rawOut.indexOf(brPlaceholder, brPos + 1);
    if (brSecond >= 0) {
        errorMsg = QStringLiteral("Duplicate /ByteRange placeholder found");
        QFile::remove(tmpPath);
        return false;
    }

    QString brReal = QString("[%1 %2 %3 %4]")
        .arg(br1).arg(br2).arg(br3).arg(br4);
    QByteArray brRealBytes = brReal.toLatin1();
    // Pad with spaces to match exact placeholder length
    if (brRealBytes.size() > brPlaceholder.size()) {
        errorMsg = QStringLiteral("/ByteRange value overflow (placeholder too small)");
        QFile::remove(tmpPath);
        return false;
    }
    while (brRealBytes.size() < brPlaceholder.size())
        brRealBytes.append(' ');
    memcpy(rawOut.data() + brPos, brRealBytes.constData(), brRealBytes.size());

    // 3d — Assemble signed data (range 1 + range 2)
    QByteArray signedData;
    signedData.append(rawOut.constData() + br1, static_cast<int>(br2));
    signedData.append(rawOut.constData() + br3, static_cast<int>(br4));

    // 3e — Create CMS signature (SHA-256, detached)
    BioPtr dataBio(BIO_new_mem_buf(signedData.constData(),
                                   static_cast<int>(signedData.size())));
    if (!dataBio.p) {
        errorMsg = QStringLiteral("BIO_new_mem_buf failed for signed data");
        QFile::remove(tmpPath);
        return false;
    }

    CmsPtr cms(CMS_sign(nullptr, nullptr, nullptr, nullptr,
        CMS_DETACHED | CMS_BINARY | CMS_PARTIAL | CMS_NOSMIMECAP));
    if (!cms.p) {
        unsigned long err = ERR_get_error();
        char buf[256];
        ERR_error_string_n(err, buf, sizeof(buf));
        errorMsg = QStringLiteral("CMS_sign (partial) failed: %1").arg(buf);
        QFile::remove(tmpPath);
        return false;
    }

    CMS_SignerInfo* si = CMS_add1_signer(cms, cert, pkey, EVP_sha256(),
        CMS_DETACHED | CMS_BINARY | CMS_NOSMIMECAP);
    if (!si) {
        unsigned long err = ERR_get_error();
        char buf[256];
        ERR_error_string_n(err, buf, sizeof(buf));
        errorMsg = QStringLiteral("CMS_add1_signer failed: %1").arg(buf);
        QFile::remove(tmpPath);
        return false;
    }

    // Add intermediate certificates
    if (chain) {
        for (int i = 0; i < sk_X509_num(chain.get()); ++i) {
            CMS_add1_cert(cms, sk_X509_value(chain.get(), i));
        }
    }

    if (!CMS_final(cms, dataBio, nullptr, CMS_DETACHED | CMS_NOSMIMECAP)) {
        unsigned long err = ERR_get_error();
        char buf[256];
        ERR_error_string_n(err, buf, sizeof(buf));
        errorMsg = QStringLiteral("CMS_final failed: %1").arg(buf);
        QFile::remove(tmpPath);
        return false;
    }

    // DER encode
    int derLen = i2d_CMS_ContentInfo(cms, nullptr);
    if (derLen <= 0) {
        errorMsg = QStringLiteral("i2d_CMS_ContentInfo (null) failed");
        QFile::remove(tmpPath);
        return false;
    }
    if (static_cast<size_t>(derLen) > 16384) {
        errorMsg = QStringLiteral("CMS signature too large (%1 bytes, max 16384)").arg(derLen);
        QFile::remove(tmpPath);
        return false;
    }
    QByteArray der(derLen, '\0');
    unsigned char* derPtr = reinterpret_cast<unsigned char*>(der.data());
    int derLen2 = i2d_CMS_ContentInfo(cms, &derPtr);
    if (derLen2 <= 0) {
        errorMsg = QStringLiteral("i2d_CMS_ContentInfo failed");
        QFile::remove(tmpPath);
        return false;
    }

    // Hex-encode DER and write into /Contents (after '<')
    QByteArray hexDer = der.toHex(); // lowercase hex — perfectly valid per PDF spec
    qsizetype contentWritePos = contentsStart + 1; // after '<'
    memcpy(rawOut.data() + contentWritePos, hexDer.constData(),
           qMin(hexDer.size(), 32768));

    // 3f — Write output via QSaveFile (atomic)
    QSaveFile saveFile(outputPath);
    if (!saveFile.open(QIODevice::WriteOnly)) {
        errorMsg = QStringLiteral("Cannot write output PDF \"%1\"").arg(outputPath);
        QFile::remove(tmpPath);
        return false;
    }
    saveFile.write(rawOut);
    if (!saveFile.commit()) {
        errorMsg = QStringLiteral("Failed to atomically write output PDF");
        QFile::remove(tmpPath);
        return false;
    }

    // Clean up temp file
    QFile::remove(tmpPath);

    // ════════════════════════════════════════════════════════════════════════
    // Step 4 — self-verify
    // ════════════════════════════════════════════════════════════════════════
    QMutexLocker lock(&PdfDocument::pdfiumGlobalMutex());

    QFile verifyFile(outputPath);
    if (!verifyFile.open(QIODevice::ReadOnly)) {
        errorMsg = QStringLiteral("Cannot open signed file for self-verify");
        return false;
    }
    QByteArray verifyData = verifyFile.readAll();
    verifyFile.close();

    FPDF_FILEACCESS verifyFA = {};
    verifyFA.m_FileLen = static_cast<unsigned long>(verifyData.size());
    verifyFA.m_GetBlock = getBlockCallback;
    verifyFA.m_Param = &verifyData;

    FPDF_DOCUMENT verifyDoc = FPDF_LoadCustomDocument(&verifyFA, nullptr);
    if (!verifyDoc) {
        errorMsg = QStringLiteral("Cannot load signed PDF for self-verify");
        return false;
    }

    QList<SignatureInfo> sigs = readAndVerify(verifyDoc, outputPath);
    FPDF_CloseDocument(verifyDoc);
    lock.unlock();

    if (sigs.isEmpty() || !sigs[0].cryptoValid) {
        errorMsg = sigs.isEmpty()
            ? QStringLiteral("Self-verify: no signature found")
            : QStringLiteral("Self-verify failed: %1").arg(sigs[0].errorMsg);
        return false;
    }

    return true;
}
