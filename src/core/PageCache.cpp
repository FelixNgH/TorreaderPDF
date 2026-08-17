#include "PageCache.h"
#include "PdfCoords.h"
#include <fpdf_edit.h>
#include <QMutexLocker>
#include <QtConcurrent>
#include <QElapsedTimer>
#include <QDebug>

extern QMutex s_pdfiumMutex;

QMutex  PageCache::s_mutex;
QHash<PageCache::Key, PageCache::Entry> PageCache::s_entries;
QList<PageCache::Key>  PageCache::s_lru;
QSet<PageCache::Key>   PageCache::s_inflight;
QHash<FPDF_DOCUMENT, quint64> PageCache::s_epoch;

void PageCache::touch_locked(const Key& k) {
    s_lru.removeAll(k);
    s_lru.append(k);          // MRU o cuoi
}

void PageCache::closeEntry(Entry& e) {
    if (e.tp)   FPDFText_ClosePage(e.tp);
    if (e.page) FPDF_ClosePage(e.page);
    e = Entry();
}

void PageCache::evict_locked() {
    // GIA DINH: caller giu s_pdfiumMutex + s_mutex. Dong LRU victim.
    while (s_lru.size() > kCapacity) {
        const Key victim = s_lru.takeFirst();
        auto it = s_entries.find(victim);
        if (it == s_entries.end()) continue;
        qDebug().noquote() << "[pagecache] evict doc=" << reinterpret_cast<quintptr>(victim.first)
                           << "page=" << victim.second;
        closeEntry(it.value());
        s_entries.erase(it);
    }
}

FPDF_PAGE PageCache::loadAndRegister(FPDF_DOCUMENT doc, int pageIndex) {
    // GIA DINH: caller giu s_pdfiumMutex.
    if (!doc || pageIndex < 0) return nullptr;
    QElapsedTimer t; t.start();
    FPDF_PAGE page = FPDF_LoadPage(doc, pageIndex);
    if (!page) return nullptr;
    qDebug().noquote() << "[pageload] doc=" << reinterpret_cast<quintptr>(doc)
                       << "page=" << pageIndex << "by=cache ms=" << t.elapsed();

    Entry e;
    e.page = page;
    e.rot  = FPDFPage_GetRotation(page) & 3;
    e.box  = pdfBoxOrigin(page);
    e.disp = QSizeF(FPDF_GetPageWidth(page), FPDF_GetPageHeight(page));

    const Key k(doc, pageIndex);
    QMutexLocker lk(&s_mutex);
    s_epoch.insert(doc, s_epoch.value(doc));       // doc dang con song
    // Neu da co entry (nhuoi khac nap truoc, doi khi tinh toan cham) thi giai phong cai moi.
    auto it = s_entries.find(k);
    if (it != s_entries.end()) { closeEntry(it.value()); it.value() = e; }
    else                        s_entries.insert(k, e);
    touch_locked(k);
    evict_locked();                                // FPDF_ClosePage victim — s_pdfiumMutex dang giu
    return page;
}

FPDF_PAGE PageCache::acquire(FPDF_DOCUMENT doc, int pageIndex) {
    if (!doc || pageIndex < 0) return nullptr;
    const Key k(doc, pageIndex);
    {
        QMutexLocker lk(&s_mutex);
        auto it = s_entries.find(k);
        if (it != s_entries.end()) { touch_locked(k); return it->page; }
    }
    return loadAndRegister(doc, pageIndex);
}

FPDF_PAGE PageCache::tryAcquire(FPDF_DOCUMENT doc, int pageIndex) {
    if (!doc || pageIndex < 0) return nullptr;
    const Key k(doc, pageIndex);
    QMutexLocker lk(&s_mutex);
    auto it = s_entries.find(k);
    if (it == s_entries.end()) return nullptr;
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
        if (it == s_entries.end()) return nullptr;
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
    if (it == s_entries.end() || !it->tp) return nullptr;
    return it->tp;
}

bool PageCache::metaFor(FPDF_DOCUMENT doc, int pageIndex, PageMeta& out) {
    if (!doc || pageIndex < 0) return false;
    const Key k(doc, pageIndex);
    QMutexLocker lk(&s_mutex);
    auto it = s_entries.find(k);
    if (it == s_entries.end()) return false;
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
        QMutexLocker pdf(&s_pdfiumMutex);
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

void PageCache::invalidate(FPDF_DOCUMENT doc, int pageIndex) {
    if (!doc || pageIndex < 0) return;
    const Key k(doc, pageIndex);
    QMutexLocker lk(&s_mutex);
    auto it = s_entries.find(k);
    if (it == s_entries.end()) return;
    qDebug().noquote() << "[pagecache] invalidate doc=" << reinterpret_cast<quintptr>(doc)
                       << "page=" << pageIndex;
    closeEntry(it.value());
    s_entries.erase(it);
    s_lru.removeAll(k);
}

void PageCache::forgetDocument(FPDF_DOCUMENT doc) {
    if (!doc) return;
    int closed = 0;
    QMutexLocker lk(&s_mutex);
    s_epoch.remove(doc);
    // Lọc entry thuoc doc nay (so entry <= kCapacity, quet O(n) la du).
    for (auto it = s_entries.begin(); it != s_entries.end(); ) {
        if (it.key().first == doc) {
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
    qDebug().noquote() << "[pagecache] forgetDocument doc=" << reinterpret_cast<quintptr>(doc)
                       << "entries=" << closed;
}

int PageCache::size() {
    QMutexLocker lk(&s_mutex);
    return s_entries.size();
}
