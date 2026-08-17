#pragma once
#include <QWidget>
#include <QHash>
#include <QtGlobal>

class QLabel;
class QPushButton;
class QComboBox;
class QProgressBar;
class QResizeEvent;
class PdfDocument;

// Bo nho dem "trang co chu hay khong" theo (doc, pageIndex). SPEC_PERF_DESK_ABOUT:
// pageHasText goi FPDF_LoadPage + FPDFText_LoadPage RAT NANG (file CAD trang den
// 2,18 trieu path) va truoc day chay dong bo tren luong giao dien moi lan doi
// trang. Cache nay cho chac chan: luong giao dien tra "chua biet" ngay (khong
// block), viec kiem chay o luong QtConcurrent, xong lai cap nhat cache.
// Truy cap CHI TU LUONG GIAO DIEN (worker post ve bang QMetaObject queued).
namespace OcrTextCache {
    using DocHandle = quintptr;

    // -1 = chua biet (chua nam trong cache). 0 = khong co chu. 1 = co chu.
    int  hasTextStatus(DocHandle doc, int pageIndex);
    void setHasText(DocHandle doc, int pageIndex, bool hasText);
    // OCR vua chen chu vao trang → du lieu cu khong con dung.
    void invalidatePage(DocHandle doc, int pageIndex);
    // Dong tai lieu → xoa het dem cua doc do.
    void clearDocument(DocHandle doc);
}

// Panel OCR trong sidebar (tab thu 6, SPEC_OCR_TAB_AND_SELECT phan 1).
// Chi giao dien + trang thai; VIEC CHAY NHAN DANG de MainWindow lo (runOcr),
// panel nay chi phat lenh + hien tien do/Cancel qua tinh hieu.
class OcrPanel : public QWidget {
    Q_OBJECT
public:
    explicit OcrPanel(QWidget* parent = nullptr);

    // Doc + trang hien tai -> cap nhat dong Status.
    void setDocument(PdfDocument* doc);
    void setCurrentPage(int page);
    // Doi theme (Dark/Light) — nhanh chua dung QSS chung, chi vien nha cho panel.
    void setDarkMode(bool dark);

    // MainWindow goi khi OCR bat dau / ket thuc.
    void setOcrRunning(bool running);
    // Tien do theo trang: pageIndex1Based = so trang dang xu ly (1-based).
    void setProgress(int pageIndex1Based, int totalPages);
    // Ghi nho so tu OCR doc duoc cho mot trang (de hien "Recognized (N words)").
    void setPageWords(int page, int words);
    // Cap nhat lai Status ngay (sau khi OCR xong / doi trang).
    void refresh();

public slots:
    void updateStatus();

signals:
    void recognizeWholeRequested(const QString& langs);
    void recognizeCurrentPageRequested(const QString& langs);
    void cancelRequested();

private:
    QString statusText() const;
    void updateButtonState();
    void applyPanelTheme();
    void elideStatus();

protected:
    void resizeEvent(QResizeEvent* e) override;

private:

    PdfDocument* m_doc        = nullptr;
    int          m_currentPage = -1;
    bool         m_ocrRunning  = false;
    int          m_progressPage = 0;
    int          m_progressTotal = 0;
    bool         m_hasRun      = false;   // da chay OCR lan nao -> nut "Re-recognize"
    bool         m_dark        = false;
    QHash<int,int> m_wordsByPage;         // page -> so tu OCR

    QLabel*       m_status      = nullptr;
    QString       m_statusFull;           // chu day du truoc khi elide
    QPushButton*  m_wholeBtn    = nullptr;
    QPushButton*  m_pageBtn     = nullptr;
    QComboBox*    m_langCombo   = nullptr;
    QProgressBar* m_progress    = nullptr;
    QLabel*       m_progressLabel = nullptr;
    QPushButton*  m_cancelBtn   = nullptr;
    QPushButton*  m_openLogBtn  = nullptr;
};
