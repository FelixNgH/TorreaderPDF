#pragma once
#include <QObject>
#include <QString>
#include <QSizeF>
#include <QVector>
#include <QMutex>
#include <QByteArray>
#include <fpdfview.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#endif

// Wraps a PDFium FPDF_DOCUMENT. Thread-safe for read (render) after open.
// On Windows, uses CreateFileMapping so PDFium reads directly from the OS
// page cache instead of copying the whole file into heap — dramatically
// reduces RAM (400 MB PDF = ~50-100 MB working set instead of 3 GB).
// On Linux, uses POSIX mmap for the same benefit.
//
// mmapData() and mmapSize() expose the mapped buffer so PdfRenderer can
// create additional FPDF_DOCUMENT handles from the same memory, enabling
// true parallel rendering (one FPDF_DOCUMENT per thread).
class PdfDocument {
public:
    PdfDocument();
    ~PdfDocument();

    bool open(const QString& filePath, const QString& password = {});
    void close();
    bool isOpen() const { return m_doc != nullptr; }

    int pageCount() const;
    QSizeF pageSize(int pageIndex) const; // in points (1pt = 1/72 inch)
    // Called from render thread after LoadPage to correct /Rotate-aware dimensions.
    void updatePageSize(int pageIndex, double w, double h);

    FPDF_DOCUMENT raw() const { return m_doc; }
    QString filePath() const { return m_filePath; }

    bool hasRasterPages() const;

    static QMutex& pdfiumGlobalMutex();

    // ── mmap access for pool creation ──────────────────────────────────────────
    const void* mmapData() const { return m_mapView; }
    unsigned long mmapSize() const { return m_fileSize; }
    bool hasMmap() const { return m_mapView != nullptr; }
    const char* password() const { return m_password.isEmpty() ? nullptr : m_password.constData(); }

    // New reference-counting helpers for library initialization
    static void libAddRef();
    static void libRelease();

private:
    FPDF_DOCUMENT   m_doc       = nullptr;
    QString         m_filePath;
    QByteArray      m_password;
    int             m_pageCount = 0;
    QVector<QSizeF> m_pageSizes;
    mutable QMutex  m_sizesMutex;

    // ── Cross-platform mmap state ──────────────────────────────────────────────
    void*           m_mapView    = nullptr;
    unsigned long   m_fileSize   = 0;
    FPDF_FILEACCESS m_fileAccess = {}; // callback struct for FPDF_LoadCustomDocument

#ifdef _WIN32
    HANDLE          m_fileHandle = INVALID_HANDLE_VALUE;
    HANDLE          m_mapHandle  = nullptr;
#else
    int             m_fileFd     = -1;
#endif
};
