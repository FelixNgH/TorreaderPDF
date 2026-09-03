#pragma once
#include <QImage>
#include <QRect>
#include <atomic>
#include <fpdfview.h>

class ForeignAnnotLayer {
public:
    // LOP COMMENT (owner chot 02/09): MOT anh TRONG SUOT co trang, gom MOI chu thich ngoai
    // cua trang, dung MOT LAN o ti le hien thi (maxPx), chay tren LUONG NEN. Sinh bang cach
    // render clip dung o TUNG annot (do: ~20.9ms cold / <1ms warm) roi gao nen trang — KHONG
    // con la "phep tru hai lan render ca trang" (36-41 giay) cua ban cu.
    bool build(FPDF_DOCUMENT doc, int pageIndex, int maxPx);

    // Huy tac vu dang chay. MainWindow goi truoc khi dong/giai phong tai lieu de
    // build()/buildRegion() TRA VE NGAY tai ranh pha (tai lieu bi huy = con tro
    // FPDF_DOCUMENT chet => pdfium ghi vao vung da giai phong => crash 30/08).
    void cancel() { m_cancel = true; }

    bool          isReady()   const { return m_ready; }
    int           pageIndex() const { return m_page; }
    const QImage& image()     const { return m_img; }

    // Muc zoom da dung khi build lop bu nay (SPEC_PERF_FGNLAYER_ADAPTIVE 2026-08-30).
    void   setBuiltZoom(double z) { m_builtZoom = z; }
    double builtZoom()      const { return m_builtZoom; }

    // Vung sac net theo zoom that. Goi khi DANG giu khoa pdfium (giong build()).
    bool buildRegion(FPDF_DOCUMENT doc, int pageIndex, double scale, QRect regionPx);

    bool          regionReady() const { return m_regReady; }
    int           regionPage()  const { return m_regPage; }
    double        regionScale() const { return m_regScale; }
    QRect         regionRect()  const { return m_regRect; }
    const QImage& regionImage() const { return m_regImg; }

private:
    // LOC y het mot annot "ngoai": bo TRUID (markup cua app), bo HIDDEN, bo POPUP, chi lay co /AP.
    // GIA DINH dang GIU s_pdfiumMutex. Dung CHUNG cho computeForeignBbox va build() —
    // mot nguoi cham loc duy nhat, de lop comment va bbox khong bao gio lech nhau.
    static bool annotIsForeign(FPDF_ANNOTATION a);
    // Tinh BBOX hop nhat cua annot ngoai (loc qua annotIsForeign).
    // GIA DINH dang GIU s_pdfiumMutex. Tra false neu khong co annot nao.
    static bool computeForeignBbox(FPDF_PAGE page, FS_RECTF& out);
    // Chuyen FS_RECTF (toa do page, goc trai-duoi) sang QRect (toa do pixel, goc trai-tren).
    static QRect bboxToPx(const FS_RECTF& bbox, double pageH, double scale);

    std::atomic<bool> m_cancel{false};

    QImage m_img;
    int    m_page  = -1;
    bool   m_ready = false;
    double m_builtZoom = 0.0;   // zoom luc build lop bu (de biet khi nao can dung lai)

    QImage m_regImg;
    QRect  m_regRect;
    double m_regScale = 0.0;
    int    m_regPage  = -1;
    bool   m_regReady = false;
};
