#include "PdfDocument.h"
#include "OcrTextLayer.h"
#include "PageCache.h"
#include "PdfiumLock.h"
#include "DocTaskGate.h"
#include <QMutex>
#include <QDebug>
#include <fpdf_text.h>
#include <fpdf_transformpage.h>

#ifndef _WIN32
#include <QFile>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>
#endif

// Serializes FPDF_LoadCustomDocument / FPDF_LoadDocument / FPDF_CloseDocument
// (PDFium document-level operations have internal global state).
QMutex s_pdfiumMutex;
QAtomicInteger<int> g_pdfiumDocOpen{0},  g_pdfiumDocClose{0};
QAtomicInteger<int> g_pdfiumPoolOpen{0}, g_pdfiumPoolClose{0};
QAtomicInteger<int> g_pdfiumPageOpen{0}, g_pdfiumPageClose{0};
QAtomicInteger<qint64> g_gpuTexBytes{0};
QAtomicInteger<int>    g_gpuTexAlive{0};

// PDFium requires one-time library init. Guard with static + mutex.
static QMutex s_initMutex;
static int s_refCount = 0;

static void ensureInit() {
    QMutexLocker lock(&s_initMutex);
    if (s_refCount++ == 0) {
        FPDF_LIBRARY_CONFIG config{};
        config.version          = 2;
        config.m_pUserFontPaths = nullptr;
        config.m_pIsolate       = nullptr;
        config.m_v8EmbedderSlot = 0;
        FPDF_InitLibraryWithConfig(&config);
    }
}

QMutex& PdfDocument::pdfiumGlobalMutex() { return s_pdfiumMutex; }

static void ensureDestroy() {
    QMutexLocker lock(&s_initMutex);
    if (--s_refCount != 0) return;
    // 0927 LƯỢT 8: trả về gọi THẬT. Lượt 2 đã bỏ qua `FPDF_DestroyLibrary` khi còn
    // tài liệu sống để né CHECK — nhưng đó là giấu lỗi bằng cách không dọn: 12 doc của
    // pool cùng buffer mmap bị bỏ rơi mỗi lần đóng tab, và thư viện không bao giờ được
    // hủy. Nay thứ tự teardown đã đúng (MainWindow::shutdownTab → ~PdfRenderer đóng doc
    // pool → ~PdfDocument đóng doc chính → unmap) nên tới đây phải luôn là 0.
    const int live = PdfCloseTrace::liveDocs();
    if (live > 0)
        PdfCloseTrace::note("LIB-DESTROY-CON",
            QStringLiteral("conTaiLieuSong=%1 — xem POOL-SKIP / FORGET-DOC truoc do").arg(live));
    PdfCloseTrace::note("LIB-DESTROY", QStringLiteral("bat dau FPDF_DestroyLibrary"));
    FPDF_DestroyLibrary();
}

void PdfDocument::libAddRef()  { ensureInit(); }
void PdfDocument::libRelease() { ensureDestroy(); }

// 0928 LƯỢT 25 — THÍ NGHIỆM (TORREADER_REINIT_IDLE=1, mac dinh TAT):
// Hủy + khởi động lại PDFium khi khong con tai lieu nao mo, de do xem state toan
// cuc cua PDFium (CPDF_PageModule, font/glyph cache...) co phinh to qua cac lap
// khong (lap 2 docMo=0 RSS 444-624MB vs 106MB lap 1). Goi tren LUONG GIAO DIEN,
// duoi s_pdfiumMutex. An toan ve handle: PageCache::forgetDocument +
// OcrTextLayer::forgetDocument + pool docs dong theo tai lieu, nen khi
// refCount==1 khong con FPDF_DOCUMENT/FPDF_PAGE/FPDF_FONT song ngoai tru
// m_orphanPoolDocs (duoc ensureDestroy dong ho) — do la cung duong app thoat
// van chay hang ngay, nay chi la chay som khi rảnh.
bool PdfDocument::libReinitIdle() {
    QMutexLocker pdfium(&s_pdfiumMutex);
    QMutexLocker lock(&s_initMutex);
    // refCount==0 => thu vien DA bi ensureDestroy huy (khong co gi de do).
    // >0 => van con song (day la truong hop GUI: luon con 1 vo PdfDocument dong
    // nen refCount khong bao gio ve 0, DestroyLibrary khong bao gio chay giua cac
    // lap => state toan cuc phinh). Force destroy+init, GIU NGUYEN refCount.
    if (s_refCount < 1) return false;
    FPDF_DestroyLibrary();
    FPDF_LIBRARY_CONFIG config{};
    config.version          = 2;
    config.m_pUserFontPaths = nullptr;
    config.m_pIsolate       = nullptr;
    config.m_v8EmbedderSlot = 0;
    FPDF_InitLibraryWithConfig(&config);
    return true;
}

PdfDocument::PdfDocument() { ensureInit(); }

PdfDocument::~PdfDocument() {
    close();
    ensureDestroy();
}

// ── open ──────────────────────────────────────────────────────────────────────
bool PdfDocument::open(const QString& filePath, const QString& password) {
    close();
    QByteArray pwd = password.toUtf8();

#ifdef _WIN32
    // Prefer memory-mapped loading: PDFium reads data through the OS page cache
    // rather than allocating the whole file in heap. Typical saving: 10-20x RAM.
    std::wstring wpath = filePath.toStdWString();
    // FILE_SHARE_WRITE is required so AnnotationManager can open the same file
    // for writing (FPDF_SaveAsCopy) while this read-only mmap is still active.
    // FILE_SHARE_DELETE only unlocks rename/move in Explorer; delete is still
    // blocked while the file stays memory-mapped.
    m_fileHandle = CreateFileW(wpath.c_str(), GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (m_fileHandle != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER sz{};
        if (GetFileSizeEx(m_fileHandle, &sz) && sz.QuadPart > 0
            && sz.QuadPart <= static_cast<LONGLONG>(ULONG_MAX))
        {
            m_mapHandle = CreateFileMappingW(m_fileHandle, nullptr,
                                             PAGE_READONLY, 0, 0, nullptr);
            if (m_mapHandle) {
                m_mapView = MapViewOfFile(m_mapHandle, FILE_MAP_READ, 0, 0, 0);
                if (m_mapView) {
                    m_fileSize = sz.QuadPart;
                    {
                        BoundedPdfiumLock lock(__FILE__, __LINE__);
                        m_doc = FPDF_LoadMemDocument(m_mapView, static_cast<int>(sz.QuadPart),
                                                     pwd.isEmpty() ? nullptr : pwd.constData()); g_pdfiumDocOpen.fetchAndAddOrdered(1);
                        if (m_doc) PdfCloseTrace::docOpen(m_doc, "PdfDocument::open");
                        if (m_doc) {
                            m_pageCount = FPDF_GetPageCount(m_doc);
                            m_pageSizes.resize(m_pageCount);
                            for (int i = 0; i < m_pageCount; ++i) {
                                double w = 0, h = 0;
                                FPDF_GetPageSizeByIndex(m_doc, i, &w, &h);
                                m_pageSizes[i] = {w, h};
                            }
                            m_pageBoxOrigins.fill(QPointF(0.0, 0.0), m_pageCount);
                            m_pageBoxKnown.fill(false, m_pageCount);
                        }
                    }
                    if (m_doc) {
                        m_filePath = filePath;
                        return true;
                    }
                    // PDFium rejected the file; fall through to regular load
                    UnmapViewOfFile(m_mapView); m_mapView   = nullptr;
                }
                CloseHandle(m_mapHandle); m_mapHandle = nullptr;
            }
        }
        CloseHandle(m_fileHandle); m_fileHandle = INVALID_HANDLE_VALUE;
        m_fileSize = 0;
    }
#endif

#ifndef _WIN32
    // Linux mmap path: map the file, then open via FPDF_LoadMemDocument so
    // PdfRenderer's doc pool can create additional FPDF_DOCUMENT handles from
    // the same mapped memory — enabling true parallel rendering.
    m_fileFd = ::open(QFile::encodeName(filePath).constData(), O_RDONLY);
    if (m_fileFd >= 0) {
        struct stat st;
        if (::fstat(m_fileFd, &st) == 0 && st.st_size > 0) {
            m_mapView = ::mmap(nullptr, static_cast<size_t>(st.st_size),
                               PROT_READ, MAP_SHARED, m_fileFd, 0);
            if (m_mapView != MAP_FAILED) {
                m_fileSize = static_cast<unsigned long>(st.st_size);
                {
                    BoundedPdfiumLock lock(__FILE__, __LINE__);
                    m_doc = FPDF_LoadMemDocument(m_mapView, static_cast<int>(st.st_size),
                                                 pwd.isEmpty() ? nullptr : pwd.constData()); g_pdfiumDocOpen.fetchAndAddOrdered(1);
                    if (m_doc) PdfCloseTrace::docOpen(m_doc, "PdfDocument::open");
                    if (m_doc) {
                        m_pageCount = FPDF_GetPageCount(m_doc);
                        m_pageSizes.resize(m_pageCount);
                        for (int i = 0; i < m_pageCount; ++i) {
                            double w = 0, h = 0;
                            FPDF_GetPageSizeByIndex(m_doc, i, &w, &h);
                            m_pageSizes[i] = {w, h};
                        }
                        m_pageBoxOrigins.fill(QPointF(0.0, 0.0), m_pageCount);
                        m_pageBoxKnown.fill(false, m_pageCount);
                    }
                }
                if (m_doc) {
                    m_filePath = filePath;
                    return true;
                }
                // PDFium rejected the file; fall through
                ::munmap(m_mapView, m_fileSize);
                m_mapView = nullptr;
                m_fileSize = 0;
            }
        }
        ::close(m_fileFd);
        m_fileFd = -1;
    }
#endif

    // Fallback: FPDF_LoadDocument (loads entire file into PDFium heap)
    QByteArray pathUtf8 = filePath.toUtf8();
    {
        BoundedPdfiumLock lock(__FILE__, __LINE__);
        m_doc = FPDF_LoadDocument(pathUtf8.constData(),
                                   pwd.isEmpty() ? nullptr : pwd.constData()); g_pdfiumDocOpen.fetchAndAddOrdered(1);
        if (m_doc) PdfCloseTrace::docOpen(m_doc, "PdfDocument::open");
        if (m_doc) {
            m_pageCount = FPDF_GetPageCount(m_doc);
            m_pageSizes.resize(m_pageCount);
            for (int i = 0; i < m_pageCount; ++i) {
                double w = 0, h = 0;
                FPDF_GetPageSizeByIndex(m_doc, i, &w, &h);
                m_pageSizes[i] = {w, h};
            }
            m_pageBoxOrigins.fill(QPointF(0.0, 0.0), m_pageCount);
            m_pageBoxKnown.fill(false, m_pageCount);
        }
    }
    if (!m_doc) return false;
    m_filePath = filePath;
    return true;
}

// ── close ─────────────────────────────────────────────────────────────────────
void PdfDocument::close() {
    // 🔴🔴 0928 LƯỢT 14 — CHỜ VIỆC NỀN XONG TRƯỚC KHI ĐÓNG (vá 4/4 minidump).
    // Bằng chứng: 4/4 minidump chết trên luồng QtConcurrent trong
    // `VectorLayer::build`, và log mọi lượt crash cho thấy
    //   `DOC-CLOSE-BEGIN … pages=1 texts=1 <<< CON SOT`  rồi SAU ĐÓ vẫn
    //   `[khoa] giu ms=… tai=VectorLayer::build`
    // ⇒ lúc FPDF_CloseDocument chạy, task nền vẫn còn giữ PageBorrow của doc.
    //
    // ⛔ GỌI TRƯỚC `BoundedPdfiumLock` — KHÔNG được giữ s_pdfiumMutex lúc chờ:
    //    task cần chính khoá đó để nhìn thấy cờ huỷ, giữ khoá = tự khoá chết.
    // (a) beginClose() bật cờ huỷ ⇒ task dừng ở ranh giới lát kế tiếp (≤ 40 ms).
    // (b) rồi chờ có trần (3 s). Quá trần ⇒ trả về, KHÔNG đóng doc: rò bộ nhớ
    //     còn hơn đọc vùng đã free (đây là 4/4 minidump).
    if (m_doc) {
        constexpr int kWaitMs = 3000;
        if (!trdoc::beginClose(m_doc, kWaitMs, "PdfDocument::close")) {
            // CỐ TÌNH KHÔNG đóng. Giữ m_doc, giữ mmap, giữ handle pool — tài liệu
            // vẫn sống, chỉ rò tới khi tiến trình thoát. KHÔNG quay lại FPDF_
            // _CloseDocument: đó chính là chỗ sinh crash.
            return;
        }
    }
    BoundedPdfiumLock lock(__FILE__, __LINE__);
    if (m_doc) {
        OcrTextLayer::forgetDocument(m_doc);
        // 🔴 PageCache giu FPDF_PAGE cua doc — phai xoa TRUOC FPDF_CloseDocument
        //    neu khong con tro chet (SPEC_PAGECACHE_CORE muc 2).
        PageCache::forgetDocument(m_doc);
        // [closeorder] 0927: in con tro + so page/textpage/annot CON SOT cua doc nay ngay
        // TRUOC FPDF_CloseDocument. `pages=0 texts=0 annots=0` = sach (an toan). >0 =
        // tai lieu bi pha trong khi con handle muon => chinh la nguon CHECK 0x80000003.
        // LƯỢT 2: tach -BEGIN (truoc) va -DONE (sau) => biet chet O TRONG lenh hay sau.
        PdfCloseTrace::docCloseBegin(m_doc, "PdfDocument::close");
        // 🔴 LƯỢT 14: sau khi `forgetDocument` mà VẪN còn handle mượn ⇒ đã còn ai
        // đó giữ trang/textpage mà SỔ VIỆC NỀN không bắt được (task mở thô FPDF_LoadPage
        // ngoài PageCache, hoặc thread khác ngoài QtConcurrent). Ghi rõ tên lỗi thay
        // vì chỉ hy vọng ai đó đọc trường `pages=` trong log [closeorder].
        const int pLeft = PdfCloseTrace::pages(m_doc);
        const int tLeft = PdfCloseTrace::texts(m_doc);
        if (pLeft > 0 || tLeft > 0)
            qWarning().noquote() << "[dongdoc] LOI con muon trang pages=" << pLeft
                                 << " texts=" << tLeft << " tai=PdfDocument::close";
        FPDF_CloseDocument(m_doc); g_pdfiumDocClose.fetchAndAddOrdered(1);
        PdfCloseTrace::docCloseDone(m_doc, "PdfDocument::close");
        // 0928 LƯỢT 14: xoá sổ việc nền + CỜ HUỶ của doc vừa đóng. Bắt buộc: cùng
        // địa chỉ FPDF_DOCUMENT có thể được cấp lại ngay cho lần mở sau (open()
        // gọi close() rồi load lại), và cờ huỷ sót lại sẽ giết ngay mọi task của
        // lần mở mới.
        trdoc::forget(m_doc);
        m_doc = nullptr;
        m_filePath.clear();
        m_pageCount = 0;
        m_pageSizes.clear();
        m_pageBoxOrigins.clear();
        m_pageBoxKnown.clear();
    }
    // [closeorder] 0927: MMAP — bao nhieu FPDF_DOCUMENT con SONG (doc do pool cua
    // PdfRenderer mo bang FPDF_LoadMemDocument tren cung m_doc->mmapData()). Neu
    // con >0 thi bo dem o duoi se thanh vung ma pool doc chua FPDF_CloseDocument.
    if (m_mapView)
        PdfCloseTrace::note("MMAP-UNMAP",
            QStringLiteral("tai=PdfDocument::close taiLieuConSongChungBuffer=%1").arg(PdfCloseTrace::liveDocs()));
#ifdef _WIN32
    if (m_mapView)  { UnmapViewOfFile(m_mapView);  m_mapView  = nullptr; }
    if (m_mapHandle){ CloseHandle(m_mapHandle);    m_mapHandle = nullptr; }
    if (m_fileHandle != INVALID_HANDLE_VALUE) {
        CloseHandle(m_fileHandle);
        m_fileHandle = INVALID_HANDLE_VALUE;
    }
#else
    if (m_mapView)  { ::munmap(m_mapView, m_fileSize); m_mapView  = nullptr; }
    if (m_fileFd >= 0) { ::close(m_fileFd); m_fileFd = -1; }
#endif
    m_fileSize = 0;
}

// ── Metadata helpers ──────────────────────────────────────────────────────────

int PdfDocument::pageCount() const { return m_pageCount; }

QSizeF PdfDocument::pageSize(int pageIndex) const {
    QMutexLocker lk(&m_sizesMutex);
    if (pageIndex < 0 || pageIndex >= m_pageSizes.size()) {
        qDebug() << "[PdfDoc] pageSize(" << pageIndex << ") OUT OF RANGE, total=" << m_pageSizes.size();
        return {};
    }
    QSizeF s = m_pageSizes[pageIndex];
    qDebug() << "[PdfDoc] pageSize(" << pageIndex << ") =" << s;
    return s;
}

QPointF PdfDocument::pageBoxOrigin(int pageIndex) const {
    {
        QMutexLocker lk(&m_sizesMutex);
        if (pageIndex < 0 || pageIndex >= m_pageBoxKnown.size()) return QPointF(0.0, 0.0);
        if (m_pageBoxKnown[pageIndex]) return m_pageBoxOrigins[pageIndex];
    }
    // 🔴 NHA m_sizesMutex TRUOC khi lay khoa PDFium — giu ca hai = khoa long nhau.
    QPointF org(0.0, 0.0);
    {
        QMutexLocker lk(&pdfiumGlobalMutex());
        if (!m_doc) return org;
        FPDF_PAGE page = FPDF_LoadPage(m_doc, pageIndex);
        if (page) {
            float l = 0.f, b = 0.f, r = 0.f, t = 0.f;
            bool ok = FPDFPage_GetCropBox(page, &l, &b, &r, &t) != 0;
            if (!ok) ok = FPDFPage_GetMediaBox(page, &l, &b, &r, &t) != 0;
            if (ok && r > l && t > b) org = QPointF(double(l), double(b));
            FPDF_ClosePage(page);
        }
    }
    {
        QMutexLocker lk(&m_sizesMutex);
        if (pageIndex >= 0 && pageIndex < m_pageBoxKnown.size()) {
            m_pageBoxOrigins[pageIndex] = org;
            m_pageBoxKnown[pageIndex]   = true;
        }
    }
    return org;
}

QPointF PdfDocument::pageBoxOriginCached(int pageIndex) const {
    QMutexLocker lk(&m_sizesMutex);
    if (pageIndex < 0 || pageIndex >= m_pageBoxKnown.size()) return QPointF(0.0, 0.0);
    if (m_pageBoxKnown[pageIndex]) return m_pageBoxOrigins[pageIndex];
    return QPointF(0.0, 0.0);
}

void PdfDocument::updatePageSize(int pageIndex, double w, double h) {
    QMutexLocker lk(&m_sizesMutex);
    if (pageIndex >= 0 && pageIndex < m_pageSizes.size() && w > 0 && h > 0)
        m_pageSizes[pageIndex] = {w, h};
}

void PdfDocument::updatePageBoxOrigin(int pageIndex, QPointF origin) {
    QMutexLocker lk(&m_sizesMutex);
    if (pageIndex >= 0 && pageIndex < m_pageBoxKnown.size()) {
        m_pageBoxOrigins[pageIndex] = origin;
        m_pageBoxKnown[pageIndex]   = true;
    }
}

bool PdfDocument::hasRasterPages() const {
    if (!m_doc) return false;
    for (int i = 0; i < pageCount(); ++i) {
        FPDF_PAGE page = FPDF_LoadPage(m_doc, i);
        FPDF_TEXTPAGE text = FPDFText_LoadPage(page);
        int charCount = FPDFText_CountChars(text);
        FPDFText_ClosePage(text);
        FPDF_ClosePage(page);
        if (charCount == 0) return true;
    }
    return false;
}
