pub mod token;
pub mod graphics_state;
pub mod path;
pub mod pdf_object;
pub mod xref;
pub mod document;
pub mod font;
pub mod ccitt;
pub mod jbig2;
pub mod j2k;
pub mod image_decoder;
pub mod cmap;
pub mod shading;
pub mod resource;
pub mod interpreter;
pub mod rasterizer;
pub mod function;
pub mod pattern;
pub mod text_order;
pub mod acroform;

use crate::document::PdfDocumentReader;
use crate::font::FontCache;
use crate::interpreter::Interpreter;
use crate::resource::PageResources;
use crate::interpreter::RenderCommand;
use std::sync::Mutex;
use std::collections::HashSet;
use rayon::prelude::*;

/// Scale band index (0=thumb 0-0.3×, 1=half 0.3-0.7×, 2=full 0.7-1.5×, 3=hi 1.5×+)
fn scale_band(scale: f64) -> u8 {
    if scale < 0.3 { 0 } else if scale < 0.7 { 1 } else if scale < 1.5 { 2 } else { 3 }
}

pub struct FormibDoc {
    pub reader:     PdfDocumentReader,
    /// Persists glyph bitmaps across render calls — eliminates per-page font re-parse.
    pub font_cache: Mutex<FontCache>,
    /// Pages already known to need PDFium fallback (detected on first render).
    /// Avoids re-running the interpreter for thumbnail → full-res cycles.
    fallback_pages: Mutex<HashSet<u32>>,
    /// In-memory render cache keyed by (page, scale_band).
    /// FormibPDF renders are expensive; cache avoids duplicate work on
    /// thumbnail → full-res cycles and tab switching.
    render_cache:   Mutex<std::collections::HashMap<(u32, u8), Vec<u8>>>,
    /// Lazily populated AcroForm field list.
    acroform_cache: Mutex<Option<Vec<crate::acroform::AcroField>>>,
}

#[repr(C)]
pub struct PixelBuffer {
    pub data:   *mut u8,
    pub width:  u32,
    pub height: u32,
    pub stride: u32,
    pub len:    u32,
}

/// Open PDF from raw bytes. Returns null on failure.
/// Caller must call formibpdf_close() when done.
#[no_mangle]
pub extern "C" fn formibpdf_open(data: *const u8, len: u32) -> *mut FormibDoc {
    if data.is_null() || len == 0 { return std::ptr::null_mut(); }
    let bytes = unsafe { std::slice::from_raw_parts(data, len as usize) };
    if bytes.len() < 4 || &bytes[..4] != b"%PDF" { return std::ptr::null_mut(); }
    // Guard the whole parse in catch_unwind: a panic in xref/object parsing of a
    // malformed/huge PDF must NOT unwind across the C ABI (that is UB → crashes the
    // host app). On panic we return null and the C++ caller falls back to PDFium.
    std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        match PdfDocumentReader::open(bytes.to_vec()) {
            Ok(reader) => {
                let mut fc = FontCache::new();
                fc.load_fallback_font(); // loads Arial/DejaVu for non-embedded font PDFs
                Box::into_raw(Box::new(FormibDoc {
                    reader,
                    font_cache:     Mutex::new(fc),
                    fallback_pages: Mutex::new(HashSet::new()),
                    render_cache:   Mutex::new(std::collections::HashMap::new()),
                    acroform_cache: Mutex::new(None),
                }))
            }
            Err(_) => std::ptr::null_mut(),
        }
    })).unwrap_or(std::ptr::null_mut())
}

#[no_mangle]
pub extern "C" fn formibpdf_page_count(doc: *const FormibDoc) -> u32 {
    if doc.is_null() { return 0; }
    unsafe { (*doc).reader.page_count() as u32 }
}

/// Get page dimensions in PDF points. Returns false if page index out of range.
#[no_mangle]
pub extern "C" fn formibpdf_page_size(
    doc: *const FormibDoc, page: u32,
    out_width_pt: *mut f64, out_height_pt: *mut f64,
) -> bool {
    if doc.is_null() { return false; }
    let r = unsafe { &*doc };
    match r.reader.page(page as usize) {
        Some(p) => {
            if !out_width_pt.is_null()  { unsafe { *out_width_pt  = p.width_pt;  } }
            if !out_height_pt.is_null() { unsafe { *out_height_pt = p.height_pt; } }
            true
        }
        None => false,
    }
}

/// Render page to pre-allocated RGBA buffer (width * height * 4 bytes).
/// Returns false if unsupported → caller falls back to PDFium.
#[no_mangle]
pub extern "C" fn formibpdf_render_page(
    doc: *const FormibDoc,
    page:    u32,
    width:   u32,
    height:  u32,
    out_buf: *mut u8,
) -> bool {
    if doc.is_null() || out_buf.is_null() || width == 0 || height == 0 { return false; }
    // catch_unwind: a panic during interpret/raster of a malformed page must not
    // cross the C ABI (UB → host crash). On panic return false → C++ uses PDFium.
    std::panic::catch_unwind(std::panic::AssertUnwindSafe(move || -> bool {
    use crate::rasterizer::Canvas;

    let doc_ref = unsafe { &*doc };

    // Fast path: page already known to need PDFium — skip interpreter entirely.
    if let Ok(fb) = doc_ref.fallback_pages.lock() {
        if fb.contains(&page) { return false; }
    }

    let page_info = match doc_ref.reader.page(page as usize) {
        Some(p) => p,
        None    => return false,
    };

    let page_w_pt = page_info.width_pt;
    let page_h_pt = page_info.height_pt;
    if page_w_pt < 1.0 || page_h_pt < 1.0 { return false; }
    let scale_x = width  as f64 / page_w_pt;
    let scale_y = height as f64 / page_h_pt;
    let scale   = scale_x.min(scale_y);

    // Load page resources under the font_cache lock, then release it immediately.
    // Holding the lock for the full render would serialize all concurrent renders
    // (one per CPU thread) through a single bottleneck — causing the app to freeze
    // when navigating rapidly through many pages.
    let (page_resources, content) = {
        let mut fc = match doc_ref.font_cache.lock() {
            Ok(g)  => g,
            Err(_) => return false,
        };
        let xref = &doc_ref.reader.xref;
        let data = &doc_ref.reader.data;
        let res     = PageResources::from_page(&page_info.page_dict, xref, data, &mut *fc);
        let content = page_info.content.clone();
        (res, content)
        // font_cache lock drops here — other pages can now render concurrently
    };

    // Interpret the content stream (lock-free).
    let mut interp = Interpreter::new(page_w_pt, page_h_pt);
    interp.set_resources(page_resources);
    let cmds = interp.interpret(&content).to_vec();

    // SSAA params — computed BEFORE pre-rasterize so glyphs are cached at the SAME
    // px size the strips will request (strips render at aa_scale). Must match the
    // values used for the strip loop below.
    let use_ssaa = width <= 1400 && height <= 1400;
    let aa_w     = if use_ssaa { width  * 2 } else { width  };
    let aa_h     = if use_ssaa { height * 2 } else { height };
    let aa_scale = if use_ssaa { scale * 2.0 } else { scale };

    // Pre-rasterize glyphs into font cache before parallel rendering
    // and snapshot a read-only clone for strips.
    let (cmds_arc, fc_snapshot) = {
        // Re-acquire font cache to populate glyphs
        let mut fc = match doc_ref.font_cache.lock() {
            Ok(g)  => g,
            Err(_) => return false,
        };
        // Pre-rasterize every glyph referenced by cmds at the strip render scale.
        let scale_for_pregen = aa_scale;
        for cmd in &cmds {
            if let RenderCommand::DrawGlyph{codepoints,glyph_ids,font_id,font_size,ctm,..} = cmd {
                let ctm_scale = (ctm.a*ctm.a+ctm.b*ctm.b).sqrt().max(0.01);
                let px = ((*font_size * ctm_scale * scale_for_pregen) as f32).max(4.0);
                for &ch in codepoints {
                    let _ = fc.rasterize(*font_id, ch, px);
                }
                for &gid in glyph_ids {
                    let _ = fc.rasterize_index(*font_id, gid, px);
                }
            }
        }
        (std::sync::Arc::new(cmds), fc.clone())
        // fc lock dropped after clone
    };

    // Shadow copy for multi-thread strips
    let fc_arc = std::sync::Arc::new(fc_snapshot);

    // ── Pre-render cache population complete; proceed with multi-strip render ──
    // Snapshot the glyph cache arc for sharing across strips
    // Snapshot already done; reuse fc_arc

    // ── Render cache check (fast path for repeated renders) ─────────────────
    let band = scale_band(scale);
    let expected_len = match (width as usize).checked_mul(height as usize)
                                             .and_then(|n| n.checked_mul(4)) {
        Some(n) => n,
        None    => return false,
    };
    if let Ok(cache) = doc_ref.render_cache.lock() {
        if let Some(cached) = cache.get(&(page, band)) {
            if cached.len() == expected_len {
                let buf = unsafe { std::slice::from_raw_parts_mut(out_buf, expected_len) };
                buf.copy_from_slice(cached);
                return true;
            }
        }
    }

    // ── Parallel rendering (2× SSAA only for small pages) ───────────────────
    // (use_ssaa / aa_w / aa_h / aa_scale computed above, before pre-rasterize.)
    // Use all available CPU threads for strips — rayon will schedule them optimally.
    // On a 4-core laptop: 4 strips. On an 8-core desktop: 8 strips.
    let num_strips = std::thread::available_parallelism()
        .map(|n| n.get())
        .unwrap_or(4)
        .max(4); // minimum 4 strips for quality (avoid coarse strip boundaries)
    let strip_h = (aa_h as usize + num_strips - 1) / num_strips;

    let cmds_arc = cmds_arc; // Arc<Vec<RenderCommand>>
    let fc_arc = fc_arc;     // Arc<FontCache>

    let strips: Vec<Vec<u8>> = (0..num_strips).into_par_iter().map(|s| {
        let y0 = (s * strip_h) as u32;
        let y1 = ((s + 1) * strip_h).min(aa_h as usize) as u32;
        let h   = y1 - y0;
        if h == 0 { return Vec::new(); }

        // Each strip is a separate Canvas covering output rows [y0, y1). to_screen
        // maps a PDF point to global screen row (page_h_pt - py)*aa_scale; to land it
        // at the strip-local row (global - y0) we SUBTRACT y0/aa_scale from page_h.
        // (A '+' here renders every strip below the first entirely off-canvas → blank
        // bottom / white stripes. Regression guard: tests/strip_render.rs.)
        let page_h_offset = page_h_pt - (y0 as f64 / aa_scale);
        let mut c  = Canvas::new(aa_w, h);
        // Render uses the shared font cache
        c.render(&cmds_arc, page_h_offset, aa_scale, &*fc_arc);
        c.pixels
    }).collect();

    // Box-filter 2×2 → final pixels — parallelized across rows via rayon.
    let buf = unsafe { std::slice::from_raw_parts_mut(out_buf, expected_len) };

    if use_ssaa {
        // Parallel SSAA downscale: each output row computed independently.
        buf.par_chunks_mut(width as usize * 4)
            .enumerate()
            .for_each(|(y, row)| {
                for x in 0..width as usize {
                    let mut r = 0u32; let mut g = 0u32;
                    let mut b = 0u32; let mut a = 0u32;
                    for dy in 0..2usize {
                        for dx in 0..2usize {
                            let ay = y * 2 + dy;
                            let strip_idx = ay / strip_h;
                            let strip_y   = ay % strip_h;
                            if let Some(strip) = strips.get(strip_idx) {
                                let si = (strip_y * aa_w as usize + (x * 2 + dx)) * 4;
                                if si + 3 < strip.len() {
                                    r += strip[si]     as u32;
                                    g += strip[si + 1] as u32;
                                    b += strip[si + 2] as u32;
                                    a += strip[si + 3] as u32;
                                }
                            }
                        }
                    }
                    let di = x * 4;
                    row[di]     = (r / 4) as u8;
                    row[di + 1] = (g / 4) as u8;
                    row[di + 2] = (b / 4) as u8;
                    row[di + 3] = (a / 4) as u8;
                }
            });
    } else {
        // No SSAA — strips map 1:1 to output; just copy row by row.
        for (s, strip) in strips.iter().enumerate() {
            let y0 = s * strip_h;
            let row_bytes = width as usize * 4;
            for (row_in_strip, chunk) in strip.chunks(aa_w as usize * 4).enumerate() {
                let y = y0 + row_in_strip;
                if y >= height as usize { break; }
                let di = y * row_bytes;
                if di + row_bytes <= expected_len {
                    buf[di..di + row_bytes].copy_from_slice(&chunk[..row_bytes.min(chunk.len())]);
                }
            }
        }
    }

    // Store in render cache — size-based limit 80MB total.
    // Count-based limit (100 entries) fails for large PDFs: 100 × 24MB = 2.4GB.
    if let Ok(mut cache) = doc_ref.render_cache.lock() {
        let total_bytes: usize = cache.values().map(|v| v.len()).sum();
        const MAX_CACHE_BYTES: usize = 80 * 1024 * 1024; // 80 MB
        if total_bytes + expected_len > MAX_CACHE_BYTES { cache.clear(); }
        cache.insert((page, band), buf.to_vec());
    }
    true
    })).unwrap_or(false)
}

/// Extract text from a page as UTF-8.
///
/// Returns the number of bytes written (not including null terminator).
/// If `out_buf` is null or `buf_len` is 0, returns the required buffer size
/// (not including null terminator) so the caller can allocate the right amount.
///
/// Usage pattern:
/// ```c
/// uint32_t need = formibpdf_extract_text(doc, page, NULL, 0);
/// char* buf = malloc(need + 1);
/// formibpdf_extract_text(doc, page, buf, need + 1);
/// ```
#[no_mangle]
pub extern "C" fn formibpdf_extract_text(
    doc:     *const FormibDoc,
    page:    u32,
    out_buf: *mut u8,
    buf_len: u32,
) -> u32 {
    if doc.is_null() { return 0; }
    let doc_ref = unsafe { &*doc };

    let page_info = match doc_ref.reader.page(page as usize) {
        Some(p) => p,
        None    => return 0,
    };

    let mut font_cache = match doc_ref.font_cache.lock() {
        Ok(g)  => g,
        Err(_) => return 0,
    };

    let page_resources = {
        let xref = &doc_ref.reader.xref;
        let data = &doc_ref.reader.data;
        PageResources::from_page(&page_info.page_dict, xref, data, &mut *font_cache)
    };

    let content = page_info.content.clone();
    let page_w  = page_info.width_pt;
    let page_h  = page_info.height_pt;

    let mut interp = Interpreter::new(page_w, page_h);
    interp.set_resources(page_resources);
    interp.interpret(&content);

    // Sort text items top-to-bottom (PDF Y increases upward, so -y sorts top→bottom),
    // then left-to-right within the same line.
    let mut items = interp.text_items;
    items.sort_by(|a, b| {
        let ya = -a.y; let yb = -b.y;
        ya.partial_cmp(&yb)
            .unwrap_or(std::cmp::Ordering::Equal)
            .then(a.x.partial_cmp(&b.x).unwrap_or(std::cmp::Ordering::Equal))
    });

    // Join items: newline between different lines, space between items on the same line.
    let mut result = String::new();
    let mut prev_y   = f64::MAX;
    let mut prev_set = false;

    for item in &items {
        let line_threshold = item.font_size * 0.5;
        if prev_set && (prev_y - item.y).abs() > line_threshold {
            result.push('\n');
        } else if prev_set {
            result.push(' ');
        }
        result.push_str(&item.text);
        prev_y   = item.y;
        prev_set = true;
    }

    let bytes  = result.as_bytes();
    let needed = bytes.len() as u32;

    // Size-query mode: return required size without writing.
    if out_buf.is_null() || buf_len == 0 {
        return needed;
    }

    // Write mode: copy up to buf_len-1 bytes and null-terminate.
    let write_len = needed.min(buf_len.saturating_sub(1)) as usize;
    unsafe {
        std::ptr::copy_nonoverlapping(bytes.as_ptr(), out_buf, write_len);
        *out_buf.add(write_len) = 0; // null terminator
    }
    write_len as u32
}

#[no_mangle]
pub extern "C" fn formibpdf_close(doc: *mut FormibDoc) {
    if doc.is_null() { return; }
    unsafe { drop(Box::from_raw(doc)); }
}

/// Extract text in visual reading order (spatial clustering, v2.0).
/// Usage: call with out_buf=NULL to get required size, then call again with buffer.
#[no_mangle]
pub extern "C" fn formibpdf_extract_text_ordered(
    doc: *const FormibDoc,
    page:    u32,
    out_buf: *mut u8,
    buf_len: u32,
) -> u32 {
    if doc.is_null() { return 0; }
    let doc_ref = unsafe { &*doc };

    let page_info = match doc_ref.reader.page(page as usize) { Some(p) => p, None => return 0 };
    let mut fc = match doc_ref.font_cache.lock() { Ok(g) => g, Err(_) => return 0 };
    let res = {
        let xref = &doc_ref.reader.xref;
        let data = &doc_ref.reader.data;
        crate::resource::PageResources::from_page(&page_info.page_dict, xref, data, &mut *fc)
    };
    let mut interp = crate::interpreter::Interpreter::new(page_info.width_pt, page_info.height_pt);
    interp.set_resources(res);
    interp.interpret(&page_info.content.clone());

    let ordered = crate::text_order::extract_ordered(&interp.text_items);
    let bytes   = ordered.as_bytes();
    let needed  = bytes.len() as u32;

    if out_buf.is_null() || buf_len == 0 { return needed; }
    let write_len = needed.min(buf_len.saturating_sub(1)) as usize;
    unsafe {
        std::ptr::copy_nonoverlapping(bytes.as_ptr(), out_buf, write_len);
        *out_buf.add(write_len) = 0;
    }
    write_len as u32
}

fn ensure_acroform(doc: &FormibDoc) {
    let mut cache = match doc.acroform_cache.lock() { Ok(g) => g, Err(_) => return };
    if cache.is_some() { return; }
    let fields = crate::acroform::extract_fields(
        &doc.reader.catalog,
        &doc.reader.xref,
        &doc.reader.data,
    );
    *cache = Some(fields);
}

/// Return the number of AcroForm fields in the document.
#[no_mangle]
pub extern "C" fn formibpdf_field_count(doc: *const FormibDoc) -> u32 {
    if doc.is_null() { return 0; }
    let doc_ref = unsafe { &*doc };
    ensure_acroform(doc_ref);
    doc_ref.acroform_cache.lock().ok()
        .and_then(|c| c.as_ref().map(|v| v.len() as u32))
        .unwrap_or(0)
}

/// Fill name_buf, value_buf, and *out_field_type for the given field index.
/// Returns true if field_idx is valid.
#[no_mangle]
pub extern "C" fn formibpdf_field_info(
    doc:           *const FormibDoc,
    field_idx:     u32,
    name_buf:      *mut u8,
    name_len:      u32,
    value_buf:     *mut u8,
    value_len:     u32,
    out_field_type: *mut u32,
) -> bool {
    if doc.is_null() { return false; }
    let doc_ref = unsafe { &*doc };
    ensure_acroform(doc_ref);
    let cache = match doc_ref.acroform_cache.lock() { Ok(g) => g, Err(_) => return false };
    let fields = match cache.as_ref() { Some(f) => f, None => return false };
    let field  = match fields.get(field_idx as usize) { Some(f) => f, None => return false };

    let write_str = |s: &str, buf: *mut u8, len: u32| {
        if buf.is_null() || len == 0 { return; }
        let bytes = s.as_bytes();
        let n = bytes.len().min((len as usize).saturating_sub(1));
        unsafe {
            std::ptr::copy_nonoverlapping(bytes.as_ptr(), buf, n);
            *buf.add(n) = 0;
        }
    };
    write_str(&field.name,  name_buf,  name_len);
    write_str(&field.value, value_buf, value_len);
    if !out_field_type.is_null() { unsafe { *out_field_type = field.field_type; } }
    true
}

#[cfg(test)]
mod tests {
    use super::token::{Tokenizer, Token};
    use super::graphics_state::{Color, StateStack};
    use super::path::Path;
    use super::pdf_object::{PdfObject, parse_object};

    #[test]
    fn tokenizer_basic() {
        let stream = b"1 0 0 1 100 200 cm /Helvetica 12 Tf (Hello) Tj";
        let mut tok = Tokenizer::new(stream);
        assert_eq!(tok.next_token(), Some(Token::Integer(1)));
        assert_eq!(tok.next_token(), Some(Token::Integer(0)));
    }

    #[test]
    fn cmyk_to_rgb() {
        let c = Color::from_cmyk(0.0, 0.0, 0.0, 1.0); // pure black
        assert!(c.r < 0.01 && c.g < 0.01 && c.b < 0.01);
        let white = Color::from_cmyk(0.0, 0.0, 0.0, 0.0);
        assert!(white.r > 0.99);
    }

    #[test]
    fn state_stack_push_pop() {
        let mut stack = StateStack::new();
        stack.current_mut().line_width = 3.0;
        stack.push();
        stack.current_mut().line_width = 7.0;
        assert_eq!(stack.current().line_width, 7.0);
        stack.pop();
        assert_eq!(stack.current().line_width, 3.0);
    }

    #[test]
    fn pdf_object_dict() {
        let data = b"<< /Type /Page /MediaBox [0 0 612 792] >>";
        let mut pos = 0;
        let obj = parse_object(data, &mut pos).unwrap();
        assert!(matches!(obj, PdfObject::Dict(_)));
        let t = obj.dict_get("Type").and_then(|o| o.as_name());
        assert_eq!(t, Some("Page"));
    }

    #[test]
    fn pdf_indirect_ref() {
        let data = b"1 0 R";
        let mut pos = 0;
        let obj = parse_object(data, &mut pos).unwrap();
        assert_eq!(obj.as_ref_id(), Some((1, 0)));
    }

    #[test]
    fn path_transform() {
        use super::graphics_state::Matrix;
        let mut p = Path::new();
        p.move_to(0.0, 0.0);
        p.line_to(100.0, 0.0);
        let m = Matrix { a: 1.0, b: 0.0, c: 0.0, d: 1.0, e: 10.0, f: 20.0 };
        let t = p.transformed(&m);
        if let super::path::PathCmd::LineTo(x, y) = t.cmds[1] {
            assert!((x - 110.0).abs() < 1e-9);
            assert!((y - 20.0).abs() < 1e-9);
        } else { panic!("wrong cmd"); }
    }

    #[test]
    fn cmap_bfchar_roundtrip() {
        let cmap_data = b"begincmap\n2 beginbfchar\n<0041> <0041>\n<0042> <0042>\nendbfchar\nendcmap\n";
        let map = super::cmap::parse_cmap(cmap_data);
        assert_eq!(map.get(&0x41), Some(&'A'));
        assert_eq!(map.get(&0x42), Some(&'B'));
    }

    #[test]
    fn decode_text_bytes_with_cmap() {
        use super::interpreter::decode_text_bytes;
        use std::collections::HashMap;
        let mut cmap: HashMap<u32, char> = HashMap::new();
        cmap.insert(0x0041, 'A');
        cmap.insert(0x0042, 'B');
        // Single-byte lookup.
        let chars = decode_text_bytes(&[0x41, 0x42], Some(&cmap));
        assert_eq!(chars, vec!['A', 'B']);
    }

    #[test]
    fn decode_text_bytes_no_cmap() {
        use super::interpreter::decode_text_bytes;
        let chars = decode_text_bytes(b"Hello", None);
        assert_eq!(chars, vec!['H', 'e', 'l', 'l', 'o']);
    }
}
