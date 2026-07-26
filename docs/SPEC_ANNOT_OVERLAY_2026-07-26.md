# SPEC — Lớp overlay annotation (bỏ re-render trang khi sửa markup)

> Ngày 2026-07-26. Nguồn gốc: user báo "vẽ 1 Line chờ lâu, xoá markup app đơ, lật trang mất mượt".
> Truy nguyên: markup đang nằm TRONG đường render trang → mỗi thao tác markup = re-raster cả trang
> (trang CAD 3-4 giây, số đã đo ở `docs/PROJECT_TORREADER.md` §2.1).

## 1. Bằng chứng — vì sao kiến trúc hiện tại sai

| Sự thật | Nguồn (kiểm được) |
|---|---|
| `FPDFPage_GenerateContent` **chỉ cần gọi trước khi Save hoặc reload trang** | `third_party/pdfium/include/fpdf_edit.h:258-259` — *"Before you save the page to a file, or reload the page, you must call FPDFPage_GenerateContent or any changes to page will be lost."* |
| Ta đang gọi nó **14 lần** — sau mỗi lần tạo/xoá/đổi style annotation | `AnnotationLayer.cpp` 5 chỗ + `AnnotationManager.cpp` 9 chỗ |
| `FPDF_ANNOT` là **cờ tuỳ chọn** khi render → render nội dung trang không kèm annotation được | `fpdfview.h:814` |
| Okular: lấy pixmap **đã cache** (`page->_o_nearestPixmap`), dựng back-buffer khi có annotation (`useBackBuffer = … !bufferedAnnotations.isEmpty()`), vẽ annotation bằng **QPainter** (`LineAnnotPainter::draw`, `drawShapeOnImage`) — KHÔNG nhờ backend PDF | `KDE/okular` `gui/pagepainter.cpp` |
| pdf.js: `AnnotationLayer` / `AnnotationEditorLayer` tách khỏi canvas; canvas chỉ render lại khi đổi **zoom/rotation** | tài liệu pdf.js layers |
| PDFium **đọc lại được hình học** annotation (nên overlay vẽ được cả annot có sẵn trong file) | `fpdf_annot.h`: `GetInkListCount` 442, `GetInkListPath` 457, `GetAttachmentPoints` 391, `CountAttachmentPoints` 380, `GetVertices` 430, `GetLine` 471, `GetColor` 319, `GetBorder` 503 |

**Kết luận:** lớp nền (raster nội dung trang) và lớp annotation phải TÁCH. Sửa markup chỉ repaint overlay.

## 2. Kiến trúc đích

```
┌─ Lớp nền: raster nội dung trang, render KHÔNG kèm annotation, cache (RAM + tile đĩa)
│   → chỉ render lại khi ĐỔI TRANG hoặc ĐỔI ZOOM (như pdf.js)
└─ Lớp overlay: MỌI annotation của trang đang xem, vẽ bằng QPainter từ model trong RAM
    → thêm/xoá/kéo/đổi màu markup = cập nhật model + update() ⇒ TỨC THÌ, không đụng PDFium render
```

Ghi vào PDF: vẫn tạo/xoá annotation trong tài liệu PDFium như hiện tại (để Save ra file đúng), nhưng
**`GenerateContent` hoãn tới lúc Save**.

## 3. ⚠️ Hai cạm bẫy BẮT BUỘC xử lý

### 3.1 Annotation do app khác tạo — cấm mất im lặng
Bỏ `FPDF_ANNOT` khỏi render nền ⇒ mọi annotation trong file sẽ do overlay vẽ. Overlay chỉ vẽ được các subtype ta
đọc được hình học (INK, SQUARE, CIRCLE, HIGHLIGHT, LINE, POLYGON, FREETEXT, TEXT). Gặp subtype khác (Stamp có ảnh,
FileAttachment, Sound, Redact, 3D, Popup…) mà cứ bỏ qua thì **annotation biến mất khỏi trang** — vi phạm
**LUẬT SỐ 1** của project (`docs/PROJECT_TORREADER.md` §2.1: cấm lệnh thoát sớm im lặng trong đường render/vẽ).

**Giải pháp — cờ `overlayCapable` THEO TỪNG TRANG:**
- Khi nạp visuals của trang, nếu **mọi** annot đều thuộc nhóm vẽ được → `overlayCapable = true` → render nền
  **không** `FPDF_ANNOT` + overlay vẽ.
- Nếu có **bất kỳ** annot lạ → `overlayCapable = false` → trang đó render nền **CÓ** `FPDF_ANNOT` như cũ và
  overlay **không** vẽ gì (tránh vẽ đôi). Trang đó mất đường nhanh (sửa markup vẫn re-render) nhưng **đúng hình**.
- Ghi log: `[annot] page=N overlayCapable=0 reason=subtype=<n>` để biết vì sao chậm.

### 3.2 `FPDF_FFLDraw` — con dấu chữ ký visible
Trong `src/core/PdfRenderer.cpp` có **7** chỗ truyền `FPDF_ANNOT`, trong đó **2 chỗ là `FPDF_FFLDraw`**
(dòng ~183, ~591). FFLDraw vẽ form field + **con dấu chữ ký visible** (xem `docs/PROJECT_TORREADER.md` §Bảo mật).
⇒ **GIỮ NGUYÊN 2 chỗ FFLDraw**, chỉ bỏ `FPDF_ANNOT` ở các lời gọi render **nội dung trang**
(dòng ~175, ~351, ~489, ~499, ~587). Sau khi sửa phải kiểm chữ ký vẫn hiện.

## 4. Model dữ liệu

```cpp
struct AnnotVisual {
    int      page = 0;
    QString  uid;              // TRUID (rỗng nếu annot của file gốc)
    int      subtype = 0;      // FPDF_ANNOT_*
    QRectF   rect;             // ĐÃ ở hệ display (qua pdfToDisp) — paint khỏi phải tính lại
    QColor   stroke, fill;
    float    border = 2.0f;
    QVector<QVector<QPointF>> ink;    // INK: nhiều stroke, điểm ở hệ display
    QVector<QRectF>           quads;  // HIGHLIGHT: QuadPoints → rect display
    QString  text;                    // FreeText/Note contents
    float    fontSize = 11.0f;
    bool     isNote = false;          // TEXT subtype → vẽ icon note
};
```

`AnnotationManager::loadPageVisuals(int page, bool* outOverlayCapable) -> QList<AnnotVisual>`
- Một lần khoá `s_pdfiumMutex`, một lần `FPDF_LoadPage`. Đọc:
  `GetSubtype` · `GetRect` · `GetColor` (fallback khoá custom **TRC** — pdfium Windows xoá `/C`, xem
  `SPEC_SIGNATURE_MARKUP_2026-07-19.md` §7) · `GetBorder` · INK: `GetInkListCount`+`GetInkListPath` ·
  HIGHLIGHT: `CountAttachmentPoints`+`GetAttachmentPoints` · LINE: `GetLine` · FreeText: `Contents` + chuỗi `DA`.
- **Mọi toạ độ chuyển sang hệ display bằng `pdfToDisp`/`pdfRectToDisp` trong `src/core/PdfCoords.h`** — KHÔNG
  viết phép biến đổi thứ hai (gốc chuỗi bug markup lệch chỗ là trang `/Rotate 90`, xem memory
  `torreader-markup-rotate90-rootcause-2026-07-21`).

## 5. Phạm vi làm theo 2 bước (bước 1 KHÔNG được đổi hình ảnh trên màn hình)

### BƯỚC 1 — dựng lớp overlay, giữ hành vi y hệt hiện tại
Mục tiêu: sau bước này app **trông không khác gì**, chỉ là annotation do overlay vẽ thay vì PDFium vẽ.
1. `AnnotationManager::loadPageVisuals()` như trên.
2. `PdfGpuView::setAnnotVisuals(const QList<AnnotVisual>&)` + vẽ trong `paintGL` (dùng lại đúng kiểu code đang vẽ
   `m_pendingMarkups`, cùng phép nhân zoom + `pageOrigin()`).
3. `ContinuousView` **cũng phải** vẽ overlay tương ứng — nếu bỏ sót thì chế độ cuộn liên tục sẽ mất sạch
   annotation (mất im lặng, không được).
4. `PdfRenderer`: thêm tham số/cờ `bool withAnnots` cho đường render nội dung trang; main-view truyền `false`
   khi trang `overlayCapable`, `true` khi không. **Không đụng 2 chỗ FFLDraw.** Thumbnail giữ nguyên như cũ.
5. Nạp/refresh visuals khi: mở file, đổi trang, đổi zoom, sau mỗi thay đổi annotation (1 trang, không quét cả file).

**Nghiệm thu bước 1:** mở file có markup sẵn → hình markup hiện **giống hệt** bản cũ ở single mode và continuous
mode, ở nhiều mức zoom, và ở trang có `/Rotate 90`. Chữ ký visible vẫn hiện. Chưa cần nhanh hơn.

### BƯỚC 2 — bật đường nhanh
6. Mọi thao tác markup (add/delete/move/properties) **BỎ** `invalidatePage()` + `requestPage()`; chỉ:
   cập nhật visuals của trang đó → `view->setAnnotVisuals(...)` → `update()`.
7. `GenerateContent`: **bỏ** khỏi đường tạo/xoá annotation thuần (INK/SQUARE/CIRCLE/HIGHLIGHT).
   Giữ cho đường **Note/FreeText** (đang vẽ bằng page object — mẫu `TRNote`, xem memory
   `torreader-annot-rotate-pageobject-pattern-2026-07-21`), vì page object nằm trong content stream.
8. `DocTab` thêm `QSet<int> pagesNeedingGenerate;` — lúc **Save**: gọi `GenerateContent` cho từng trang trong set
   rồi mới `saveDocument()`. Đây là chỗ DUY NHẤT còn gọi GenerateContent theo lô.

**Nghiệm thu bước 2:** trên trang bản vẽ CAD nặng, vẽ 1 Line / xoá 1 markup phải **tức thì** (mắt không thấy chờ),
log `[perf] markup commit ms=` phải < 50ms, và **không** xuất hiện dòng render lại trang.

## 6. Việc KHÔNG được làm
- KHÔNG thêm lại undo/redo (user đã yêu cầu gỡ).
- KHÔNG revert: `invalidatePage` (thay `clearCache`), bỏ `setTileCache(nullptr)` trên đường markup,
  `annotsForPage` cache, `refreshCommentsForPage` (cập nhật 1 trang), `invalidateTiles()`.
- KHÔNG đụng kiến trúc render tiệm tiến (cắt lát + nhả mutex) ở `PdfRenderer` — chỉ thêm cờ `withAnnots`.
- KHÔNG dùng API Qt 6.6+ (bản Linux build trên Qt 6.2.4).
- KHÔNG bỏ `s_pdfiumMutex` quanh bất kỳ lời gọi `FPDF_*`.

## 7. Đo lường (bắt buộc có, để lần sau khỏi đoán)
- `[perf] loadPageVisuals page=N count=K capable=0/1 ms=X`
- `[perf] markup commit ms=X` (từ lúc nhả chuột tới lúc overlay repaint xong)
- `[annot] page=N overlayCapable=0 reason=subtype=<n>`
