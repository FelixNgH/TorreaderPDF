#include "PageCache.h"
#include "PdfCoords.h"
#include "PdfiumLock.h"
#include "DocTaskGate.h"
#include "Bisect.h"
#include <fpdf_edit.h>
#include <fpdf_text.h>
#include <QMutexLocker>
#include <QtConcurrent>
#include <QElapsedTimer>
#include <QAtomicInteger>
#include <QDebug>
#include <QFile>
#include <QDir>
#include <QDateTime>
#include <QThread>

extern QMutex s_pdfiumMutex;

// ═══════════════════════════════════════════════════════════════════════════════
// [closeorder] 0927 LƯỢT 2 — dem handle PDFium con song theo tung FPDF_DOCUMENT.
// La NUT GI: chi giu khoa rieng, khong bao gio goi PageCache/PDFium. Vì vậy an
// toan khi bi goi tu noi dang giu PageCache::s_mutex (closeEntry).
// ═══════════════════════════════════════════════════════════════════════════════
namespace {
struct CloseCount { int pages = 0; int texts = 0; int renders = 0; };
QMutex                s_traceMutex;
QHash<FPDF_DOCUMENT, CloseCount> s_trace;
QSet<FPDF_DOCUMENT>   s_traceDocs;
QAtomicInteger<quint64> s_traceSeq{0};

// ── [closeorder] LƯỢT 7: SỔ KHAI THÁC handle PDFium (xem PageCache.h) ──────────
// Nhớ handle ĐÃ đóng + nơi đóng lần đầu, để phát hiện đóng 2 lần / dùng sau khi
// đóng — đúng thứ mà bộ đếm theo-doc của lượt 2-6 không nhìn thấy.
QHash<FPDF_PAGE,     const char*> s_closedPage;
QHash<FPDF_TEXTPAGE, const char*> s_closedText;
// Render tiến trình còn treo, theo (doc,page).
QHash<FPDF_PAGE, QPair<FPDF_DOCUMENT, int>> s_renderDepth;

bool closeOrderOn() {
    static const bool on = qgetenv("TORREADER_CLOSEORDER") != QByteArray("0");
    return on;
}
// So thu tu toan cuc: nhieu luong ve cung luc, phai duy nhat de doc lai hieu dung.
quint64 nextSeq() { return s_traceSeq.fetchAndAddOrdered(1) + 1; }

// ── FILE BANG CHUNG: %TEMP%\torreader_closeorder.log ──────────────────────────
// 0927 LƯỢT 2: log chinh (torreader.log) co the MAT DONG CUOI khi tien trinh
// chet trong pdfium.dll — do la thu duoc xac nhan (khong thay DOC-CLOSE nao).
// File nay mo Append + FLUSH TUNG DONG nen luon doc duoc toi dong chet.
// Dat duong dan bang TORREADER_CLOSEORDER_LOG de ghi ra cho khac.
void closeOrderFile(const QString& line) {
    static QMutex   fMutex;
    static QFile    f;
    static bool     tried = false;
    QMutexLocker lk(&fMutex);
    if (!tried) {
        tried = true;
        QString path = QString::fromLocal8Bit(qgetenv("TORREADER_CLOSEORDER_LOG"));
        if (path.isEmpty())
            path = QDir::tempPath() + QLatin1String("/torreader_closeorder.log");
        f.setFileName(path);
        f.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Append);
    }
    if (!f.isOpen()) return;
    f.write(line.toUtf8());
    f.write("\n");
    f.flush();          // BAT BUOC: dong chet phai con san o dia
}

// GHI RA HAI NOI: file rieng (luon) + log chinh (theo co). `tid=` la MA LUONG
// — 0927 LƯỢT 2: biet thu tu ma khong biet luong nao la vung chet thi vo dung.
void sink(quint64 seq, const QString& body, bool warn) {
    const QString line = QStringLiteral("[closeorder] #%1 %2 tid=%3")
                             .arg(seq, 6, 10, QLatin1Char('0'))
                             .arg(body)
                             .arg(reinterpret_cast<quintptr>(QThread::currentThreadId()), 0, 16);
    closeOrderFile(line);
    if (!closeOrderOn()) return;
    if (warn) qWarning().noquote() << line;
    else      qDebug().noquote()   << line;
}
} // namespace

void PdfCloseTrace::docOpen(FPDF_DOCUMENT d, const char* where) {
    if (!d) return;
    PageCache::documentOpened(d);   // 0927 LƯỢT 7: bỏ cờ "doc đã dọn" của PageCache
    quint64 seq;
    {
        QMutexLocker lk(&s_traceMutex);
        s_traceDocs.insert(d);
        s_trace[d];
        seq = nextSeq();
    }
    sink(seq, QStringLiteral("DOC-OPEN  doc=%1 tai=%2 pages=0 texts=0")
                   .arg(reinterpret_cast<quintptr>(d), 0, 16)
                   .arg(QString::fromLatin1(where)), false);
}

void PdfCloseTrace::docCloseBegin(FPDF_DOCUMENT d, const char* where) {
    if (!d) return;
    CloseCount c;
    bool known = false;
    quint64 seq;
    {
        QMutexLocker lk(&s_traceMutex);
        auto it = s_trace.constFind(d);
        if (it != s_trace.constEnd()) { c = it.value(); known = true; }
        seq = nextSeq();
    }
    sink(seq, QStringLiteral("DOC-CLOSE-BEGIN doc=%1 tai=%2 pages=%3 texts=%4 %5 taiLieuDangSong=%6")
                   .arg(reinterpret_cast<quintptr>(d), 0, 16)
                   .arg(QString::fromLatin1(where))
                   .arg(known ? c.pages : -1)
                   .arg(known ? c.texts : -1)
                   .arg(known && (c.pages || c.texts)
                            ? QStringLiteral("<<< CON SOT: %1 page / %2 textpage CHUA DONG")
                                  .arg(c.pages).arg(c.texts)
                            : QStringLiteral("sach"))
                   .arg(liveDocs()),
         true);
}

void PdfCloseTrace::docCloseDone(FPDF_DOCUMENT d, const char* where) {
    if (!d) return;
    quint64 seq;
    {
        QMutexLocker lk(&s_traceMutex);
        s_trace.remove(d);
        s_traceDocs.remove(d);
        seq = nextSeq();
    }
    // Sau lenh FPDF_CloseDocument: neu co dong nay thi lenh do da ve; thieu dong
    // nay ⇒ chet NGAY TRONG FPDF_CloseDocument.
    sink(seq, QStringLiteral("DOC-CLOSE-DONE  doc=%1 tai=%2 (da qua FPDF_CloseDocument)")
                   .arg(reinterpret_cast<quintptr>(d), 0, 16)
                   .arg(QString::fromLatin1(where)), true);
}

void PdfCloseTrace::pageOpen(FPDF_DOCUMENT d, int pageIndex, const char* where) {
    if (!d) return;
    int total = 0;
    quint64 seq;
    {
        QMutexLocker lk(&s_traceMutex);
        total = ++s_trace[d].pages;
        seq = nextSeq();
    }
    sink(seq, QStringLiteral("PAGE-OPEN  doc=%1 page=%2 tai=%3 docPageDangMo=%4")
                   .arg(reinterpret_cast<quintptr>(d), 0, 16)
                   .arg(pageIndex)
                   .arg(QString::fromLatin1(where))
                   .arg(total), false);
}

void PdfCloseTrace::pageClose(FPDF_DOCUMENT d, FPDF_PAGE p, const char* where) {
    if (!d) return;
    int left = 0;
    quint64 seq;
    {
        QMutexLocker lk(&s_traceMutex);
        auto it = s_trace.find(d);
        if (it != s_trace.end() && it->pages > 0) { --it->pages; left = it->pages; }
        seq = nextSeq();
    }
    sink(seq, QStringLiteral("PAGE-CLOSE doc=%1 p=%2 tai=%3 conLai=%4")
                   .arg(reinterpret_cast<quintptr>(d), 0, 16)
                   .arg(reinterpret_cast<quintptr>(p), 0, 16)
                   .arg(QString::fromLatin1(where))
                   .arg(left), false);
}

void PdfCloseTrace::textOpen(FPDF_DOCUMENT d, FPDF_TEXTPAGE t, const char* where) {
    if (!d) return;
    quint64 seq;
    {
        QMutexLocker lk(&s_traceMutex);
        ++s_trace[d].texts;
        seq = nextSeq();
    }
    sink(seq, QStringLiteral("TEXT-OPEN  doc=%1 tp=%2 tai=%3")
                   .arg(reinterpret_cast<quintptr>(d), 0, 16)
                   .arg(reinterpret_cast<quintptr>(t), 0, 16)
                   .arg(QString::fromLatin1(where)), false);
}

void PdfCloseTrace::textClose(FPDF_DOCUMENT d, FPDF_TEXTPAGE t, const char* where) {
    if (!d) return;
    int left = 0;
    quint64 seq;
    {
        QMutexLocker lk(&s_traceMutex);
        auto it = s_trace.find(d);
        if (it != s_trace.end() && it->texts > 0) { --it->texts; left = it->texts; }
        seq = nextSeq();
    }
    sink(seq, QStringLiteral("TEXT-CLOSE doc=%1 tp=%2 tai=%3 conLai=%4")
                   .arg(reinterpret_cast<quintptr>(d), 0, 16)
                   .arg(reinterpret_cast<quintptr>(t), 0, 16)
                   .arg(QString::fromLatin1(where))
                   .arg(left), false);
}

void PdfCloseTrace::note(const char* kind, const QString& detail) {
    sink(nextSeq(), QStringLiteral("%1 %2").arg(QString::fromLatin1(kind), detail), true);
}

// ═══════════════════════════════════════════════════════════════════════════════
// [closeorder] LƯỢT 7 — SO DAY KHAI THAC. Xem giai thich trong PageCache.h.
// ═══════════════════════════════════════════════════════════════════════════════

void PdfCloseTrace::openPage(FPDF_DOCUMENT d, FPDF_PAGE p, int pageIndex, const char* where) {
    if (!p) return;
    const char* firstClose = nullptr;
    {
        QMutexLocker lk(&s_traceMutex);
        auto it = s_closedPage.constFind(p);
        if (it != s_closedPage.constEnd()) firstClose = it.value();
    }
    if (firstClose)
        // DUNG HANDLE DA DONG ⇒ chắc chắn se lam PDFium CHECK 0x80000003.
        sink(nextSeq(), QStringLiteral("PAGE-MO-SAU-KHI-DONG doc=%1 p=%2 page=%3 tai=%4 (lanDongDau=%5)")
                       .arg(reinterpret_cast<quintptr>(d), 0, 16)
                       .arg(reinterpret_cast<quintptr>(p), 0, 16)
                       .arg(pageIndex)
                       .arg(QString::fromLatin1(where))
                       .arg(QString::fromLatin1(firstClose)), true);
    pageOpen(d, pageIndex, where);
}

void PdfCloseTrace::openTextPage(FPDF_DOCUMENT d, FPDF_TEXTPAGE t, const char* where) {
    if (!t) return;
    const char* firstClose = nullptr;
    {
        QMutexLocker lk(&s_traceMutex);
        auto it = s_closedText.constFind(t);
        if (it != s_closedText.constEnd()) firstClose = it.value();
    }
    if (firstClose)
        sink(nextSeq(), QStringLiteral("TEXT-MO-SAU-KHI-DONG doc=%1 tp=%2 tai=%3 (lanDongDau=%4)")
                       .arg(reinterpret_cast<quintptr>(d), 0, 16)
                       .arg(reinterpret_cast<quintptr>(t), 0, 16)
                       .arg(QString::fromLatin1(where))
                       .arg(QString::fromLatin1(firstClose)), true);
    textOpen(d, t, where);
}

void PdfCloseTrace::closePage(FPDF_DOCUMENT d, FPDF_PAGE p, const char* where) {
    if (!p) return;
    const char* firstClose = nullptr;
    {
        QMutexLocker lk(&s_traceMutex);
        auto it = s_closedPage.constFind(p);
        if (it != s_closedPage.constEnd()) firstClose = it.value();
        else                                s_closedPage.insert(p, where);
    }
    int rdepth = 0;
    {
        QMutexLocker lk(&s_traceMutex);
        auto it = s_renderDepth.find(p);
        if (it != s_renderDepth.end()) rdepth = it.value().second;
    }
    pageClose(d, p, where);
    if (firstClose) {
        // ⛔ ĐÃ ĐÓNG RỒI. pdfium se CHECK 0x80000003 khi tha object lan hai — bo qua
        // lenh dong (giu bang chung, khong giet tien trinh).
        sink(nextSeq(), QStringLiteral("PAGE-CLOSE-LAI doc=%1 p=%2 tai=%3 (lanDongDau=%4) renderConTreo=%5 — BO QUA FPDF_ClosePage")
                       .arg(reinterpret_cast<quintptr>(d), 0, 16)
                       .arg(reinterpret_cast<quintptr>(p), 0, 16)
                       .arg(QString::fromLatin1(where))
                       .arg(QString::fromLatin1(firstClose))
                       .arg(rdepth), true);
        return;
    }
    if (rdepth)
        sink(nextSeq(), QStringLiteral("PAGE-CLOSE renderConTreo doc=%1 p=%2 tai=%3 soLan=%4 — thieu FPDF_RenderPage_Close")
                       .arg(reinterpret_cast<quintptr>(d), 0, 16)
                       .arg(reinterpret_cast<quintptr>(p), 0, 16)
                       .arg(QString::fromLatin1(where))
                       .arg(rdepth), true);
    FPDF_ClosePage(p);
    g_pdfiumPageClose.fetchAndAddOrdered(1);
    // ĐÃ QUA FPDF_ClosePage: thieu dong nay ⇒ chet NGAY TRONG lenh nay.
    sink(nextSeq(), QStringLiteral("PAGE-CLOSE-XONG doc=%1 p=%2 tai=%3")
                   .arg(reinterpret_cast<quintptr>(d), 0, 16)
                   .arg(reinterpret_cast<quintptr>(p), 0, 16)
                   .arg(QString::fromLatin1(where)), false);
}

void PdfCloseTrace::closeTextPage(FPDF_DOCUMENT d, FPDF_TEXTPAGE t, const char* where) {
    if (!t) return;
    const char* firstClose = nullptr;
    {
        QMutexLocker lk(&s_traceMutex);
        auto it = s_closedText.constFind(t);
        if (it != s_closedText.constEnd()) firstClose = it.value();
        else                                s_closedText.insert(t, where);
    }
    textClose(d, t, where);
    if (firstClose) {
        sink(nextSeq(), QStringLiteral("TEXT-CLOSE-LAI doc=%1 tp=%2 tai=%3 (lanDongDau=%4) — BO QUA FPDFText_ClosePage")
                       .arg(reinterpret_cast<quintptr>(d), 0, 16)
                       .arg(reinterpret_cast<quintptr>(t), 0, 16)
                       .arg(QString::fromLatin1(where))
                       .arg(QString::fromLatin1(firstClose)), true);
        return;
    }
    FPDFText_ClosePage(t);
    sink(nextSeq(), QStringLiteral("TEXT-CLOSE-XONG doc=%1 tp=%2 tai=%3")
                   .arg(reinterpret_cast<quintptr>(d), 0, 16)
                   .arg(reinterpret_cast<quintptr>(t), 0, 16)
                   .arg(QString::fromLatin1(where)), false);
}

void PdfCloseTrace::renderOpen(FPDF_DOCUMENT d, FPDF_PAGE p, const char* where) {
    if (!d || !p) return;
    int n = 0;
    {
        QMutexLocker lk(&s_traceMutex);
        auto& v = s_renderDepth[p];
        n = ++v.second;
        v.first = d;
        ++s_trace[d].renders;
    }
    sink(nextSeq(), QStringLiteral("RENDER-OPEN  doc=%1 p=%2 tai=%3 soLan=%4")
                   .arg(reinterpret_cast<quintptr>(d), 0, 16)
                   .arg(reinterpret_cast<quintptr>(p), 0, 16)
                   .arg(QString::fromLatin1(where))
                   .arg(n), false);
}
void PdfCloseTrace::renderClose(FPDF_DOCUMENT d, FPDF_PAGE p, const char* where) {
    if (!p) return;
    int n = 0;
    {
        QMutexLocker lk(&s_traceMutex);
        auto it = s_renderDepth.find(p);
        if (it == s_renderDepth.end()) n = -1;          // Close ma chua co Start
        else if (--it.value().second <= 0) s_renderDepth.erase(it);
    }
    {
        QMutexLocker lk(&s_traceMutex);
        auto it = s_trace.find(d);
        if (it != s_trace.end() && it->renders > 0) --it->renders;
    }
    sink(nextSeq(), QStringLiteral("RENDER-CLOSE doc=%1 p=%2 tai=%3 conLai=%4")
                   .arg(reinterpret_cast<quintptr>(d), 0, 16)
                   .arg(reinterpret_cast<quintptr>(p), 0, 16)
                   .arg(QString::fromLatin1(where))
                   .arg(n), n < 0);
}
int PdfCloseTrace::renderDepth(FPDF_DOCUMENT d, FPDF_PAGE p) {
    Q_UNUSED(d);
    if (!p) return 0;
    QMutexLocker lk(&s_traceMutex);
    auto it = s_renderDepth.constFind(p);
    return it == s_renderDepth.constEnd() ? 0 : it.value().second;
}

int PdfCloseTrace::pages(FPDF_DOCUMENT d) {
    if (!d) return 0;
    QMutexLocker lk(&s_traceMutex);
    auto it = s_trace.constFind(d);
    return it == s_trace.constEnd() ? -1 : it->pages;
}
int PdfCloseTrace::texts(FPDF_DOCUMENT d) {
    if (!d) return 0;
    QMutexLocker lk(&s_traceMutex);
    auto it = s_trace.constFind(d);
    return it == s_trace.constEnd() ? -1 : it->texts;
}
int PdfCloseTrace::liveDocs() { QMutexLocker lk(&s_traceMutex); return s_traceDocs.size(); }
void PdfCloseTrace::reset() {
    QMutexLocker lk(&s_traceMutex);
    s_trace.clear(); s_traceDocs.clear();
    s_closedPage.clear(); s_closedText.clear(); s_renderDepth.clear();
}

QMutex  PageCache::s_mutex;
QHash<PageCache::Key, PageCache::Entry> PageCache::s_entries;
QList<PageCache::Key>  PageCache::s_lru;
QList<QPair<PageCache::Key, PageCache::Entry>> PageCache::s_orphans;
QSet<PageCache::Key>   PageCache::s_pinned;
QList<PageCache::Key>  PageCache::s_pinOrder;
QSet<PageCache::Key>   PageCache::s_inflight;
QHash<FPDF_DOCUMENT, quint64> PageCache::s_epoch;
FPDF_DOCUMENT PageCache::s_activeDoc = nullptr;
qint64 PageCache::s_totalBytes = 0;
QSet<FPDF_DOCUMENT> PageCache::s_dead;

void PageCache::documentOpened(FPDF_DOCUMENT doc) {
    if (!doc) return;
    QMutexLocker lk(&s_mutex);
    if (s_dead.remove(doc))
        qDebug().noquote() << "[pagecache] doc MO LAI doc=" << reinterpret_cast<quintptr>(doc)
                           << "— tiep tuc phuc vu trang";
}

void PageCache::touch_locked(const Key& k) {
    s_lru.removeAll(k);
    s_lru.append(k);          // MRU o cuoi
}

void PageCache::closeEntry(Entry& e) {
    // 🔴 [closeorder] LƯỢT 7 — đây là NÚT THẮT DUY NHẤT đóng trang. Mọi thứ còn
    // bám vào FPDF_PAGE (text page, render tiến trình) phải ĐÓNG/HUỶ Ở ĐÂY TRƯỚC,
    // rồi mới FPDF_ClosePage — vì objdump trên pdfium.dll cho thấy CHECK 0x80000003
    // của lượt này là `CFX_RetainablePtr::Reset()` bắt "refcount đã bằng 0", tức
    // thả 2 lần; thả thừa 1 lần là do còn thứ giữ chặt.
    if (e.borrow > 0 || (e.page && PdfCloseTrace::renderDepth(e.doc, e.page) > 0))
        PdfCloseTrace::note("PAGE-NUT THAT",
            QStringLiteral("doc=%1 p=%2 borrow=%3 renderConTreo=%4")
                .arg(reinterpret_cast<quintptr>(e.doc), 0, 16)
                .arg(reinterpret_cast<quintptr>(e.page), 0, 16)
                .arg(e.borrow)
                .arg(e.page ? PdfCloseTrace::renderDepth(e.doc, e.page) : 0));
    if (e.tp)   PdfCloseTrace::closeTextPage(e.doc, e.tp,   "PageCache::closeEntry");
    if (e.page) PdfCloseTrace::closePage   (e.doc, e.page, "PageCache::closeEntry");
    s_totalBytes -= e.bytes;   // SPEC_PAGECACHE_THRASH_2026-08-31: giam bo nho proxy
    e = Entry();
}

void PageCache::evict_locked(const Key* keep) {
    // GIA DINH: caller giu s_pdfiumMutex + s_mutex. Dong LRU victim.
    // Thu tu duoi (SUA 2026-08-30): uu tien duoi trang cua tai lieu KHONG phai tai lieu
    // dang hien hanh (reason=inactive) truoc, roi moi toi trang cua tai lieu dang xem
    // (reason=lru) — tranh 3 tai lieu A0 duoi trang cua nhau lien tuc khi cache day.
    // Trang dang PIN hoac DANG MUON (borrow>0) thi bo qua vong duoi (giu cho toi khi
    // unpin / release). Entry "cho dong" (doomed, borrow==0) thi dong luon — no da het han.
    // keep != nullptr: trang vua nap (SPEC_PAGECACHE_THRASH_2026-08-31) — KHONG duoc duoi
    // trong luot nay, vi borrow van = 0 (acquire chua tang) ma caller sap dung handle.
    sweepOrphans_locked();                        // dong orphan da het nguoi muon truoc
    const int capacity = kCapacity;
    const auto isProtected = [&](const Key& v, auto& it) {
        if (keep && v == *keep) return true;                       // trang vua nap — miem tru
        if (s_pinned.contains(v) || it->borrow > 0) return true;   // pin / dang muon
        return false;
    };

    // Pass 1: chi duoi trang cua doc != activeDoc.
    {
        const int fullPass = s_lru.size();
        for (int i = 0; i < fullPass && s_lru.size() > capacity; ++i) {
            const Key victim = s_lru.takeFirst();
            auto it = s_entries.find(victim);
            if (it == s_entries.end()) continue;
            if (isProtected(victim, it)) {
                s_lru.append(victim);   // pin/muon/miem tru → khong duoi, de lai xuong MRU
                continue;
            }
            if (s_activeDoc && victim.first == s_activeDoc) {
                s_lru.append(victim);   // doc dang xem → de pass 2 moi duoi
                continue;
            }
            qDebug().noquote() << "[pagecache] evict doc=" << reinterpret_cast<quintptr>(victim.first)
                               << "page=" << victim.second << "reason=inactive";
            closeEntry(it.value());
            s_entries.erase(it);
        }
    }

    // Pass 2: van vuot thi moi duoi trang cua doc dang xem (theo LRU).
    {
        const int fullPass = s_lru.size();
        for (int i = 0; i < fullPass && s_lru.size() > capacity; ++i) {
            const Key victim = s_lru.takeFirst();
            auto it = s_entries.find(victim);
            if (it == s_entries.end()) continue;
            if (isProtected(victim, it)) {
                s_lru.append(victim);   // pin/muon/miem tru → khong duoi
                continue;
            }
            qDebug().noquote() << "[pagecache] evict doc=" << reinterpret_cast<quintptr>(victim.first)
                               << "page=" << victim.second << "reason=lru";
            closeEntry(it.value());
            s_entries.erase(it);
        }
    }

    // Pass 3 (SPEC_PAGECACHE_THRASH_2026-08-31): van vuot BO NHO thi duoi tiep theo LRU
    // (trang A0 raster RGBA proxy 32 MB/anh — so trang thap nhung bo nho cao).
    {
        const int fullPass = s_lru.size();
        for (int i = 0; i < fullPass && s_totalBytes > kMaxBytes; ++i) {
            const Key victim = s_lru.takeFirst();
            auto it = s_entries.find(victim);
            if (it == s_entries.end()) continue;
            if (isProtected(victim, it)) {
                s_lru.append(victim);   // pin/muon/miem tru → khong duoi
                continue;
            }
            qDebug().noquote() << "[pagecache] evict doc=" << reinterpret_cast<quintptr>(victim.first)
                               << "page=" << victim.second << "reason=mem";
            closeEntry(it.value());
            s_entries.erase(it);
        }
    }
}

void PageCache::sweepOrphans_locked() {
    // GIA DINH: caller giu s_pdfiumMutex + s_mutex. Dong cac orphan da het nguoi muon
    // (borrow == 0). Orphan con borrow>0 thi GIU NGUYEN — con nguoi dang dung handle.
    for (int i = s_orphans.size() - 1; i >= 0; --i) {
        if (s_orphans[i].second.borrow > 0) continue;
        qDebug().noquote() << "[pagecache] close orphan doc="
                           << reinterpret_cast<quintptr>(s_orphans[i].first.first)
                           << "page=" << s_orphans[i].first.second;
        closeEntry(s_orphans[i].second);
        s_orphans.removeAt(i);
    }
}

FPDF_PAGE PageCache::loadAndRegister(FPDF_DOCUMENT doc, int pageIndex) {
    // GIA DINH: caller giu s_pdfiumMutex.
    if (!doc || pageIndex < 0) return nullptr;
    {   // 🔴 LƯỢT 7: doc đã bị forgetDocument ⇒ TUỔI ĐỘNG, không được hồi sinh.
        QMutexLocker lk(&s_mutex);
        if (s_dead.contains(doc)) {
            qWarning().noquote() << "[pagecache] TUOI doc=" << reinterpret_cast<quintptr>(doc)
                                 << "page=" << pageIndex
                                 << "— doc da forgetDocument, KHONG nap lai";
            return nullptr;
        }
    }
    QElapsedTimer t; t.start();
    FPDF_PAGE page = FPDF_LoadPage(doc, pageIndex);
    if (!page) return nullptr;
    g_pdfiumPageOpen.fetchAndAddOrdered(1);
    PdfCloseTrace::openPage(doc, page, pageIndex, "PageCache::loadAndRegister");
    qDebug().noquote() << "[pagecache] LOAD page=" << pageIndex << "ms=" << t.elapsed();
    qDebug().noquote() << "[pageload] doc=" << reinterpret_cast<quintptr>(doc)
                       << "page=" << pageIndex << "by=cache ms=" << t.elapsed();

    Entry e;
    e.doc  = doc;
    e.page = page;
    e.rot  = FPDFPage_GetRotation(page) & 3;
    e.box  = pdfBoxOrigin(page);
    e.disp = QSizeF(FPDF_GetPageWidth(page), FPDF_GetPageHeight(page));
    // Uoc luong bo nho = raster RGBA (w*h*4) — proxy cho "trang nay nang bao nhieu"
    // (SPEC_PAGECACHE_THRASH_2026-08-31). A4 ~2 MB, A0 ~32 MB → tran bo nho kMaxBytes.
    e.bytes = qint64(e.disp.width()) * qint64(e.disp.height()) * 4;

    const Key k(doc, pageIndex);
    QMutexLocker lk(&s_mutex);
    s_epoch.insert(doc, s_epoch.value(doc));       // doc dang con song
    // Neu da co entry (nguoi khac nap truoc, doi khi tinh toan cham) thi giai phong cai moi.
    auto it = s_entries.find(k);
    if (it != s_entries.end()) {
        unpin_locked(k);
        if (it->borrow > 0) {
            qWarning().noquote() << "[pagecache] re-load while borrowed page=" << pageIndex
                                 << "(leak old handle)";
            s_totalBytes -= it->bytes;   // tay cu danh dau "leak" → tru bytes cu thu cong
        } else {
            closeEntry(it.value());      // closeEntry tru e.bytes cu
        }
        it.value() = e;
    }
    else                        s_entries.insert(k, e);
    s_totalBytes += e.bytes;                        // cong bytes moi (sau khi tru bytes cu)
    touch_locked(k);
    evict_locked(&k);   // 🔴 miem tru key vua nap — KHONG duoi no (SPEC_PAGECACHE_THRASH_2026-08-31)
    return page;
}

FPDF_PAGE PageCache::acquire(FPDF_DOCUMENT doc, int pageIndex) {
    if (!doc || pageIndex < 0) return nullptr;
    const Key k(doc, pageIndex);
    FPDF_PAGE page = nullptr;
    {
        QMutexLocker lk(&s_mutex);
        auto it = s_entries.find(k);
        if (it != s_entries.end()) {
            if (it->doomed) {
                if (it->borrow > 0) {
                    // Con nguoi muon (render dang tha khoa giua slice, thumbnail...):
                    // KHONG duoc dong duoi chan ho. Tach entry ra khoi map de lan sau nap
                    // MOI, nguoi muon cuoi cung khi release se dem ve 0 va sweep dong.
                    qDebug().noquote() << "[pagecache] doomed page=" << pageIndex
                                       << "con borrow=" << it->borrow
                                       << " — tach ra, KHONG dong";
                    s_orphans.push_back(qMakePair(k, it.value()));
                    s_entries.erase(it);
                    s_lru.removeAll(k);
                } else {
                    qDebug().noquote() << "[pagecache] reload doomed page=" << pageIndex;
                    closeEntry(it.value());
                    s_entries.erase(it);
                    s_lru.removeAll(k);
                }
            } else {
                touch_locked(k);
                qDebug().noquote() << "[pagecache] HIT page=" << pageIndex;
                page = it->page;
            }
        }
    }
    if (!page) page = loadAndRegister(doc, pageIndex);
    if (!page) return nullptr;
    {
        QMutexLocker lk(&s_mutex);
        auto it = s_entries.find(k);
        if (it == s_entries.end()) return nullptr;   // khong the: caller giu s_pdfiumMutex
        ++it->borrow;
        qDebug().noquote() << "[pagecache] BORROW page=" << pageIndex << "n=" << it->borrow;
    }
    return page;
}

void PageCache::release(FPDF_DOCUMENT doc, int pageIndex, FPDF_PAGE page) {
    if (!doc || pageIndex < 0) return;
    const Key k(doc, pageIndex);
    QMutexLocker lk(&s_mutex);
    if (page) {
        // Handle-aware: giam DUNG entry ma ben goi dang giu. Khi cung (doc,page) co
        // nhieu handle song song (orphan cu bi tach khoi map + entry MOI), release theo
        // (doc,pageIndex) khong phan biet duoc — phai theo handle.
        auto it = s_entries.find(k);
        if (it != s_entries.end() && it->page == page) {
            if (it->borrow > 0) --it->borrow;
            qDebug().noquote() << "[pagecache] RELEASE page=" << pageIndex << "n=" << it->borrow;
            // Doomed & het muon: de lan truy cap sau (acquire/evict) FPDF_ClosePage —
            // release() co the khong giu s_pdfiumMutex nen khong dong ngay o day.
            return;
        }
        for (int i = 0; i < s_orphans.size(); ++i) {
            if (s_orphans[i].first != k || s_orphans[i].second.page != page) continue;
            if (s_orphans[i].second.borrow > 0) --s_orphans[i].second.borrow;
            qDebug().noquote() << "[pagecache] RELEASE(orphan) page=" << pageIndex
                               << "n=" << s_orphans[i].second.borrow;
            // Het muon: sweepOrphans_locked (giu s_pdfiumMutex) se dong va lo bo.
            return;
        }
        // Handle da dong/loai bo — bo qua (borrow tuong ung da duoc xoa theo entry).
        return;
    }
    // Legacy (khong co handle): giam entry trong map theo key.
    auto it = s_entries.find(k);
    if (it == s_entries.end()) return;
    if (it->borrow > 0) --it->borrow;
    qDebug().noquote() << "[pagecache] RELEASE page=" << pageIndex << "n=" << it->borrow;
    // Neu entry dang cho dong va het nguoi muon: de duoi/acquire don dep (release co the
    // duoc goi ngoai s_pdfiumMutex nen KHONG FPDF_ClosePage o day — tranh dung con tro chet).
}

void PageCache::unpin_locked(const Key& k) {
    // GIA DINH: giu s_mutex.
    if (s_pinned.remove(k)) {
        s_pinOrder.removeAll(k);
        qDebug().noquote() << "[pagecache] UNPIN page=" << k.second;
    }
}

void PageCache::pin(FPDF_DOCUMENT doc, int pageIndex) {
    if (!doc || pageIndex < 0) return;
    const Key k(doc, pageIndex);
    QMutexLocker lk(&s_mutex);
    if (s_pinned.contains(k)) {                 // pin lai cung trang → xoa dau cu
        s_pinOrder.removeAll(k);
        s_pinOrder.append(k);
        return;
    }
    // Toi da kMaxPinned: bo pin cu nhat truoc khi pin them.
    while (s_pinOrder.size() >= kMaxPinned)
        unpin_locked(s_pinOrder.takeFirst());
    s_pinned.insert(k);
    s_pinOrder.append(k);
    qDebug().noquote() << "[pagecache] PIN page=" << pageIndex;
}

void PageCache::unpin(FPDF_DOCUMENT doc, int pageIndex) {
    if (!doc || pageIndex < 0) return;
    QMutexLocker lk(&s_mutex);
    unpin_locked(Key(doc, pageIndex));
}

void PageCache::setActiveDoc(FPDF_DOCUMENT doc) {
    QMutexLocker lk(&s_mutex);
    s_activeDoc = doc;
}

FPDF_DOCUMENT PageCache::activeDoc() {
    QMutexLocker lk(&s_mutex);
    return s_activeDoc;
}

FPDF_PAGE PageCache::tryAcquire(FPDF_DOCUMENT doc, int pageIndex) {
    if (!doc || pageIndex < 0) return nullptr;
    const Key k(doc, pageIndex);
    QMutexLocker lk(&s_mutex);
    auto it = s_entries.find(k);
    if (it == s_entries.end() || it->doomed) return nullptr;
    touch_locked(k);
    return it->page;
}

FPDF_TEXTPAGE PageCache::textPage(FPDF_DOCUMENT doc, int pageIndex) {
    // GIA DINH: caller giu s_pdfiumMutex. Trang phai da co trong dem.
    // 0927 LƯỢT 10 (--no-textpage): KHONG tao FPDF_TEXTPAGE cho TAI LIEU CHINH ⇒
    // TextSelection (TextSelection::pageFor), MainWindow::pageHasTextSync va
    // OcrPanel::ensureHasTextKnown deu nhan nullptr. KHONG vao vong khoa s_mutex,
    // KHONG goi FPDFText_LoadPage ⇒ khong sinh them mot doi tuong dem-tham-chieu.
    if (trNoTextPage()) return nullptr;
    const Key k(doc, pageIndex);
    FPDF_TEXTPAGE tp;
    {
        QMutexLocker lk(&s_mutex);
        auto it = s_entries.find(k);
        if (it == s_entries.end() || it->doomed) return nullptr;
        if (it->tp) return it->tp;
        tp = FPDFText_LoadPage(it->page);
        it->tp = tp;
        PdfCloseTrace::openTextPage(doc, tp, "PageCache::textPage");
        return tp;
    }
}

FPDF_TEXTPAGE PageCache::tryAcquireTextPage(FPDF_DOCUMENT doc, int pageIndex) {
    if (!doc || pageIndex < 0) return nullptr;
    const Key k(doc, pageIndex);
    QMutexLocker lk(&s_mutex);
    auto it = s_entries.find(k);
    if (it == s_entries.end() || !it->tp || it->doomed) return nullptr;
    return it->tp;
}

bool PageCache::metaFor(FPDF_DOCUMENT doc, int pageIndex, PageMeta& out) {
    if (!doc || pageIndex < 0) return false;
    const Key k(doc, pageIndex);
    QMutexLocker lk(&s_mutex);
    auto it = s_entries.find(k);
    if (it == s_entries.end() || it->doomed) return false;
    out.rot = it->rot; out.box = it->box; out.disp = it->disp;
    return true;
}

void PageCache::prefetch(FPDF_DOCUMENT doc, int pageIndex) {
    if (!doc || pageIndex < 0) return;
    // 🔴 LƯỢT 33b (J — tab nặng chặn tab mới): tab NỀN không được mượn khoá PDFium
    // để prefetch — loadAndRegister giữ s_pdfiumMutex 1–2 s/trang nặng, chặn đúng
    // lúc tab vừa mở cần FPDF_LoadMemDocument + trang đầu. Tab nền đứng lại; khi nó
    // thành tab hiện hành, onPageChanged/schedulePagePrefetch lại nạp tiếp.
    {
        const FPDF_DOCUMENT a = activeDoc();
        // 🔴 LƯỢT 33d (J): thêm nhánh null-activeDoc + khẩn đang chờ — khoảng trống
        // tab mới đang mở (raw=null tới lúc open xong) là lúc cần khoá nhất; đo
        // r33b: nền prefetch lọt qua chốt cũ đúng cửa sổ này.
        if ((a && doc != a) || (!a && pdfiumUrgentPending())) return;
        // 🔴 LƯỢT 33e (mục 5 — J): cửa sổ "trang đầu đang chờ ảnh" (g_pdfiumUrgentPage
        // do mở tab đặt, xóa lúc ACCEPT) — prefetch trang KHÔNG phải trang khẩn đứng
        // lại lượt này. Đo r33d: lượt LoadPage của prefetch chen giữa cửa sổ f2 vừa
        // mở xong, đúng lúc raster trang 0 của f2 cần khoá.
        const int up = g_pdfiumUrgentPage().load(std::memory_order_acquire);
        if (up >= 0 && pageIndex != up && QDateTime::currentMSecsSinceEpoch()
            <= g_pdfiumUrgentPageDeadline().load(std::memory_order_acquire)) return;
    }
    const Key k(doc, pageIndex);
    quint64 capturedEpoch = 0;
    {
        QMutexLocker lk(&s_mutex);
        if (s_entries.contains(k)) return;        // da co
        if (s_inflight.contains(k)) return;       // dang nap — khong xep lan hai
        if (!s_epoch.contains(doc)) s_epoch.insert(doc, 0);
        capturedEpoch = s_epoch.value(doc);
        s_inflight.insert(k);
    }
    // 🔴 0928 LƯỢT 22 (reviewer mục 2): token ĐĂNG KÝ LÚC SPAWN (thread gọi
    // prefetch — thường là UI) rồi RAII move vào lambda. Bản cũ đăng ký trong
    // thân task ⇒ beginClose thấy 0 khi task còn xếp hàng ⇒ doc đóng, prefetch
    // chạy với `doc` đã free.
    trdoc::Task task(doc, "PageCache::prefetch");
    // 0928 LƯỢT 14: prefetch là task nền mượn trang của doc qua
    // loadAndRegister() nhưng QFuture bị bỏ rơi ⇒ không chờ được. Token sổ
    // đảm nhiệm: close() chờ nó về 0 trước FPDF_CloseDocument.
    QtConcurrent::run([task = std::move(task), doc, pageIndex, capturedEpoch] {
        TimedPdfiumLock pdf(__FILE__, __LINE__);
        {
            QMutexLocker lk(&PageCache::s_mutex);
            const Key k(doc, pageIndex);
            // Doc da bi dong/mo lai giua chung → bo (khong cham con tro da free).
            if (!PageCache::s_epoch.contains(doc)
                || PageCache::s_epoch.value(doc) != capturedEpoch) {
                PageCache::s_inflight.remove(k);
                return;
            }
            // Ai do (acquire) da nap xong khi ta dang cho mutex → bo.
            if (PageCache::s_entries.contains(k)) {
                PageCache::s_inflight.remove(k);
                return;
            }
        }
        PageCache::loadAndRegister(doc, pageIndex);
        {
            QMutexLocker lk(&PageCache::s_mutex);
            PageCache::s_inflight.remove(Key(doc, pageIndex));
        }
    });
}

qint64 PageCache::totalBytes() { QMutexLocker lk(&s_mutex); return s_totalBytes; }
int    PageCache::entryCount() { QMutexLocker lk(&s_mutex); return s_entries.size(); }

void PageCache::invalidate(FPDF_DOCUMENT doc, int pageIndex) {
    if (!doc || pageIndex < 0) return;
    const Key k(doc, pageIndex);
    QMutexLocker lk(&s_mutex);
    auto it = s_entries.find(k);
    if (it == s_entries.end()) return;
    qDebug().noquote() << "[pagecache] invalidate doc=" << reinterpret_cast<quintptr>(doc)
                       << "page=" << pageIndex;
    unpin_locked(k);                  // entry bi dong thi khong con giu pin
    if (it->borrow > 0) {
        // Dang co nguoi muon (render task tha khoa giua slice, thumbnail...) — chi danh
        // dau "cho dong"; release() khi borrow ve 0 va luot duoi/acquire ke se FPDF_ClosePage.
        it->doomed = true;
        qDebug().noquote() << "[pagecache] invalidate deferred (borrowed) page=" << pageIndex;
        return;
    }
    closeEntry(it.value());
    s_entries.erase(it);
    s_lru.removeAll(k);
}

void PageCache::bumpAnnotGeneration(FPDF_DOCUMENT doc, int pageIndex) {
    // Markup them/xoa/sua annot (SPEC_MARKUP_FIX_2026-08-31): FPDF_PAGE trong cache
    // vẫn hợp lệ và đã phản ánh thay đổi (CreateAnnot/RemoveAnnot sua truc tiep doi
    // tuong trang). Chi tang the he annot de cache dan xuat (annot list, visuals,
    // raster) biet noi dung da doi — KHONG dong handle, KHONG doomed.
    if (!doc || pageIndex < 0) return;
    const Key k(doc, pageIndex);
    QMutexLocker lk(&s_mutex);
    auto it = s_entries.find(k);
    if (it == s_entries.end()) return;   // trang chua nap — khong co gi de danh dau
    ++it->annotGen;
    qDebug().noquote() << "[pagecache] bumpAnnotGeneration doc=" << reinterpret_cast<quintptr>(doc)
                       << "page=" << pageIndex << "gen=" << it->annotGen;
}

quint64 PageCache::annotGeneration(FPDF_DOCUMENT doc, int pageIndex) {
    if (!doc || pageIndex < 0) return 0;
    QMutexLocker lk(&s_mutex);
    auto it = s_entries.find(Key(doc, pageIndex));
    return it == s_entries.end() ? 0 : it->annotGen;
}

void PageCache::forgetDocument(FPDF_DOCUMENT doc) {
    if (!doc) return;
    int closed = 0;
    QMutexLocker lk(&s_mutex);
    s_epoch.remove(doc);
    // 🔴 LƯỢT 7: đánh dấu "doc đã bị dọn" — acquire/loadAndRegister/prefetch từ
    // đây trở đi KHÔNG được nạp trang của doc này nữa (xem s_dead ở PageCache.h).
    s_dead.insert(doc);
    // Go pin cua doc nay TRUOC (R1 muc 5)— khong giu con tro sau khi dong handle.
    for (auto it = s_pinOrder.begin(); it != s_pinOrder.end(); ) {
        if (it->first == doc) it = s_pinOrder.erase(it);
        else ++it;
    }
    for (auto it = s_pinned.begin(); it != s_pinned.end(); ) {
        if (it->first == doc) it = s_pinned.erase(it);
        else ++it;
    }
    // Lọc entry thuoc doc nay (so entry <= kCapacity, quet O(n) la du).
    for (auto it = s_entries.begin(); it != s_entries.end(); ) {
        if (it.key().first == doc) {
            if (it->borrow > 0) {
                // Vẫn con nguoi muon — chi danh dau cho dong, dong khi borrow ve 0.
                it->doomed = true;
                ++it;
                continue;
            }
            closeEntry(it.value());
            s_lru.removeAll(it.key());
            it = s_entries.erase(it);
            ++closed;
        } else ++it;
    }
    for (auto it = s_inflight.begin(); it != s_inflight.end(); ) {
        if (it->first == doc) it = s_inflight.erase(it);
        else ++it;
    }
    // Orphan cua doc nay (da tach khoi map khi doomed + con borrow): dong het borrow==0
    // TRUOC khi caller FPDF_CloseDocument — khong dong con borrow>0 (nguoi muon con dung).
    for (int i = s_orphans.size() - 1; i >= 0; --i) {
        if (s_orphans[i].first.first != doc) continue;
        if (s_orphans[i].second.borrow > 0) {
            qWarning().noquote() << "[pagecache] forgetDocument: orphan con borrow>0 doc="
                                 << reinterpret_cast<quintptr>(doc)
                                 << "page=" << s_orphans[i].first.second;
            continue;
        }
        closeEntry(s_orphans[i].second);
        s_orphans.removeAt(i);
        ++closed;
    }
    qDebug().noquote() << "[pagecache] forgetDocument doc=" << reinterpret_cast<quintptr>(doc)
                       << "entries=" << closed;
    // [closeorder] 0927 LƯỢT 2: cùng thông tin nhưng vào FILE BANG CHUNG + có tid.
    // `conMuonLon=` > 0 ⇒ còn người đang giữ FPDF_PAGE của doc này lúc
    // FPDF_CloseDocument chạy ⇒ đúng cái CHECK 0x80000003.
    PdfCloseTrace::note("FORGET-DOC",
        QStringLiteral("doc=%1 daDong=%2 conMuonLon=%3 taiLieuDangSong=%4")
            .arg(reinterpret_cast<quintptr>(doc), 0, 16)
            .arg(closed)
            .arg(PdfCloseTrace::pages(doc))
            .arg(PdfCloseTrace::liveDocs()));
}

int PageCache::size() {
    QMutexLocker lk(&s_mutex);
    return s_entries.size();
}

// 🔴 LƯỢT 27 (săn rò): doc so handle còn SỐNG trong PageCache mà KHONG cham
// s_pdfiumMutex. doomed + orphan > 0 sau khi docMo=0 ⇒ FPDF_PAGE của tab đã đóng
// chưa được FPDF_ClosePage (ro handle). deadDocs = số doc đã forgetDocument.
int PageCache::doomedCount() {
    QMutexLocker lk(&s_mutex);
    int n = 0;
    for (const Entry& e : s_entries) if (e.doomed) ++n;
    return n;
}
int PageCache::orphanCount() {
    QMutexLocker lk(&s_mutex);
    return s_orphans.size();
}
int PageCache::deadDocCount() {
    QMutexLocker lk(&s_mutex);
    return s_dead.size();
}
