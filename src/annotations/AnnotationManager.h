#pragma once
#include <QObject>
#include <QString>
#include <QByteArray>
#include <QList>
#include <QRectF>
#include <QColor>
#include <QVector>
#include <QImage>
#include <QHash>
#include <QSet>
#include <QMutex>
#include <atomic>
#include <QFont>
#include <fpdfview.h>
#include <fpdf_annot.h>

class QPainter;
class QFontMetricsF;

// ── FreeText: MOT cong thuc duy nhat cho ca HAI view ( PdfGpuView +
// ContinuousView) ───────────────────────────────────────────────────────────
// Toa do trang cua app anh xa 1 PDF pt = 1 px * zoom (pw = pagePt * zoom), nen
// chu cung phai theo he nay: pixelSize = fontSizePt * zoom (KHONG phai
// pointSizeF — no phong len dpi/72 lan). Vi vay hai ham nay dung chung mot font
// DejaVu (giong /AP) va mot le/wrap — lech la chu nhay khi doi che do.
QFont trDejaVuFontAtPixelSize(double px);
QRectF trFreeTextFitRect(const QRectF& dispRect, const QString& text, float fontSizePt);

// Lề trái-phải mà /AP đặt chữ (x = apPad trong lệnh Td đầu). Ô tự nóng dùng
// ĐÚNG số này để suy ra số dòng ⇒ số dòng trên màn hình LUÔN bằng số dòng
// trong /AP. Đổi hằng này là đổi cả hai cùng lúc — không tách.
constexpr double kTrFreeTextWrapPad = 4.0;

// Hàm ngắt dòng cho FreeText CỦA TA. Một hàm, một nguồn sự thật: ô tự nóng
// (trFreeTextFitRect) VÀ mọi đường dựng /AP (createInlineNote_locked, kể cả
// đường đi qua rebuildTextNote/retextNote) đều gọi nó ⇒ màn hình và file lệch
// nhau. Tách cứng theo ‘\n’, ngắt mềm theo wrapW tại dấu cách, từ quá dài
// thì ngắt theo ký tự. Cùng QFontMetricsF với /AP (trDejaVuFontAtPixelSize).
QStringList trWrapFreeText(const QString& text, double wrapW, const QFontMetricsF& fm);

// Chuẩn hoá xuống dòng cho /Contents: ghi \r (quy ước Acrobat cho
// FreeText) ⇒ đọc ra chuẩn hoá về ‘\n’ (để màn hình + overlay
// + trWrapFreeText cùng thấy đúng dòng).
QString trContentsToCr(const QString& text);
QString trUnescapeContents(const QString& text);

// 🔎 0927 LUOT 7: SO DO khop khung FreeText NGOAI, do TOI /AP DA LUU DOC LAI
// (khong dung so tinh luc dung). Harness --ftngoai-edit-probe goi ham nay de in
// rectBefore/rectAfter/maxLineW/innerW/textH/innerH/clipped.
// `annotIndex` < 0 ⇒ tu do FreeText dau tien tren trang. Khuong do duoc
// (khong tim thay /Rect, /BBox hoac noi dung) ⇒ ok=false, cac truong = 0.
struct ForeignApFit {
    bool   ok = false;
    double rectBefore[4] = {0, 0, 0, 0};
    double rectAfter[4]  = {0, 0, 0, 0};
    double maxLineW = 0.0;
    double innerW   = 0.0;
    double textH    = 0.0;
    double innerH   = 0.0;
    int    nLines   = 0;
    int    clipped  = 0;
    double fontSize = 0.0;
    double lead     = 0.0;
    double bboxH    = 0.0;
    double bboxY0   = 0.0;
    QString reason;      // ly do khuong do duoc
};
bool trMeasureForeignApFit(const QString& path, int pageIndex, int annotIndex,
                           ForeignApFit* out);
void   drawFreeTextOverlay(QPainter& p, const QRectF& dRect, const QString& text,
                           float fontSizePt, double zoom, const QColor& penColor);

// Flat record for one annotation read from a PDF page.
struct AnnotInfo {
    int     pageIndex = 0;
    int     indexInPage = -1;
    QString type;      // "Note", "FreeText", "Highlight", "Underline", etc.
    QString text;      // /Contents
    QString author;    // /T
    QRectF  rect;      // in PDF points, Y upward
    QColor  color;
    bool isDraft = false;
    QString uid;
    bool isOwn = false;   // có /TRUID hoặc /TRID ⇒ annot CỦA TA (xem isOwnAnnot)
};

Q_DECLARE_METATYPE(AnnotInfo)

struct AnnotSnapshot {
    bool  valid = false;
    int   subtype = 0;
    float rl = 0, rt = 0, rr = 0, rb = 0;
    unsigned int r = 255, g = 0, b = 0, a = 255;
    bool hasColor = false;
    bool hasFill = false;
    unsigned int fr = 255, fg = 255, fb = 255, fa = 0;
    float border = 2.0f;
    bool isDraft = false;
    QString da;
    QString contents;
    QString uid;
    QVector<QVector<QPointF>> ink;
    // Insert Image (SPEC_INSERT_IMAGE_2026-08-30): hic anh goc cua Stamp annot
    // (pixel giu kenh alpha) de snapshot/addSnapshot hoan tac dugoc.
    QImage  stamp;
};

// Overlay annotation data — coordinates in display space (Y-down, rotation applied).
struct AnnotVisual {
    int      page = 0;
    QString  uid;
    int      subtype = 0;      // FPDF_ANNOT_*
    QRectF   rect;             // display coords (via pdfToDisp)
    QColor   stroke = QColor(Qt::red);
    QColor   fill = QColor(Qt::transparent);
    float    border = 2.0f;
    QVector<QVector<QPointF>> ink;    // INK: strokes, display coords
    QVector<QRectF>           quads;  // HIGHLIGHT: quad points, display coords
    QString  text;
    float    fontSize = 11.0f;
    bool     isNote = false;
    bool     hasColor = false;  // /C co trong annot dict
    bool     hasFill  = false;  // /IC co trong annot dict
    bool     hasAP    = false;  // /AP co trong annot dict
    // Insert Image (SPEC_INSERT_IMAGE_2026-08-30): anh goc cua Stamp annot de
    // overlay ve (giu kenh alpha). Rong = khong ve duoc qua overlay.
    QImage   image;
    // ponytail: FreeText/Note are drawn as page objects in renderer, not by overlay
    bool     paintByOverlay = true;
};

// 🔴 LÁT C 0902 — CẬP NHẬT CÓ CHỌN LỌC (owner: chú thích phải hiện NGAY khi vừa tạo/xoá,
// không được xếp hàng sau bộ dựng trang). Mỗi lần tạo/xoá, AnnotationManager — vốn ĐANG
// GIU KHOÁ + đang cầm handle annot — dựng luôn AnnotVisual của đúng chú thích đó (µs,
// không thêm thời gian giữ khoá đáng kể) và bỏ vào hàng chờ DELTA theo trang. MainWindow
// trộn delta vào visualsCache thay vì gọi loadPageVisuals (thứ phải chờ 1–2,4 giây sau
// bộ dựng trang rồi tự ôm khoá thêm 2 giây để phân tích cả trang). Phát lại delta theo
// thứ tự là idempotent (thay/xoá theo uid) ⇒ cùng một delta áp hai lần không sao.
struct VisualDelta {
    AnnotVisual av;        // removed=true: chi dung av.uid de dinh vi
    bool        removed = false;
};

// Reads and creates annotations via PDFium.
// All operations are main-thread only (PDFium is single-threaded for writes).
class AnnotationManager : public QObject {
    Q_OBJECT
public:
    explicit AnnotationManager(QObject* parent = nullptr);
    ~AnnotationManager() override;
    // 🔴 0929 LƯỢT 33h: phần nặng của dtor (flush pending gen + PageCache::forgetDocument
    // trên doc CHÍNH) chạy trên LUỒNG NỀN (closeJob) TRƯỚC khi doc->close(); ~AnnotationManager
    // gọi lại → no-op (guard). bản thân mgr là QObject affinity UI ⇒ chết TRÊN UI.
    void shutdownHeavy();

    void setDocument(FPDF_DOCUMENT doc, const QString& filePath);
    // 0928 LƯỢT 14: đọc FPDF_DOCUMENT để task nền gọi mgr (loadAllStreaming /
    // loadPageVisuals / loadPage) cầm token trdoc::Task — close() chờ token về 0
    // trước FPDF_CloseDocument. KHÔNG tự thêm getter này thì task nền phải chốt
    // doc bên ngoài, dễ sót tab.
    FPDF_DOCUMENT document() const { return m_doc; }

    // Read all annotations from one page (fast, called per-page).
    // outOk (optional): when provided, loadPage MUST NOT block the GUI — it uses
    // tryLock and reports whether the page was actually read. *outOk=false means
    // "chua doc duoc (khoa ban)" — NOT "khong co annot". Caller must keep old data
    // and reschedule. When outOk==nullptr, loadPage blocks like before (background
    // scan / headless tests).
    QList<AnnotInfo> loadPage(int pageIndex, bool* outOk = nullptr);
    // GIA DINH ben goi DA giu s_pdfiumMutex. Than xu ly thuc su cua loadPage.
    QList<AnnotInfo> loadPage_locked(int pageIndex);

    // Read all annotations across the whole document.
    QList<AnnotInfo> loadAll(int pageCount);

    // Stream annotations page-by-page, emitting pageAnnotsLoaded per non-empty page.
    void loadAllStreaming(int pageCount, int startPage = 0);

    // Read annotations as overlay visuals for one page.
    // 🔴 LƯỢT 33e (mục 1): `aborted` — BO DE VIEN (tab nen / khoa ban / trang hong),
    // KHONG phai ket qua that. Ben goi PHAI vui tam, KHONG duoc ghi cache — danh sach
    // rong bi cache la loi mat markup khi tab quay lai (reviewer 33d loi 1).
    QList<AnnotVisual> loadPageVisuals(int page, bool* outOverlayCapable,
                                       bool* hasForeign = nullptr,
                                       bool* aborted = nullptr,
                                       bool* unloadable = nullptr,
                                       bool heavyPage = false);

    // Build one AnnotVisual from an already-open annot. Returns false if not overlay-drawable.
    // Caller must hold s_pdfiumMutex and have `page` open.
    bool buildVisual(FPDF_PAGE page, FPDF_ANNOTATION annot, int pageIndex, AnnotVisual& out);

    // Create a sticky-note annotation (FPDF_ANNOT_TEXT) at a point on the page.
    // Saves the document to disk.
    bool createPopupNote(int pageIndex, QPointF pointPdf,
                         const QString& text, const QString& author);

    // Create a free-text annotation (FPDF_ANNOT_FREETEXT) over a rect on the page.
    // Saves the document to disk.
    bool createInlineNote(int pageIndex, QRectF rectPdf,
                          const QString& textIn, const QString& author,
                          bool withBackground = true,
                          QColor textColor = Qt::black,
                          float fontSize = 11.0f);

    // A4 (0921): updateNote da bi XOA HAN — ma chet (khong noi goi nao) va ghi /Contents
    // vao BAT KY annot nao roi tu saveDocument(). Khong con trong API.

    bool removeAnnot(int pageIndex, int index);
    int removeNotePageObjects(int pageIndex, unsigned int noteId);
    bool setAnnotStyle(int pageIndex, int index, QColor color, float width, bool fill, int fillAlpha = 255);
    bool rebuildTextNote(int pageIndex, int index, QColor newColor, float newFontSize);
    int findAnnotIndexByUid(int pageIndex, const QString& uid);
    int findAnnotIndexByAnyUid(int pageIndex, const QString& uid);
    QString ensureExternalUid(int pageIndex, int index);
    bool setAnnotUid(int pageIndex, int index, const QString& uid);
    bool setAnnotContents(int pageIndex, int index, const QString& text);
    QString generateUid();
    bool retextNote(int pageIndex, int index, const QString& newText);

    // Di chuyen annot BAT KY loai nao: chi tinh tien, KHONG dung lai gi.
    // dxU/dyU la delta trong he PDF CHUA xoay (goi ben tu doi theo /Rotate).
    // Note cua ta (co TRID): dich luon page object cua no.
    // Annot ngoai / hinh khoi: chi dich /Rect, /AP giu nguyen.
    // INK (Freehand): snapshot + remove + add (vi PDFium khong cho sua InkList tai cho).
    bool moveAnnot(int pageIndex, int index, double dxU, double dyU);

    // ── Insert Image (SPEC_INSERT_IMAGE_2026-08-30) ─────────────────────────
    // Tao STAMP annot tu anh (giu kenh alpha); rectDisp o TOA DO HIEN THI
    // (Y-down, da ap /Rotate + pageBoxOrigin). Sinh /AP ngay de file luu ra
    // hien dung, giu trong suot. Tra TRUID neu xong, rong neu that bai.
    QString insertStampImage(int pageIndex, const QImage& image, QRectF rectDisp);
    // Co giãn Stamp annot: dat /Rect moi (toa do hien thi) — PDFium tu scale
    // AP form vua /Rect nen chi can /Rect. Toi thieu 20x20 pt. Goi bumpPageRevision.
    bool setAnnotRectDisplay(int pageIndex, int index, QRectF rectDisp);

    // Annot cua TorReader co TRUID (moi) hoac TRID (note cu). Khong co ca hai = cua phan mem khac.
    bool isOwnAnnot(int pageIndex, int index);

    // ── VIỆC 1 (0921): MỘT CỬA DUY NHẤT cho mọi đường GHI vào annot ──────────
    // Trước 0921, quyền sở hữu chỉ là vài chốt rải rác theo từng hàm; vòng chấm
    // độc lập tìm ra 9 đường ghi khác nhau chạm vào annot của phần mềm khác. Từ
    // đây, MỌI hàm ghi phải gọi guardWrite(...) TRƯỚC lệnh ghi đầu tiên.
    //   • annot có /TRUID hoặc /TRID  ⇒ CỦA TA ⇒ cho qua mọi loại ghi.
    //   • annot không có cả hai        ⇒ CỦA PHẦN MỀM KHÁC ⇒ TỪ CHỐI, trừ:
    //       – Delete: owner chốt "backup rồi xoá" (caller có trách nhiệm backup);
    //       – Contents: 🔴 0927 LƯỢT 6 (SPEC 0927 BƯỚC 2) MỞ KHOÁ cho GUI —
    //         FreeText / FreeTextCallout ngoài sửa được qua chuột phải →
    //         "Edit text…". An toàn vì retextNote() nhánh ngoài KHÔNG xoá
    //         annot: chỉ ghi /Contents + /TR_AP_REBUILD, /AP được vá lúc
    //         save (rebuildForeignFreeTextAp) với /TR_AP_ORIG +
    //         /TR_CONTENTS_ORIG ghi đúng MỘT lần. Style/Rect/Geometry
    //         (nhánh default) vẪN TỪ CHỐI; Ink/Square/Line/PolyLine không
    //         có đường sửa chữ nên không bao giờ đi qua đây.
    enum class AnnotWrite { Contents, Style, Rect, Geometry, Delete, Uid };
    bool guardWrite(int pageIndex, int index, AnnotWrite what, QString* whyNot = nullptr);

    // Dem so annot tren mot trang (nhanh hon loadPage vi khong parse tung cai).
    int annotCount(int pageIndex);

    AnnotSnapshot snapshotAnnot(int pageIndex, int index);
    // GIA DINH: ben goi DA giu s_pdfiumMutex va co `page` mo — dung de chup snapshot
    // NGAY TRUOC invalidate (khong re-load trang 2 giay nhu snapshotAnnot sau commit).
    AnnotSnapshot snapshotAnnot_locked(FPDF_PAGE page, int pageIndex, int index);
    bool addSnapshot(int pageIndex, const AnnotSnapshot& s);

    // Read-back for Properties dialog. Returns false if annot does not exist.
    bool getAnnotEditState(int pageIndex, int index, QString& outType,
                           QColor& outColor, float& outWidth, float& outFontSize,
                           bool* outHasFill = nullptr, int* outFillAlpha = nullptr);

    bool createSignatureDraft(int pageIndex, QRectF rectPt, const QString& text);
    QRectF findSignatureDraftRect(int pageIndex, int* outIndex);

    quint32 pageRevision(int page) const { return m_pageRev.value(page, 0); }
    void    bumpPageRevision(int page);

    // ── LÁT C 0902: hàng chờ delta cho đường CẬP NHẬT CÓ CHỌN LỌC ──────────────
    // Peek (KHONG xoa) — delta chi bi xoá trong loadPageVisuals, luc ay anh cat
    // da bao tram moi thay doi ghi truoc no.
    QList<VisualDelta> visualDeltas(int page) const;
    // Nhap delta tu ngoai vao (AnnotationLayer::annotVisualAdded noi truc tiep vao day).
    void recordVisualDelta(int page, const AnnotVisual& av);

    void stopScan()  { m_stopScan.store(true); }
    void resetScan() { m_stopScan.store(false); }
    bool scanStopped() const { return m_stopScan.load(); }
    // Low-priority: user dang thao tac (markup/scroll/zoom) → scan nhuong buoc.
    void setUserBusy(bool b) { m_userBusy.store(b); }
    bool isUserBusy() const { return m_userBusy.load(); }

    // Generate content for a single page (called just before save from deferred set).
    static int setOwnNoteObjectsActive(FPDF_PAGE page, bool active);

    void generateContentForPage(int page);

    void flushPendingGenerate(int page);
    void flushAllPendingGenerate();

    bool saveDocument();
    // 📐 NẤC 1 (0921): vá `/AP/N` cho FreeText NGOÀI (không TRUID) đã đánh dấu
    // `/TR_AP_REBUILD`. QPDF lưu `/AP` gốc vào `/TR_AP_ORIG` và `/RC` gốc vào
    // `/TR_RC_ORIG`, thay khối text bằng bản DejaVu ngắt dòng lại, xoá `/RC`.
    // Trả về số annot đã vá (>=0), hoặc -1 nếu có annot cần vá mà không vá được
    // (mất font / không parse được /AP / lỗi QPDF / ghi tệp) — khi đó save phải thất bại.
    // 🔴 0927 LƯợT 2: vá `/AP` cho FreeText CỦA TA — `FPDFAnnot_SetAP`
    // trá false (PDFium bug #1381) ⇒ file lưu ra không có `/AP`, không renderer
    // độc lập nào vẽ được chủ. Nội dung `/AP` ghi tạm vào sidecar
    // `.traownap` để QPDF dựng lại `/AP/N` thật lúc save. Trả số đã vá,
    // -1 = có trang cần vá mà không vá được (save phải thát bại).
    int patchOwnNoteAp(const QString& path);
    int rebuildForeignFreeTextAp(const QString& path);
    // 🔴 P1 (0921): đường HOÀN TÁC — PHẠM VI: trả `/AP/N` từ `/TR_AP_ORIG`,
    // `/Contents` từ `/TR_CONTENTS_ORIG`, `/RC` từ `/TR_RC_ORIG`. KHÔNG phải hoàn tác
    // thẳng: vật thể neo font DejaVu trong nội dung trang vẫn còn. Trả false + đặt
    // m_lastError nếu thiếu khoá gốc hoặc ghi tệp thất bại (tệp giữ nguyên).
    bool restoreForeignAp(int pageIndex, int index);

    // ── VIỆC 3-A3 (0921): "backup rồi xoá" cho annot NGOÀI ────────────────────
    // Owner chốt: vẫn cho xoá annot phần mềm khác, nhưng phải sao lưu nguyên vẹn để
    // Ctrl+Z dựng lại BYTE-BẰNG (mọi khoá + stream: /AP, /Contents, /RC, /Rect, hình
    // học, khoá riêng). Cách làm: flush trạng thái in-memory xuống m_path rồi CHÉP
    // NGUYÊN TỆP ra sidecar; lúc hoàn tác, QPDF trích đúng object annot từ sidecar và
    // chèn lại vào /Annots (deep-copy giữ nguyên stream). Trả đường dẫn sidecar, hoặc
    // rỗng nếu không sao lưu được (khi đó người gọi PHẢI TỪ CHỐI xoá).
    QString backupAnnotForDelete(int pageIndex, int index);
    // Dựng lại annot từ sidecar vào đúng vị trí `insertAt`. Thao tác ở tầng QPDF trên
    // m_path (tự flush trước). Trả false + m_lastError nếu hỏng (tệp giữ nguyên).
    bool restoreAnnotFromBackup(int pageIndex, int insertAt, const QString& backupPath);
    // Xoá sidecar sau khi dùng xong.
    void discardAnnotBackup(const QString& backupPath);

    // ⚠️ 0927 LƯỢT 6: KHÔNG còn cần bật để sửa FreeText ngoài từ GUI (đã mở
    // khoá chính thức ở guardWrite/retextNote). Giữ lại setter + cờ để
    // harness cũ `--ftngoai-ap` và các ca đo 21/09 chạy tiếp được; cờ KHÔNG
    // còn thay đổi hành vi nào. Chỉ còn `m_allowTestStampUid` là công tắc
    // thật (harness mô phỏng annot CỦA TA; GUI không bao giờ bật).
    void setAllowForeignFreeTextEdit(bool on) { m_allowForeignFreeTextEdit = on; }
    // ⚠️ CHỈ harness: cho phép `setAnnotUid` ghi /TRUID lên annot CHƯA có uid, để mô
    // phỏng "annot của ta" trong phép thử âm/dương. GUI KHÔNG BAO GIỜ bật; mặc định
    // false ⇒ trong sản phẩm, setAnnotUid vẫn TỪ CHỐI annot ngoài. Đây không phải
    // đường sản phẩm nên không tính là "cửa sau" của luật sở hữu.
    void setAllowTestStampUid(bool on) { m_allowTestStampUid = on; }
    QString lastError() const { return m_lastError; }
    QString lastCreatedUid() const { return m_lastCreatedUid; }
    int lastCreatedIndex() const { return m_lastCreatedIndex; }
    AnnotSnapshot lastCreatedSnapshot() const { return m_lastCreatedSnapshot; }

    // Kept for --foreignbench headless benchmark (main.cpp). Not used by GUI.
    QImage buildForeignAnnotLayer(int pageIndex, int wPx, int hPx);

signals:
    void annotationAdded(int pageIndex, AnnotInfo info);
    void pageAnnotsLoaded(int pageIndex, QList<AnnotInfo> annots);
    void scanProgress(int pagesScanned, int totalPages);
    void pageContentChanged(int page);

private:

    FPDF_FONT     m_unicodeFont = nullptr;
    QByteArray    m_unicodeFontData;
    FPDF_FONT unicodeFont();

    // ⚠️ _locked: caller must hold s_pdfiumMutex and have `page` open.
    // No lock, no LoadPage/ClosePage, no GenerateContent, no emit.
    // VIỆC 1: bản _locked của guardWrite — KHÔNG tự lấy khoá (tránh deadlock khi
    // gọi từ trong vùng đã khoá). Dùng cho các helper _locked có lệnh ghi PDFium.
    bool guardWrite_locked(FPDF_ANNOTATION annot, AnnotWrite what, QString* whyNot = nullptr);
    // 0927 LUOT 6 (VIEC 1) - moc 1 object chu " " bang font DejaVu vao noi dung
    // trang de PDFium NHUNG font khi save. Bat buoc cho MOI duong danh dau
    // /TR_AP_REBUILD (retextNote VA setAnnotContents): rebuildForeignFreeTextAp()
    // goi findDejaVuFont(page); trang chua tung co FreeText cua ta thi /Resources
    // khong co DejaVu => save that bai. Giu khoa m_apAnchorPages nen moi trang
    // chi moc MOT lan. GOI khi da giu s_pdfiumMutex + da mo trang.
    void ensureDejaVuFontAnchor_locked(FPDF_PAGE page, int pageIndex);
    int  removeNotePageObjects_locked(FPDF_PAGE page, unsigned int noteId);
    int  translateNotePageObjects_locked(FPDF_PAGE page, int pageIndex,
                                         unsigned int noteId, double dx, double dy);
    bool objectHasNoteId(FPDF_PAGEOBJECT obj, unsigned int noteId);
    bool removeAnnot_locked(FPDF_PAGE page, int index, bool* outNeedsGen);
    bool createInlineNote_locked(FPDF_PAGE page, int pageIndex, QRectF rectPdf,
                                 const QString& textIn, const QString& author,
                                 bool withBackground, QColor textColor, float fontSize,
                                 AnnotInfo* outInfo);
    bool createPopupNote_locked(FPDF_PAGE page, int pageIndex, QPointF pointDisp,
                                const QString& text, const QString& author,
                                AnnotInfo* outInfo);

    // GIA DINH: ben goi DA giu s_pdfiumMutex va `page` mo — ghi delta Added cho
    // annot VUA TAO (annot cuoi cung cua trang) bang buildVisual, µs, khong mo trang.
    void recordCreatedVisual_locked(FPDF_PAGE page, int pageIndex);
    // GIA DINH: ben goi DA giu s_pdfiumMutex va `page` mo — doc uid cua annot truoc
    // khi xoá (xoá xong la khong doc duoc nua).
    QString annotUid_locked(FPDF_PAGE page, int index);
    // GIA DINH: ben goi DA giu s_pdfiumMutex. Khong duoc tu khoa.
    void flushGenerate_locked(int pageIndex);
    // GIA DINH: ben goi DA giu s_pdfiumMutex.
    void releaseSharedPage_locked();
    void invalidateNoteObjCache_locked(int pageIndex);

    std::atomic<bool> m_stopScan{false};
    std::atomic<bool> m_userBusy{false};
    bool              m_heavyShutdown = false;   // L33h: shutdownHeavy đã chạy (idempotent)
    QHash<int, quint32> m_pageRev;
    QSet<int> m_pendingGenerate;
    QSet<int> m_pendingGen;                  // trang co page object doi, chua sinh noi dung
    bool      m_hasForeignApEdits = false;   // co FreeText ngoai cho vá /AP luc save
    // 🔴 0927 LƯỢT 2: FreeText CỦA TA cũng cần vá /AP ở tầng FILE.
    // Đo được 27/09: FPDFAnnot_SetAP trả FALSE (log `[inote] SetAP returned=false`,
    // apLenAfter=2) ⇒ /AP KHÔNG bao giờ vào file ⇒ PyMuPDF/khác không thấy chữ.
    // Cùng lý do PDFium bug #1381 mà NẤC 1 đã gặp với FreeText ngoài. Ta ghi
    // /TR_AP_OWN_PEND + lưu nội dung /AP vào sidecar, lúc save thì QPDF dựng
    // /AP/N thật rồi XOÁ khoá tạm (file sạch, không rò khoá nội bộ).
    QSet<int> m_apOwnPending;                // trang co FreeText cua ta can va /AP
    bool      m_ownApFailed = false;         // co trang va /AP that bai ⇒ save that bai
    // 🔴 0927 LƯỢT 3 — SỬA GỐC RỄ (đo 27/09, đây là lý do probe treo 60 s và
    // out.pdf bị 2928 byte = y hệt tệp gốc). Sidecar ".traownap" từng là nguồn
    // /AP DUY NHẤT cho patchOwnNoteAp, nhưng nó được ghi theo `m_path` lúc TẠO
    // annot (= tệp gốc) và đọc theo `m_path` lúc SAVE (= bản nhập trong %TEMP%)
    // ⇒ hai đường dẫn KHÁC NHAU ⇒ file sidecar không bao giờ tồn tại ⇒
    // patchOwnNoteAp trả -1 ⇒ saveDocument false ⇒ onSaveFile bật QMessageBox
    // MODAL ⇒ probe bơm processEvents nên treo vô hạn. Sidecar vẫn giữ làm
    // bản ghi phục (crash giữa chừng), nhưng nguồn sự thật lúc vá là bộ nhớ này.
    // key = "page:trid" — cùng khoá với dòng sidecar.
    QHash<QString, QByteArray> m_ownApInMem; // /AP (nguyên byte) cho từng annot CUA TA
    QHash<QString, QSizeF>     m_ownApBox;   // kích thước ô (w,h) theo /BBox cần
    bool      m_allowForeignFreeTextEdit = false;  // 0927 L6: da GOI (GUI mo khoa), giu lai cho harness
    bool      m_allowTestStampUid = false;         // chi harness bat; de mo phong annot cua ta
    QSet<int> m_apAnchorPages;               // trang da dat moc neo font DejaVu
    QHash<QPair<int,quint32>, QVector<int>> m_noteObjIdxCache;

    // Livelock fix (2026-08-31): dem so lan tryLock truot LIEN TIEP theo trang cho
    // duong loadPage (chi khi outOk!=nullptr = duong GUI). >= 3 lan thi CHO THAT
    // (khoa blocking) de dut diem, khong hen lai nhap nhay mai. Reset khi lay duoc khoa.
    QHash<int,int> m_loadPageRetry;

    // LÁT C 0902: delta cho duong cap nhat co chon loc. m_deltaMutex CHI bao ve
    // m_visualDeltas — KHONG BAO GIO lay s_pdfiumMutex khi dang giu no (thuat toan
    // kho: s_pdfiumMutex → m_deltaMutex mot chieu).
    QHash<int, QList<VisualDelta>> m_visualDeltas;
    mutable QMutex m_deltaMutex;

    FPDF_DOCUMENT m_doc     = nullptr;
    QString       m_path;
    QString       m_lastError;
    unsigned int  m_nextNoteId = 1;
    QString m_lastCreatedUid;
    int m_lastCreatedIndex = -1;
    AnnotSnapshot m_lastCreatedSnapshot;

public:
    // BO LOP DEM CŨ (m_pinLru + m_scratchPage) — thay bang PageCache chung
    // (SPEC_PAGECACHE_CORE_2026-08-16). Nhung phuong thuc nay giu nguyen ten de
    // MainWindow / harness cũ dung duoc; than noi duoi la PageCache::acquire /
    // forgetDocument. PageCache la chu so huu duy nhat cua FPDF_PAGE.
    bool isSharedPage(int pageIndex) const;
    // Tra FPDF_PAGE muon tu PageCache (borrow++). Ben goi PHAI goi
    // PageCache::release(m_doc, pageIndex) / PageCache::PageBorrow khi dung xong
    // (SPEC_PERF_HEAVYPAGE R1 — moi acquire phai di cap release).
    FPDF_PAGE acquireSharedPage(int pageIndex);
    void pinPage(int pageIndex);
    void pinPage_locked(int pageIndex);
    // TU giu khoa. Chi goi tu noi KHONG giu khoa.
    void      releaseSharedPage();
};
