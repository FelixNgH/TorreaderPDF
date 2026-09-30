#pragma once
#include <QPointF>
#include <QSizeF>
#include <QMutex>
#include <QHash>
#include <QList>
#include <QSet>
#include <QPair>
#include <QString>
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
    // 🔴 LƯỢT 33b (J): doc dang hien hanh de viec NEN (prefetch/vector/annot scan)
    // tu huy khi khong phai tab dang xem — mot chot doc, khong ghi de.
    static FPDF_DOCUMENT activeDoc();

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

    // 🔴 LƯỢT 27 (săn rò): sổ handle còn SỐNG thật, đọc được khi KHÔNG giu
    // s_pdfiumMutex (chi khoa s_mutex noi bo). doomed = entry cho dong con borrow>0
    // (forgetDocument/invalidate danh dau ma CHUA FPDF_ClosePage); orphan = entry da
    // tach khoi map doi borrow ve 0; deadDocs = doc da forgetDocument (s_dead).
    // Khong lech = moi FPDF_PAGE cua doc dong tab deu duoc FPDF_ClosePage that.
    static int doomedCount();
    static int orphanCount();
    static int deadDocCount();

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

    // [closeorder] 0927 LƯỢT 7: doc vừa được MỞ LẠI (sau FPDF_LoadDocument) thì bỏ
    // cờ "đã ngưng phục vụ" của PageCache. Điểm neo là PdfCloseTrace::docOpen — mọi
    // lần mở doc trong app đều đi qua đó, nên cờ KHÔNG bao giờ bị sót (địa chỉ
    // FPDF_DOCUMENT thường được dùng lại ngay sau FPDF_CloseDocument).
    static void documentOpened(FPDF_DOCUMENT doc);

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
        FPDF_DOCUMENT doc  = nullptr; // [closeorder] doc chu so huu FPDF_PAGE/tp (de in log)
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
    // 🔴 [closeorder] LƯỢT 7: doc ĐÃ bị forgetDocument. Trước đây `loadAndRegister`
    // ghi `s_epoch.insert(doc, …)` vô điều kiện ⇒ MỘT TÀI LIỆU ĐÃ ĐÓNG LẠI ĐƯỢC
    // "hồi sinh": acquire() nạp trang mới, trang đó không còn ai đóng (forgetDocument
    // đã chạy xong) và sống tới khi FPDF_CloseDocument giật object dưới chân nó ⇒
    // lần FPDF_ClosePage sau đó thả object đã có refcount = 0 ⇒ CHECK 0x80000003
    // (đúng lệnh `int3` tại RVA 0x1754b mà objdump chỉ ra trong pdfium.dll).
    static QSet<FPDF_DOCUMENT> s_dead;            // xem documentOpened()
    static FPDF_DOCUMENT s_activeDoc;             // doc cua tab dang xem
};

// ── [closeorder] 0927 LƯỢT 2: SO THU TU DONG TAI LIEU ────────────────────────
// Crash PDFium 0x80000003 (CHECK/__debugbreak) luc dong tai lieu eYACHO. De doc
// thu tu THAT tren may that, dem FPDF_DOCUMENT / FPDF_PAGE / FPDF_TEXTPAGE /
// FPDF_ANNOTATION con SONG theo tung doc va in `[closeorder]` o MOI diem goi
// PDFium tren duong dong tai lieu. LƯỢT 2 them:
//   • ghi them ra FILE RIENG %TEMP%\torreader_closeorder.log, FLUSH TUNG DONG
//     (log chinh co the mat dong cuoi khi tien trinh chet giua chung);
//   • in `tid=` (ma luong) — lan truoc chi biet THU TU, khong biet luong nao chet;
//   • DOC-CLOSE tach TRUOC (…-BEGIN) va SAU (…-DONE) lenh FPDF_CloseDocument;
//   • FORM-BEFORECLOSE / FORM-EXIT / ANNOT-CLOSE / LIB-DESTROY (xem note()).
//
// CHI LOG — khong doi hanh vi gi, khong giu khoa PDFium, khong bao gio goi nguoc
// lai PageCache (la nut gi, giu PageCache::s_mutex thi an toan). Tat khi
// `TORREADER_CLOSEORDER=0` thi CHI dem, khong in log chinh (mac dinh: in);
// file rieng LUON ghi du khi tat vi no la bang chung.
class PdfCloseTrace {
public:
    static void docOpen      (FPDF_DOCUMENT d, const char* where);
    static void docCloseBegin(FPDF_DOCUMENT d, const char* where);  // TRUOC FPDF_CloseDocument
    static void docCloseDone (FPDF_DOCUMENT d, const char* where);  // SAU  FPDF_CloseDocument
    static void pageOpen     (FPDF_DOCUMENT d, int pageIndex, const char* where);
    static void pageClose    (FPDF_DOCUMENT d, FPDF_PAGE p, const char* where);
    static void textOpen     (FPDF_DOCUMENT d, FPDF_TEXTPAGE t, const char* where);
    static void textClose    (FPDF_DOCUMENT d, FPDF_TEXTPAGE t, const char* where);
    // Su kien bat bien co ten (FORM-BEFORECLOSE / FORM-EXIT / ANNOT-CLOSE /
    // LIB-DESTROY / WAIT-ANNOT …) — dung cho lenh khong gan duoc vao mot doc.
    static void note         (const char* kind, const QString& detail);

    // ── [closeorder] 0927 LƯỢT 7: SO DAY KHAI THAC + PHAT HIEN THA LAI ──────────
    // Bằng chứng máy test: `objdump` trên chính `third_party/pdfium/bin/pdfium.dll`
    // (MAJOR=151 MINOR=0 BUILD=7906) cho thấy ngoại lệ 0x80000003 nổ đúng tại RVA
    // 0x1754b = lệnh `int3` trong hàm CFX_RetainablePtr::Reset():
    //     mov 0x8(%rcx),%rax ; test %rax,%rax ; je <int3 @0x1754b>
    // tức là "thả một object có refcount ĐÃ BẰNG 0" = giải phóng lần hai. Nó chỉ
    // được gọi từ destructor ảo của lớp 104 byte giữ 2 CFX_RetainablePtr — tức
    // bên trong `FPDF_ClosePage` khi trang bị thả. ⇒ LƯỢT 2-6 đếm PAGE-OPEN ==
    // PAGE-CLOSE nhưng CHỈ trên tập site ĐÃ truy vết; các `FPDF_ClosePage` khác
    // trong app KHÔNG được đếm ⇒ cân bằng đó không đủ để loại trừ "đóng 2 lần".
    // Sửa ở đây: SỔ KHAI THÁC toàn cục + đóng CÓ KIỂM so "đã đóng chưa".
    //   • `closePage` / `closeTextPage` là ĐƯỜNG ĐÓNG DUY NHÁT — mọi FPDF_ClosePage
    //     / FPDFText_ClosePage trong app phải đi qua đây (thiếu = sổ sai).
    //   • Đóng handle đã đóng ⇒ in `PAGE-CLOSE-LAI` / `TEXT-CLOSE-LAI` kèm nơi
    //     đóng LẦN ĐẦU, rồi BỎ QUA lệnh đóng (pdfium CHECK giết tiến trình, mất
    //     sạn bằng chứng; bỏ qua thì lỗi vẫn còn nhưng app không chết).
    //   • Mở handle đã đóng ⇒ in `PAGE-MO-SAU-KHI-DONG` (dùng sau khi đóng).
    static void closePage    (FPDF_DOCUMENT d, FPDF_PAGE p, const char* where);
    static void closeTextPage(FPDF_DOCUMENT d, FPDF_TEXTPAGE t, const char* where);
    static void openPage     (FPDF_DOCUMENT d, FPDF_PAGE p, int pageIndex, const char* where);
    static void openTextPage (FPDF_DOCUMENT d, FPDF_TEXTPAGE t, const char* where);
    // Render tiến trình (FPDF_RenderPageBitmap_Start ... _Close) trên trang nào —
    // ứng viên (a) lượt 7. Số >0 lúc đóng trang = quên FPDF_RenderPage_Close.
    static void renderOpen  (FPDF_DOCUMENT d, FPDF_PAGE p, const char* where);
    static void renderClose (FPDF_DOCUMENT d, FPDF_PAGE p, const char* where);
    static int  renderDepth (FPDF_DOCUMENT d, FPDF_PAGE p);

    // Handle da tung duoc FPDF_ClosePage / FPDFText_ClosePage (nullptr = chua).

    // So handle con SONG cua doc (tru chinh no). 0 = sach => moi FPDF_CloseDocument
    // an toan. Doc xong ma doc da bi xoa khoi dem => tra -1 (khong con the dem).
    static int  pages (FPDF_DOCUMENT d);
    static int  texts (FPDF_DOCUMENT d);
    static int  liveDocs();
    static void reset();
};
