#pragma once
#include <QVector>
#include <QRectF>
#include <QPointF>
#include <QString>
#include <QMutex>
#include <QHash>
#include <QSet>
#include <QObject>
#include <fpdfview.h>

// Tai tin hieu "da tinh link xong" tu PdfLinks sang GUI thread (xem
// SPEC_NO_SYNC_PAGELOAD_2026-08-16). View connect vao day de ve lai con tro.
// doc la quintptr (khong phai FPDF_DOCUMENT) vi queued connection khong the
// van chuyen kieu pointer cua PDFium khi chua qRegisterMetaType.
class PdfLinksNotifier : public QObject {
    Q_OBJECT
public:
    explicit PdfLinksNotifier(QObject* parent = nullptr) : QObject(parent) {}
    void emitReady(FPDF_DOCUMENT doc, int page) {
        emit linksReady(reinterpret_cast<quintptr>(doc), page);
    }
signals:
    void linksReady(quintptr doc, int pageIndex);
};

// Doc danh sach link trong PDF (SPEC_PDF_LINKS_2026-08-16).
//
// rectPdf: page space CHUA ap /Rotate — dung khong gian PDF goc (Y len, goc
// duoi trai) nhu FPDFLink_GetAnnotRect tra ve. Muon hit-test o toa do hien thi
// thi dung PageInfo + dispToPdf()/linkAt().
struct PdfLink {
    QRectF  rectPdf;        // page space, chua ap /Rotate
    int     destPage = -1;  // >= 0 la link noi bo
    double  destX = 0, destY = 0;  // vi tri trong trang dich (neu co, -1 neu khong xac dinh)
    QString uri;            // khac rong la link ngoai
};

class PdfLinks {
public:
    // Thong tin trang can thiet de quy doi toa do, lay nhanh tu cache —
    // KHONG can goi PDFium lai moi lan di chuot.
    struct PageInfo {
        double dispW = 0, dispH = 0;  // kich thuoc hien thi (da ap /Rotate), point
        int rot = 0;                  // FPDFPage_GetRotation
        double boxX = 0, boxY = 0;    // goc hop trang (CropBox.left/bottom)
    };

    // Danh sach link cua trang (co bo nho dem theo trang, trang khong link
    // cung duoc dem de khong quet lai). Tra rong khi doc/page khong hop le.
    // DONG BO: co the mat ~1s cho trang CAD nang — KHONG goi trong mouseMoveEvent.
    static QVector<PdfLink> forPage(FPDF_DOCUMENT doc, int pageIndex);

    // CHI doc dem, KHONG nap gi (SPEC_NO_SYNC_PAGELOAD muc 1). ready=false =
    // trang chua tinh link → con tro giu mui ten, goi requestPage mot lan.
    struct CachedPage {
        bool ready = false;
        QVector<PdfLink> links;
        PageInfo info;
    };
    static CachedPage cachedForPage(FPDF_DOCUMENT doc, int pageIndex);

    // Day viec tinh link sang QtConcurrent, ghi dem + emit notifier()->linksReady
    // khi xong. Dang tinh roi / da co dem thi khong lam gi (khong xep hang doi).
    static void requestPage(FPDF_DOCUMENT doc, int pageIndex);

    // QObject duoc emit tren GUI thread moi lan mot trang tinh link xong.
    static PdfLinksNotifier* notifier();

    // Thong tin trang cho quy doi toa do (tu cache cua forPage; neu chua co
    // thi tinh moi — moi goi deu duoc cache).
    static PageInfo pageInfo(FPDF_DOCUMENT doc, int pageIndex);

    // Diem toa do HIEN THI (Y xuong, goc trai tren, da ap /Rotate + box origin)
    // -> diem PDF CHUA xoay. Dung lai dung phep quy doi dispToPdf cua PdfCoords.
    static QPointF dispToPdf(const QPointF& disp, const PageInfo& info);

    // -1 neu khong trung link nao; nguoc lai chi so trong links.
    static int linkAt(const QVector<PdfLink>& links, const QPointF& dispPoint,
                      const PageInfo& info);

    // Xoa toan bo cache (goi khi dong tai lieu / load lai file).
    static void clearCache();

private:
    static quint64 key(FPDF_DOCUMENT doc, int pageIndex) {
        return (quint64(reinterpret_cast<quintptr>(doc)) << 32) ^ quint32(pageIndex);
    }

    static QMutex  s_mutex;
    static QHash<quint64, QVector<PdfLink>> s_linkCache;
    static QHash<quint64, PageInfo>         s_infoCache;
    // Trang dang tinh link o background (chong xep hang hai lan).
    static QSet<quint64> s_pending;
    // Tang moi lan clearCache (= doc sap dong/mo lai) → huy request dang bay.
    static quint64 s_epoch;
};
