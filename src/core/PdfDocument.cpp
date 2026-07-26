#include "PdfDocument.h"
#include <QMutex>
#include <QDebug>
#include <fpdf_text.h>

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
    if (--s_refCount == 0)
        FPDF_DestroyLibrary();
}

void PdfDocument::libAddRef()  { ensureInit(); }
void PdfDocument::libRelease() { ensureDestroy(); }

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
    m_fileHandle = CreateFileW(wpath.c_str(), GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE,
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
                        QMutexLocker lock(&s_pdfiumMutex);
                        m_doc = FPDF_LoadMemDocument(m_mapView, static_cast<int>(sz.QuadPart),
                                                     pwd.isEmpty() ? nullptr : pwd.constData());
                        if (m_doc) {
                            m_pageCount = FPDF_GetPageCount(m_doc);
                            m_pageSizes.resize(m_pageCount);
                            for (int i = 0; i < m_pageCount; ++i) {
                                double w = 0, h = 0;
                                FPDF_GetPageSizeByIndex(m_doc, i, &w, &h);
                                m_pageSizes[i] = {w, h};
                            }
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
                    QMutexLocker lock(&s_pdfiumMutex);
                    m_doc = FPDF_LoadMemDocument(m_mapView, static_cast<int>(st.st_size),
                                                 pwd.isEmpty() ? nullptr : pwd.constData());
                    if (m_doc) {
                        m_pageCount = FPDF_GetPageCount(m_doc);
                        m_pageSizes.resize(m_pageCount);
                        for (int i = 0; i < m_pageCount; ++i) {
                            double w = 0, h = 0;
                            FPDF_GetPageSizeByIndex(m_doc, i, &w, &h);
                            m_pageSizes[i] = {w, h};
                        }
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
        QMutexLocker lock(&s_pdfiumMutex);
        m_doc = FPDF_LoadDocument(pathUtf8.constData(),
                                  pwd.isEmpty() ? nullptr : pwd.constData());
        if (m_doc) {
            m_pageCount = FPDF_GetPageCount(m_doc);
            m_pageSizes.resize(m_pageCount);
            for (int i = 0; i < m_pageCount; ++i) {
                double w = 0, h = 0;
                FPDF_GetPageSizeByIndex(m_doc, i, &w, &h);
                m_pageSizes[i] = {w, h};
            }
        }
    }
    if (!m_doc) return false;
    m_filePath = filePath;
    return true;
}

// ── close ─────────────────────────────────────────────────────────────────────
void PdfDocument::close() {
    QMutexLocker lock(&s_pdfiumMutex);
    if (m_doc) {
        FPDF_CloseDocument(m_doc);
        m_doc = nullptr;
        m_filePath.clear();
        m_pageCount = 0;
        m_pageSizes.clear();
    }
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

void PdfDocument::updatePageSize(int pageIndex, double w, double h) {
    QMutexLocker lk(&m_sizesMutex);
    if (pageIndex >= 0 && pageIndex < m_pageSizes.size() && w > 0 && h > 0)
        m_pageSizes[pageIndex] = {w, h};
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
