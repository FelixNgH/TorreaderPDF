#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FormibDoc FormibDoc;

/// Open PDF from raw bytes. Returns null on failure.
/// Caller must call formibpdf_close() when done.
FormibDoc* formibpdf_open(const uint8_t* data, uint32_t len);

/// Returns page count (0 if doc is null).
uint32_t formibpdf_page_count(const FormibDoc* doc);

/// Get page dimensions in PDF points. Returns false if out of range.
bool formibpdf_page_size(const FormibDoc* doc, uint32_t page,
                          double* out_width_pt, double* out_height_pt);

/// Render page to pre-allocated RGBA buffer (width * height * 4 bytes).
/// Returns false if unsupported feature detected → caller falls back to PDFium.
bool formibpdf_render_page(const FormibDoc* doc, uint32_t page,
                            uint32_t width, uint32_t height, uint8_t* out_buf);

/// Extract text from a page as UTF-8.
///
/// Returns the number of bytes written (not including null terminator).
/// If out_buf is NULL or buf_len is 0, returns the required buffer size
/// (without null terminator) so the caller can pre-allocate correctly.
///
/// Typical usage:
///   uint32_t need = formibpdf_extract_text(doc, page, NULL, 0);
///   char*    buf  = (char*)malloc(need + 1);
///   formibpdf_extract_text(doc, page, (uint8_t*)buf, need + 1);
///   // buf is now null-terminated UTF-8.
///   free(buf);
uint32_t formibpdf_extract_text(const FormibDoc* doc, uint32_t page,
                                 uint8_t* out_buf, uint32_t buf_len);

/// Extract text in visual reading order (v2.0 — spatial clustering).
/// Same calling convention as formibpdf_extract_text.
uint32_t formibpdf_extract_text_ordered(const FormibDoc* doc, uint32_t page,
                                         uint8_t* out_buf, uint32_t buf_len);

/// Return the total number of AcroForm fields in the document (v2.0).
uint32_t formibpdf_field_count(const FormibDoc* doc);

/// Fill name/value/type for a field by index. Returns false if index out of range (v2.0).
bool formibpdf_field_info(const FormibDoc* doc, uint32_t field_idx,
                           char* name_buf,  uint32_t name_len,
                           char* value_buf, uint32_t value_len,
                           uint32_t* out_field_type);

/// Free document.
void formibpdf_close(FormibDoc* doc);

#ifdef __cplusplus
}
#endif
