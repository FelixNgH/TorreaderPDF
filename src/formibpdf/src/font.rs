use std::collections::HashMap;
use fontdue::{Font, FontSettings};

/// Special font ID used for the built-in fallback font (loaded from system).
/// Glyphs rendered with this ID are visible even when the PDF font is not embedded.
pub const FALLBACK_FONT_ID: u64 = u64::MAX;

#[derive(Debug, Clone, Default)]
pub struct GlyphBitmap {
    pub bitmap:    Vec<u8>,
    pub width:     usize,
    pub height:    usize,
    pub advance_x: f32,
    pub bearing_x: i32,
    pub bearing_y: i32,
}

#[derive(Hash, Eq, PartialEq, Clone)]
struct GlyphKey { font_id: u64, codepoint: char, size_x100: u32 }

pub struct FontCache {
    fonts:   HashMap<u64, Font>,
    glyphs:  HashMap<GlyphKey, GlyphBitmap>,
    glyphs_idx: HashMap<(u64,u16,u32), GlyphBitmap>,
    next_id: u64,
}

impl Clone for FontCache {
    fn clone(&self) -> Self {
        FontCache {
            fonts:   HashMap::new(),  // fontdue::Font not Clone; strips re-rasterize as needed
            glyphs:  self.glyphs.clone(),
            glyphs_idx: self.glyphs_idx.clone(),
            next_id: self.next_id,
        }
    }
}

impl FontCache {
    pub fn new() -> Self {
        Self { fonts: HashMap::new(), glyphs: HashMap::new(), glyphs_idx: HashMap::new(), next_id: 1 }
    }

    /// Register an embedded font and return its ID.
    pub fn register(&mut self, bytes: &[u8]) -> Option<u64> {
        let font = Font::from_bytes(bytes, FontSettings::default()).ok()?;
        let id = self.next_id;
        self.fonts.insert(id, font);
        self.next_id += 1;
        Some(id)
    }

    /// Register an embedded font and also return the average advance ratio
    /// (advance_width / font_size) sampled from common ASCII chars.
    /// Used by interpreter for correct text_x advance without rasterizing glyphs.
    pub fn register_with_advance(&mut self, bytes: &[u8]) -> Option<(u64, f32)> {
        let font = Font::from_bytes(bytes, FontSettings::default()).ok()?;
        // Sample advance from lowercase 'x' at 100px — a reliable EM-width proxy.
        let ratio = font.metrics('x', 100.0).advance_width / 100.0;
        let id = self.next_id;
        self.fonts.insert(id, font);
        self.next_id += 1;
        Some((id, ratio.clamp(0.3, 1.2)))
    }

    /// Load the system fallback font (Arial / DejaVu / Liberation Sans on Windows/Linux).
    /// Registered with FALLBACK_FONT_ID so rasterize() falls back to it for font_id=0.
    /// Safe to call multiple times — no-op if fallback already loaded.
    pub fn load_fallback_font(&mut self) {
        if self.fonts.contains_key(&FALLBACK_FONT_ID) { return; }
        let candidates: &[&str] = &[
            r"C:\Windows\Fonts\arial.ttf",
            r"C:\Windows\Fonts\Arial.ttf",
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
            "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
            "/usr/share/fonts/TTF/DejaVuSans.ttf",
        ];
        for path in candidates {
            if let Ok(bytes) = std::fs::read(path) {
                if let Ok(font) = Font::from_bytes(bytes.as_slice(), FontSettings::default()) {
                    self.fonts.insert(FALLBACK_FONT_ID, font);
                    return;
                }
            }
        }
        // No system font found — FALLBACK_FONT_ID remains absent; rasterize() uses placeholder.
    }

    /// Rasterize a glyph. If font_id is 0 (missing font), tries FALLBACK_FONT_ID first.
    pub fn rasterize(&mut self, font_id: u64, ch: char, size_px: f32) -> Option<&GlyphBitmap> {
        // Resolve missing font → fallback
        let effective_id = if font_id == 0 && self.fonts.contains_key(&FALLBACK_FONT_ID) {
            FALLBACK_FONT_ID
        } else {
            font_id
        };
        let key = GlyphKey { font_id: effective_id, codepoint: ch, size_x100: (size_px * 100.0) as u32 };
        if self.glyphs.contains_key(&key) { return self.glyphs.get(&key); }

        let glyph = if let Some(font) = self.fonts.get(&effective_id) {
            let (m, bitmap) = font.rasterize(ch, size_px);
            GlyphBitmap {
                bitmap,
                width:     m.width,
                height:    m.height,
                advance_x: m.advance_width,
                bearing_x: m.bounds.xmin as i32,
                bearing_y: (m.bounds.ymin + m.bounds.height as f32) as i32,
            }
        } else {
            // No font at all — invisible placeholder with approximate advance
            let w = (size_px * 0.55) as usize;
            GlyphBitmap {
                bitmap: vec![0u8; w * (size_px as usize).max(1)],
                width:     w,
                height:    size_px as usize,
                advance_x: size_px * 0.55,
                bearing_x: 0,
                bearing_y: size_px as i32,
            }
        };
        self.glyphs.insert(key.clone(), glyph);
        self.glyphs.get(&key)
    }

    /// Rasterize a CID glyph by GID index (Type0 composite fonts). Inserts into cache.
    pub fn rasterize_index(&mut self, font_id: u64, gid: u16, size_px: f32) -> Option<&GlyphBitmap> {
        let key = (font_id, gid, (size_px * 100.0) as u32);
        if self.glyphs_idx.contains_key(&key) { return self.glyphs_idx.get(&key); }
        if let Some(font) = self.fonts.get(&font_id) {
            let (m, bitmap) = font.rasterize_indexed(gid, size_px);
            let glyph = GlyphBitmap {
                bitmap,
                width:     m.width,
                height:    m.height,
                advance_x: m.advance_width,
                bearing_x: m.bounds.xmin as i32,
                bearing_y: (m.bounds.ymin + m.bounds.height as f32) as i32,
            };
            self.glyphs_idx.insert(key, glyph);
            self.glyphs_idx.get(&key)
        } else { None }
    }

    /// Pure read-only lookup of a CID glyph by GID (no insert) — used by strip renderers.
    pub fn get_glyph_index(&self, font_id: u64, gid: u16, size_px: f32) -> Option<&GlyphBitmap> {
        self.glyphs_idx.get(&(font_id, gid, (size_px * 100.0) as u32))
    }

    pub fn text_width(&mut self, font_id: u64, text: &str, size_px: f32) -> f32 {
        text.chars().map(|c| {
            self.rasterize(font_id, c, size_px).map(|g| g.advance_x).unwrap_or(size_px * 0.55)
        }).sum()
    }

    /// Pure read-only lookup: get a pre rasterized glyph without mutating the cache.
    pub fn get_glyph(&self, font_id: u64, ch: char, size_px: f32) -> Option<&GlyphBitmap> {
        // Read-only lookup used by strip renderers whose FontCache clone has an
        // EMPTY `fonts` map — so we can't gate the fallback on fonts.contains_key.
        // Try the requested font_id first; if missing and font_id==0, retry under
        // FALLBACK_FONT_ID (where rasterize() stored fallback glyphs at pre-render).
        let s = (size_px * 100.0) as u32;
        let key = GlyphKey { font_id, codepoint: ch, size_x100: s };
        if let Some(g) = self.glyphs.get(&key) { return Some(g); }
        if font_id == 0 {
            let kf = GlyphKey { font_id: FALLBACK_FONT_ID, codepoint: ch, size_x100: s };
            return self.glyphs.get(&kf);
        }
        None
    }
}
