#include "PageCache.h"
#include "PdfCoords.h"
#include "PdfiumLock.h"
#include <fpdf_edit.h>
#include <QMutexLocker>
#include <QtConcurrent>
#include <QElapsedTimer>
#include <QDebug>

extern QMutex s_pdfiumMutex;

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

void PageCache::touch_locked(const Key& k) {
    s_lru.removeAll(k);
    s_lru.append(k);          // MRU o cuoi
}

void PageCache::closeEntry(Entry& e) {
    if (e.tp)   FPDFText_ClosePage(e.tp);
    if (e.page) { FPDF_ClosePage(e.page); g_pdfiumPageClose.fetchAndAddOrdered(1); }
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
    QElapsedTimer t; t.start();
    FPDF_PAGE page = FPDF_LoadPage(doc, pageIndex);
    if (!page) return nullptr;
    g_pdfiumPageOpen.fetchAndAddOrdered(1);
    qDebug().noquote() << "[pagecache] LOAD page=" << pageIndex << "ms=" << t.elapsed();
    qDebug().noquote() << "[pageload] doc=" << reinterpret_cast<quintptr>(doc)
                       << "page=" << pageIndex << "by=cache ms=" << t.elapsed();

    Entry e;
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
    const Key k(doc, pageIndex);
    FPDF_TEXTPAGE tp;
    {
        QMutexLocker lk(&s_mutex);
        auto it = s_entries.find(k);
        if (it == s_entries.end() || it->doomed) return nullptr;
        if (it->tp) return it->tp;
        tp = FPDFText_LoadPage(it->page);
        it->tp = tp;
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
    QtConcurrent::run([doc, pageIndex, capturedEpoch] {
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
}

int PageCache::size() {
    QMutexLocker lk(&s_mutex);
    return s_entries.size();
}
