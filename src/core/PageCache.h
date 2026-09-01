#pragma once
#include <QPointF>
#include <QSizeF>
#include <QMutex>
#include <QHash>
#include <QList>
#include <QSet>
#include <QPair>
#include <fpdfview.h>
#include <fpdf_text.h>

// ── PageCache — NGUON SU THAT DUY NHAT cua FPDF_PAGE (SPEC_PAGECACHE_CORE_2026-08-16) ──
//
// Quy tac BAT DI BAT DICH:
//  🔴 PageCache la CHU SO HUU duy nhat cua FPDF_PAGE (va FPDF_TEXTPAGE theo trang).
//     Ben goi KHONG BAO GIO goi FPDF_ClosePage / FPDFText_ClosePage tren handle muon.
//  🔴 Moi truy cap PDFium cua ben goi van phai nam trong QMutexLocker lock(&s_pdfiumMutex)
//     (PDFium khong an toan da luong). acquire()/textPage()/invalidate()/forgetDocument()
//     GIA DINH ben goi DA giu s_pdfiumMutex — goi tren luong nen hoac trong mutex.
//  🔴 tryAcquire()/tryAcquireTextPage() KHONG nap va KHONG cham s_pdfiumMutex — dung duoc
//     o mouseMoveEvent/paintEvent. acquire() DUOC PHEP nap, chi goi tu luong nen.
//  🔴 prefetch() khong chan (QtConcurrent), dung sau khi trang on dinh 300 ms.
//  🔴 invalidate() sau moi lan sua annotation; forgetDocument() khi dong/mo lai tai lieu —
//     bo sot = dung con tro chet = crash.
//
// So phan tu: kCapacity = 16 (LRU) — SPEC_PAGECACHE_THRASH_2026-08-31. Truoc day = 6 qua
// nho: View Fast (Continuous) + 2 tai lieu mo cung luc chac chan thrash, va trang vua nap
// cua tab nen bi evict ngay (xem loadAndRegister). Mot trang duoc "touch" khi
// acquire/tryAcquire -> MRU. Tran BO NHO kMaxBytes (proxy raster footprint) giu so duong
// cho moi tai lieu de.
//
// 🔴🔴 LUAT MUON (BORROW) — SPEC_PERF_HEAVYPAGE R1, sua tu R1-vá 2026-08-18:
//   Luat cu "ben goi giu s_pdfiumMutex trong luc dung handle" da BI PHA: ProgressiveRenderTask
//   va RegionRenderTask trong PdfRenderer.cpp BUOC PHAI tha s_pdfiumMutex giua cac slice
//   ("Mutex unlocked here — other threads can use PDFium between slices"). Trong khoang tha
//   khoa ay, luong khac co the evict va FPDF_ClosePage cai trang dang ve => con tro chet.
//   => Moi handle tra ra tu acquire() phai di CAP VOI release():
//      - acquire() tang borrow cua entry len 1 TRUOC khi tra con tro.
//      - release(doc, page) giam borrow (khong am). Goi khi KHONG con dung handle nua.
//      - evict_locked() BO QUA entry co borrow > 0 (giong pinned) — khong dong khi dang muon.
//      - invalidate()/forgetDocument(): entry con borrow > 0 thi CHI danh dau "cho dong"
//        (doomed = true), KHONG FPDF_ClosePage ngay; dong that su o lan truy cap sau khi
//        borrow ve 0 (acquire/evict chay duoi s_pdfiumMutex se don dep).
//   RAII: PageBorrow lo viec release khi het scope — moi ben goi acquire() deu phai cap doi
//   (danh sach ben goi: PdfRenderer ca 4 task, ThumbnailWorker, VectorLayer::build,
//   AnnotationManager/AnnotationLayer, TextSelection::PageInfo, PdfLinks::computePage, MainWindow).
//   KHONG de ro muon: borrow khong ve 0 = entry do khong bao gio bi duoi duoc.
//
// Pin: trang dang "pin" THI KHONG BI evict/LRU duoi (SPEC_PERF_HEAVYPAGE R1). Toi da
// kMaxPinned = 3 trang pin cung luc; pin them thi bo pin cu nhat. forgetDocument() go
// pin cua doc do TRUOC khi dong handle nen loc entry khong the giu con tro da free.

class PageCache {
public:
    static constexpr int kCapacity = 16;   // moc TOT 31/08
    static constexpr qint64 kMaxBytes = 512 * 1024 * 1024;  // tran bo nho ~512 MB (proxy raster RGBA)
    static constexpr int kMaxPinned = 3;            // so trang pin toi da cung luc

    // Lay trang tu dem; truot thi FPDF_LoadPage. Tra nullptr neu that bai.
    // GIA DINH ben goi giu s_pdfiumMutex. Chi goi tu luong nen / duong sua annot.
    // 🔴 Tang borrow len 1 — PHAI di cap voi release(doc, pageIndex) (xem PageBorrow).
    static FPDF_PAGE acquire(FPDF_DOCUMENT doc, int pageIndex);

    // Giam borrow cua entry (khong am). Goi khi ben goi KHONG con dung handle nua.
    // An toan goi ca khi KHONG giu s_pdfiumMutex (chi lock s_mutex noi bo; entry dang
    // cho dong (doomed/orphan) se duoc FPDF_ClosePage o lan truy cap sau duoi s_pdfiumMutex —
    // acquire()/evict_locked()/sweep se don dep).
    // `page` = handle ma ben goi dang giu (khong doi khi ben goi co the da muon tu mot
    // orphan da bi tach khoi map). Neu nullptr → giam entry trong map theo (doc,pageIndex).
    static void release(FPDF_DOCUMENT doc, int pageIndex, FPDF_PAGE page = nullptr);

    // ── RAII cho cap acquire/release ────────────────────────────────────────────
    // Tao ngay sau acquire(doc, pageIndex) thanh cong: tu dong goi release khi ra khoi
    // scope (moi duong thoat: return som, huy giua chung, nem loi). Move-only.
    // `page` la handle acquire() vua tra — bat buoc de release() giam dem DUNG entry
    // khi cung (doc,page) co nhieu handle song song (reload-while-borrowed, xem s_orphans).
    class PageBorrow {
    public:
        PageBorrow() = default;
        PageBorrow(FPDF_DOCUMENT doc, int pageIndex, FPDF_PAGE page = nullptr)
            : m_doc(doc), m_page(pageIndex), m_pg(page) {}
        ~PageBorrow() { release(); }
        PageBorrow(const PageBorrow&) = delete;
        PageBorrow& operator=(const PageBorrow&) = delete;
        PageBorrow(PageBorrow&& o) noexcept
            : m_doc(o.m_doc), m_page(o.m_page), m_pg(o.m_pg) { o.m_doc = nullptr; o.m_pg = nullptr; }
        PageBorrow& operator=(PageBorrow&& o) noexcept {
            if (this != &o) { release(); m_doc = o.m_doc; m_page = o.m_page; m_pg = o.m_pg; o.m_doc = nullptr; o.m_pg = nullptr; }
            return *this;
        }
        void borrow(FPDF_DOCUMENT doc, int pageIndex, FPDF_PAGE page = nullptr)
            { release(); m_doc = doc; m_page = pageIndex; m_pg = page; }
        void release() { if (m_doc) { PageCache::release(m_doc, m_page, m_pg); m_doc = nullptr; m_pg = nullptr; } }
    private:
        FPDF_DOCUMENT m_doc = nullptr;
        int           m_page = -1;
        FPDF_PAGE     m_pg = nullptr;
    };

    // Pin trang de LRU khong duoi (trang hien tai va +-1). Toi da kMaxPinned cung luc;
    // pin them thi bo pin cu nhat. Goi tren UI thread, khong cham s_pdfiumMutex (khong
    // FPDF_LoadPage o day — chi de dau / go pin tren entry da co hoac se co).
    static void pin(FPDF_DOCUMENT doc, int pageIndex);

    // Go pin. Khong lam gi khac.
    static void unpin(FPDF_DOCUMENT doc, int pageIndex);

    // Doc dang hien hanh (tab dang xem) — duoi cache uu tien doc KHONG hien hanh truoc.
    // MainWindow goi khi doi tab. Doc null = khong co tab nao (het cache, duoi tu do).
    static void setActiveDoc(FPDF_DOCUMENT doc);

    // CHI doc dem, KHONG nap. Dung o duong giao dien / di chuot. Khoa mutex noi bo
    // cua cache (khong cham s_pdfiumMutex). Tra nullptr neu trang chua co.
    static FPDF_PAGE tryAcquire(FPDF_DOCUMENT doc, int pageIndex);

    // Lay FPDF_TEXTPAGE cua trang, tao lan dau neu chua co. Trang phai DA co trong
    // dem (goi acquire/tryAcquire truoc). GIA DINH ben goi giu s_pdfiumMutex.
    static FPDF_TEXTPAGE textPage(FPDF_DOCUMENT doc, int pageIndex);

    // Lay FPDF_TEXTPAGE tu dem, KHONG tao, KHONG nap, KHONG cham s_pdfiumMutex.
    // Tra nullptr neu trang chua co / chua tao text page. Dung o mouseMove.
    static FPDF_TEXTPAGE tryAcquireTextPage(FPDF_DOCUMENT doc, int pageIndex);

    // Xep nap nen (khong chan, QtConcurrent). Dang nap roi / da co trong dem thi bo qua.
    static void prefetch(FPDF_DOCUMENT doc, int pageIndex);

    // Bo mot trang khoi dem (goi sau khi trang bi sua doi — annot them/xoa/dich,
    // FPDFPage_GenerateContent). Dong ca FPDF_PAGE lan FPDF_TEXTPAGE.
    // GIA DINH ben goi giu s_pdfiumMutex.
    static void invalidate(FPDF_DOCUMENT doc, int pageIndex);
    static qint64 totalBytes();   // bai do bo nho
    static int    entryCount();

    // Sửa MARKUP (thêm/xoá/sửa annot): FPDFPage_CreateAnnot/RemoveAnnot sửa TRỰC TIẾP
    // đối tượng trang mà FPDF_PAGE trong cache đang trỏ tới ⇒ handle vẫn hợp lệ và đã phản
    // ánh thay đổi (SPEC_MARKUP_FIX_2026-08-31). Chỉ tăng bộ đếm thế hệ annot cho (doc,page)
    // để các cache dẫn xuất (danh sách annot, loadPageVisuals, ảnh raster) biết cần đọc lại.
    // KHÔNG đụng `doomed`, KHÔNG closeEntry, KHÔNG đóng FPDF_PAGE/FPDF_TEXTPAGE. GIA DINH
    // ben goi giu s_pdfiumMutex (nhu invalidate).
    static void bumpAnnotGeneration(FPDF_DOCUMENT doc, int pageIndex);

    // Bộ đếm thế hệ annot hiện tại của (doc,page) — 0 nếu trang chưa nạp. Dùng để cache
    // annot/visuals biết nội dung đã đổi chưa. KHÔNG cham s_pdfiumMutex.
    static quint64 annotGeneration(FPDF_DOCUMENT doc, int pageIndex);

    // Bo toan bo trang cua mot tai lieu (goi khi dong / mo lai tai lieu). Dong ca
    // FPDF_PAGE lan FPDF_TEXTPAGE cua doc. GIA DINH ben goi giu s_pdfiumMutex.
    static void forgetDocument(FPDF_DOCUMENT doc);

    static int size();

    // Thong tin trang dem duoc (tu Entry da nap) — tra false neu trang chua co.
    struct PageMeta {
        int     rot = 0;
        QPointF box;
        QSizeF  disp;
    };
    static bool metaFor(FPDF_DOCUMENT doc, int pageIndex, PageMeta& out);

private:
    using Key = QPair<FPDF_DOCUMENT, int>;
    struct Entry {
        FPDF_PAGE     page = nullptr;
        FPDF_TEXTPAGE tp   = nullptr;
        int           rot  = 0;
        QPointF       box;
        QSizeF        disp;
        int           borrow = 0;   // dem muon — doi release() moi duoc dong/duoi (SPEC R1)
        bool          doomed = false; // invalidate/forgetDocument khi borrow>0: cho dong
        quint64       annotGen = 0; // the he annot — bumpAnnotGeneration (SPEC_MARKUP_FIX_2026-08-31)
        qint64        bytes  = 0;   // uoc luong bo nho (raster RGBA w*h*4) — SPEC_PAGECACHE_THRASH_2026-08-31
    };

    static void touch_locked(const Key& k);
    static FPDF_PAGE loadAndRegister(FPDF_DOCUMENT doc, int pageIndex);
    static void closeEntry(Entry& e);
    static void evict_locked(const Key* keep = nullptr);  // GIA DINH giu ca s_pdfiumMutex lan s_mutex
    static void sweepOrphans_locked();  // GIA DINH giu ca s_pdfiumMutex lan s_mutex
    static void unpin_locked(const Key& k);  // GIA DINH giu s_mutex

    static QMutex  s_mutex;                       // cache bookkeeping
    static QHash<Key, Entry> s_entries;           // (doc,page) -> page+tp+meta
    static QList<Key> s_lru;                      // MRU o cuoi, evict tu dau
    static qint64 s_totalBytes;               // tong bytes cua cac entry dang giu (proxy)
    static QList<QPair<Key, Entry>> s_orphans;    // entry "mo coi" (doomed khi con borrow,
                                                  // da tach khoi map de acquire nap MOI) —
                                                  // dong khi borrow ve 0 (xem sweepOrphans_locked)
    static QSet<Key>  s_pinned;                   // trang dang duoc pin (khong bi evict)
    static QList<Key> s_pinOrder;                 // thu tu pin (cu o dau) de bo pin cu nhat
    static QSet<Key>  s_inflight;                 // prefetch dang cho/chay
    static QHash<FPDF_DOCUMENT, quint64> s_epoch; // doc con song; remove khi forgetDocument
    static FPDF_DOCUMENT s_activeDoc;             // doc cua tab dang xem
};
