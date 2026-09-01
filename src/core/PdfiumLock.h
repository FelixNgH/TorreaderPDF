#pragma once
#include <QMutex>
#include <QElapsedTimer>
#include <QThread>
#include <QCoreApplication>
#include <QDebug>

extern QMutex s_pdfiumMutex;

// ── BO DEM VONG DOI PDFIUM (bai do ro ri bo nho 2026-08-31) ────────────────
// Do duoc: mo roi DONG han tai lieu ma RSS khong tra ve (102MB -> giu lai 2.294MB).
// Dem MO vs DONG de biet cai gi khong duoc giai phong.
#include <QAtomicInteger>
extern QAtomicInteger<int> g_pdfiumDocOpen, g_pdfiumDocClose;
extern QAtomicInteger<int> g_pdfiumPoolOpen, g_pdfiumPoolClose;
extern QAtomicInteger<int> g_pdfiumPageOpen, g_pdfiumPageClose;
// Byte texture GPU dang cap phat (bai do 31/08). Texture nam trong bo nho driver —
// KHONG xuat hien trong bo dem nao cua app nhung CO tinh vao RSS tien trinh.
extern QAtomicInteger<qint64> g_gpuTexBytes;
extern QAtomicInteger<int>    g_gpuTexAlive;

// Do CA hai phia cua khoa pdfium: thoi gian CHO lay khoa va thoi gian GIU khoa.
// Nguong 300 ms. Danh dau ro luot nao chay tren LUONG CHINH (main=1) vi do la
// luot lam dung hinh giao dien.
class TimedPdfiumLock {
    const char* m_file;
    int         m_line;
    QElapsedTimer m_hold;
    bool        m_locked = true;
public:
    TimedPdfiumLock(const char* file, int line) : m_file(file), m_line(line) {
        QElapsedTimer w; w.start();
        s_pdfiumMutex.lock();
        const qint64 waited = w.elapsed();
        m_hold.start();
        const bool isMain = (QThread::currentThread() == QCoreApplication::instance()->thread());
        // 🔴 TIEU CHI SO 1 CUA OWNER (31/08): KHONG BAO GIO duoc not-responding.
        // Dinh nghia do duoc: LUONG GIAO DIEN khong duoc di xin khoa pdfium — bat ky lan nao.
        // Nguong 300ms cu chi ghi luc DA dung hinh; muon SUA thi phai thay MOI cho, ke ca cho
        // hom nay chi cho 0ms (mai no se cho 50 giay khi trang nang giu khoa).
        // Bat bang TORREADER_GUILOCK=1.
        static const bool kGuiLockAudit = !qEnvironmentVariableIsEmpty("TORREADER_GUILOCK");
        if (isMain && kGuiLockAudit)
            qDebug().noquote() << "[guilock] cho=" << waited << "ms at" << m_file << ":" << m_line;
        if (waited > 300) {
            qDebug().noquote() << "[lockwait] ms=" << waited << "at" << m_file << ":" << m_line
                               << "main=" << (isMain ? 1 : 0);
        }
    }
    void unlock() {
        if (!m_locked) return;
        const qint64 held = m_hold.elapsed();
        s_pdfiumMutex.unlock();
        m_locked = false;
        if (held > 300)
            qDebug().noquote() << "[lockhold] ms=" << held << "at" << m_file << ":" << m_line;
    }
    ~TimedPdfiumLock() { unlock(); }
    TimedPdfiumLock(const TimedPdfiumLock&) = delete;
    TimedPdfiumLock& operator=(const TimedPdfiumLock&) = delete;
};

// Non-blocking lock for GUI thread (SPEC_SMOOTH_123 VIỆC 1).
// - On GUI thread: tryLock(0). If fail → held()==false, caller must return stale
//   data and dispatch real work to background thread.
// - On non-GUI thread: normal blocking lock (held() always true).
// Lấy được thì làm; KHÔNG lấy được thì trả dữ liệu cũ + đẩy sang luồng nền.
class TryPdfiumLock {
    const char* m_file;
    int         m_line;
    bool        m_held = false;
    QElapsedTimer m_hold;
public:
    TryPdfiumLock(const char* file, int line) : m_file(file), m_line(line) {
        const bool isMain = (QThread::currentThread() == QCoreApplication::instance()->thread());
        static const bool kGuiLockAudit = !qEnvironmentVariableIsEmpty("TORREADER_GUILOCK");
        if (isMain) {
            m_held = s_pdfiumMutex.tryLock(0);
            if (kGuiLockAudit)
                qDebug().noquote() << "[guilock] cho= 0 ms at" << m_file << ":" << m_line;
            if (m_held) m_hold.start();
        } else {
            s_pdfiumMutex.lock();
            m_held = true;
            m_hold.start();
        }
    }
    bool held() const { return m_held; }
    void unlock() {
        if (!m_held) return;
        const qint64 held = m_hold.elapsed();
        s_pdfiumMutex.unlock();
        m_held = false;
        if (held > 300)
            qDebug().noquote() << "[lockhold] ms=" << held << "at" << m_file << ":" << m_line;
    }
    ~TryPdfiumLock() { unlock(); }
    TryPdfiumLock(const TryPdfiumLock&) = delete;
    TryPdfiumLock& operator=(const TryPdfiumLock&) = delete;
};

// Non-blocking lock for GUI thread with a BOUNDED retry, for operations that
// MUST complete (doc close) but must not hold the GUI hostage forever.
// - GUI thread: tryLock(0) up to `maxRetries`×2ms (~50ms); if still held, it is
//   a real lockholder (render slice) that will release soon → fall back to one
//   blocking lock (bounded, no livelock). 
// - Non-GUI thread: plain blocking lock.
class BoundedPdfiumLock {
    const char* m_file;
    int         m_line;
    bool        m_held = false;
    QElapsedTimer m_hold;
public:
    BoundedPdfiumLock(const char* file, int line, int maxRetries = 25)
        : m_file(file), m_line(line) {
        const bool isMain = (QThread::currentThread() == QCoreApplication::instance()->thread());
        static const bool kGuiLockAudit = !qEnvironmentVariableIsEmpty("TORREADER_GUILOCK");
        if (isMain) {
            for (int i = 0; i < maxRetries; ++i) {
                if (s_pdfiumMutex.tryLock(0)) { m_held = true; break; }
                QThread::msleep(2);
            }
            if (kGuiLockAudit)
                qDebug().noquote() << "[guilock] cho=" << (m_held ? 0 : maxRetries)
                                   << "ms at" << m_file << ":" << m_line;
            if (!m_held) {
                qDebug().noquote() << "[lockwait] bounded-gui blocked at" << m_file << ":" << m_line;
                s_pdfiumMutex.lock();
                m_held = true;
            }
            if (m_held) m_hold.start();
        } else {
            s_pdfiumMutex.lock();
            m_held = true;
            m_hold.start();
        }
    }
    bool held() const { return m_held; }
    void unlock() {
        if (!m_held) return;
        const qint64 held = m_hold.elapsed();
        s_pdfiumMutex.unlock();
        m_held = false;
        if (held > 300)
            qDebug().noquote() << "[lockhold] ms=" << held << "at" << m_file << ":" << m_line;
    }
    ~BoundedPdfiumLock() { unlock(); }
    BoundedPdfiumLock(const BoundedPdfiumLock&) = delete;
    BoundedPdfiumLock& operator=(const BoundedPdfiumLock&) = delete;
};
