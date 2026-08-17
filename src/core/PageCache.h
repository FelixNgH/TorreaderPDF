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
// So phan tu: kCapacity = 6 (LRU). Mot trang duoc "touch" khi acquire/tryAcquire -> MRU.
// Trang moi muon duoc ben goi giu an toan trong khi dung: ben goi giu s_pdfiumMutex trong
// luc dung handle. Eviction chi chay khi co luot nap moi (duoi s_pdfiumMutex) nen handle
// dang duoc dung (cung mutex) khong the bi dong giua chung.

class PageCache {
public:
    static constexpr int kCapacity = 6;   // LRU

    // Lay trang tu dem; truot thi FPDF_LoadPage. Tra nullptr neu that bai.
    // GIA DINH ben goi giu s_pdfiumMutex. Chi goi tu luong nen / duong sua annot.
    static FPDF_PAGE acquire(FPDF_DOCUMENT doc, int pageIndex);

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
    };

    static void touch_locked(const Key& k);
    static FPDF_PAGE loadAndRegister(FPDF_DOCUMENT doc, int pageIndex);
    static void closeEntry(Entry& e);
    static void evict_locked();      // GIA DINH giu ca s_pdfiumMutex lan s_mutex

    static QMutex  s_mutex;                       // cache bookkeeping
    static QHash<Key, Entry> s_entries;           // (doc,page) -> page+tp+meta
    static QList<Key> s_lru;                      // MRU o cuoi, evict tu dau
    static QSet<Key>  s_inflight;                 // prefetch dang cho/chay
    static QHash<FPDF_DOCUMENT, quint64> s_epoch; // doc con song; remove khi forgetDocument
};
