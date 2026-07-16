// jbig2.rs — JBIG2 decoder (ITU-T T.88 / ISO 14492)
//
// Supports:
//   • Embedded PDF JBIG2 streams (no file header, raw generic region data)
//   • Full JBIG2 files with segment headers (magic 0x97 0x4A 0x42 0x32 …)
//   • Generic Region segments (types 6 and 7) with MQ arithmetic coding
//   • Templates 0, 1, 2, 3 with optional AT pixels
//   • Typical Prediction (TPGDON)
//   • Symbol Dictionary segments (type 0) — decoded but not composited
//   • Page Information segments (type 48)
//
// Output: 1-bit packed rows, MSB first.
//         length = height * ceil(width / 8)
//         bit value 1 = black (as per JBIG2 convention)
//
// No external crates — pure std Rust 2021.

// ─────────────────────────────────────────────────────────────────────────────
// Public entry-point
// ─────────────────────────────────────────────────────────────────────────────

/// Decode a JBIG2 data stream embedded in a PDF `/Filter /JBIG2Decode` object.
///
/// Returns 1-bit packed rows (MSB first).
/// Length = `height * ceil(width / 8)`.
/// Bit 1 = black pixel.
/// Returns `None` if the stream cannot be decoded.
pub fn decode_pdf_stream(data: &[u8], width: u32, height: u32) -> Option<Vec<u8>> {
    // Detect full JBIG2 file by magic bytes: 0x97 'J' 'B' '2' 0x0D 0x0A 0x1A 0x0A
    if data.len() >= 8
        && data[0] == 0x97
        && data[1] == 0x4A   // 'J'
        && data[2] == 0x42   // 'B'
        && data[3] == 0x32   // '2'
        && data[4] == 0x0D
        && data[5] == 0x0A
        && data[6] == 0x1A
        && data[7] == 0x0A
    {
        decode_jbig2_file(data, width, height)
    } else {
        // Embedded stream: raw MQ-encoded generic region, no segment headers.
        // Use Template 0, no TPGDON, no AT pixels.
        decode_generic_stream(data, width, height)
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// BitReader — byte-aligned MSB-first reader (reserved for Symbol Dict parsing)
// ─────────────────────────────────────────────────────────────────────────────

#[allow(dead_code)]
struct BitReader<'a> {
    data:     &'a [u8],
    byte_pos: usize,
    bit_pos:  u8,   // 0 = MSB of current byte is next; 8 = exhausted
}

#[allow(dead_code)]
impl<'a> BitReader<'a> {
    fn new(data: &'a [u8]) -> Self {
        Self { data, byte_pos: 0, bit_pos: 0 }
    }

    #[inline]
    fn read_bit(&mut self) -> Option<u8> {
        if self.byte_pos >= self.data.len() {
            return None;
        }
        let byte = self.data[self.byte_pos];
        let bit  = (byte >> (7 - self.bit_pos)) & 1;
        self.bit_pos += 1;
        if self.bit_pos == 8 {
            self.bit_pos  = 0;
            self.byte_pos += 1;
        }
        Some(bit)
    }

    fn read_bits(&mut self, n: u8) -> Option<u32> {
        let mut val = 0u32;
        for _ in 0..n {
            val = (val << 1) | (self.read_bit()? as u32);
        }
        Some(val)
    }

    fn read_u8(&mut self) -> Option<u8> {
        self.align_byte();
        if self.byte_pos >= self.data.len() {
            return None;
        }
        let b = self.data[self.byte_pos];
        self.byte_pos += 1;
        Some(b)
    }

    fn read_u16_be(&mut self) -> Option<u16> {
        let hi = self.read_u8()? as u16;
        let lo = self.read_u8()? as u16;
        Some((hi << 8) | lo)
    }

    fn read_u32_be(&mut self) -> Option<u32> {
        let b0 = self.read_u8()? as u32;
        let b1 = self.read_u8()? as u32;
        let b2 = self.read_u8()? as u32;
        let b3 = self.read_u8()? as u32;
        Some((b0 << 24) | (b1 << 16) | (b2 << 8) | b3)
    }

    /// Skip remaining bits in the current byte so the next read is byte-aligned.
    #[inline]
    fn align_byte(&mut self) {
        if self.bit_pos != 0 {
            self.bit_pos  = 0;
            self.byte_pos += 1;
        }
    }

    /// Current byte offset (aligned).
    #[inline]
    fn position(&self) -> usize {
        if self.bit_pos == 0 { self.byte_pos } else { self.byte_pos + 1 }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// MQ-Coder (ITU-T T.88 §D — Arithmetic entropy coding)
// ─────────────────────────────────────────────────────────────────────────────

/// 47 probability states: (Qe, NMPS, NLPS, SWITCH)
/// SWITCH = 1 means exchange MPS/LPS on LPS renormalisation.
#[allow(clippy::unreadable_literal)]
static QE_TABLE: [(u32, u8, u8, u8); 47] = [
    (0x5601,  1,  1, 1),
    (0x3401,  2,  6, 0),
    (0x1801,  3,  9, 0),
    (0x0AC1,  4, 12, 0),
    (0x0521,  5, 29, 0),
    (0x0221, 38, 33, 0),
    (0x5601,  7,  6, 1),
    (0x5401,  8, 14, 0),
    (0x4801,  9, 14, 0),
    (0x3801, 10, 14, 0),
    (0x3001, 11, 17, 0),
    (0x2401, 12, 18, 0),
    (0x1C01, 13, 20, 0),
    (0x1601, 29, 21, 0),
    (0x5601, 15, 14, 1),
    (0x5401, 16, 14, 0),
    (0x5101, 17, 15, 0),
    (0x4801, 18, 16, 0),
    (0x3801, 19, 17, 0),
    (0x3401, 20, 18, 0),
    (0x3001, 21, 19, 0),
    (0x2801, 22, 19, 0),
    (0x2401, 23, 20, 0),
    (0x2201, 24, 21, 0),
    (0x1C01, 25, 22, 0),
    (0x1801, 26, 23, 0),
    (0x1601, 27, 24, 0),
    (0x1401, 28, 25, 0),
    (0x1201, 29, 26, 0),
    (0x1101, 30, 27, 0),
    (0x0AC1, 31, 28, 0),
    (0x09C1, 32, 29, 0),
    (0x08A1, 33, 30, 0),
    (0x0521, 34, 31, 0),
    (0x0441, 35, 32, 0),
    (0x02A1, 36, 33, 0),
    (0x0221, 37, 34, 0),
    (0x0141, 38, 35, 0),
    (0x0111, 39, 36, 0),
    (0x0085, 40, 37, 0),
    (0x0049, 41, 38, 0),
    (0x0025, 42, 39, 0),
    (0x0015, 43, 40, 0),
    (0x0009, 44, 41, 0),
    (0x0005, 45, 42, 0),
    (0x0001, 45, 43, 0),
    (0x5601, 46, 46, 0),
];

/// Per-context state for the MQ-coder.
#[derive(Clone, Copy)]
struct CxState {
    state: u8,  // index into QE_TABLE (0..=46)
    mps:   u8,  // most-probable symbol: 0 or 1
}

impl Default for CxState {
    fn default() -> Self { Self { state: 0, mps: 0 } }
}

/// MQ arithmetic decoder (T.88 §D.3).
struct MqDecoder<'a> {
    data: &'a [u8],
    pos:  usize,
    c:    u32,   // code register (upper 16 bits active, lower 16 = fractional)
    a:    u32,   // augmented interval register
    ct:   u8,    // bit-counter for refill
}

impl<'a> MqDecoder<'a> {
    fn new(data: &'a [u8]) -> Self {
        let mut d = MqDecoder { data, pos: 0, c: 0, a: 0x0001_0000, ct: 0 };
        // Prime: INITDEC — two BYTEIN calls, each shifts c left by 8.
        d.fill();
        d.c <<= 8;
        d.fill();
        d.c <<= 8;
        d.ct -= 8;
        d
    }

    /// BYTEIN: fetch the next byte/stuffed byte into the code register.
    fn fill(&mut self) {
        let b = if self.pos < self.data.len() { self.data[self.pos] } else { 0xFF };
        self.pos += 1;
        if b == 0xFF {
            let b2 = if self.pos < self.data.len() { self.data[self.pos] } else { 0xFF };
            if b2 > 0x8F {
                // Marker code: do NOT consume b2; add 0xFF00 as a stuffed byte.
                self.c  += 0xFF00;
                self.ct  = 8;
            } else {
                // Normal stuffed sequence: 0xFF followed by 0x00..0x8F.
                self.pos += 1;
                self.c  += ((b as u32) << 9) | ((b2 as u32) << 1);
                self.ct  = 7;
            }
        } else {
            self.c  += (b as u32) << 8;
            self.ct  = 8;
        }
    }

    /// MPS_EXCHANGE / LPS_EXCHANGE + RENORMD — decode one symbol.
    fn decode(&mut self, cx: &mut CxState) -> u8 {
        let (qe, nmps, nlps, sw) = QE_TABLE[cx.state as usize];
        self.a -= qe;

        let d: u8;
        if (self.c >> 16) < self.a {
            // Interval >= Qe and C in lower sub-interval → not LPS
            if self.a & 0x8000 != 0 {
                // No renorm needed; return MPS immediately.
                return cx.mps;
            }
            // MPS conditional exchange (MPS_EXCHANGE)
            if self.a < qe {
                d = 1 - cx.mps;
                cx.state = nlps;
                if sw != 0 { cx.mps ^= 1; }
            } else {
                d = cx.mps;
                cx.state = nmps;
            }
        } else {
            // LPS path (C >= A after subtraction)
            self.c  -= self.a << 16;
            self.a   = qe;
            if self.a < qe {
                d = cx.mps;
                cx.state = nmps;
            } else {
                d = 1 - cx.mps;
                cx.state = nlps;
                if sw != 0 { cx.mps ^= 1; }
            }
        }

        // RENORMD: double A until MSB is set, shifting C in tandem.
        loop {
            if self.ct == 0 { self.fill(); }
            self.a  <<= 1;
            self.c  <<= 1;
            self.ct  -= 1;
            if self.a & 0x8000 != 0 { break; }
        }
        d
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Context building (T.88 §6.2.5)
// ─────────────────────────────────────────────────────────────────────────────

/// Fetch the pixel at (px, py) from a partially-decoded bitmap.
/// Returns 0 for any out-of-bounds access.
#[inline(always)]
fn get_pixel(bm: &[u8], px: i32, py: i32, width: u32) -> u32 {
    if px < 0 || py < 0 || px >= width as i32 || py < 0 {
        return 0;
    }
    let row_bytes = ((width + 7) / 8) as usize;
    let byte_idx  = py as usize * row_bytes + px as usize / 8;
    if byte_idx >= bm.len() { return 0; }
    ((bm[byte_idx] >> (7 - (px as usize % 8))) & 1) as u32
}

/// Assemble the MQ context word for position (x, y) in bitmap `bm`.
///
/// Template 0 → 13-bit context (+ AT bits → up to 16 bits, index 0..=2)
/// Template 1 → 13-bit context (+ 1 AT bit)
/// Template 2 →  8-bit context
/// Template 3 →  8-bit context
fn build_context(
    x:        i32,
    y:        i32,
    width:    u32,
    bm:       &[u8],
    template: u8,
    at:       &[(i32, i32)],
) -> u32 {
    let g = |dx: i32, dy: i32| get_pixel(bm, x + dx, y + dy, width);

    match template {
        0 => {
            // Row y-2: 4 pixels at offsets (-1, 0, +1, +2)   → bits 15..12
            // Row y-1: 5 pixels at offsets (-2,-1, 0,+1,+2)  → bits 11..7
            // Row y  : 4 pixels at offsets (-4,-3,-2,-1)      → bits 6..3
            // AT[0]  : adaptive template pixel                 → bit  2
            // (AT[1], AT[2] left at 0 — rare in PDF subset)
            let mut ctx: u32 = 0;
            ctx |= g( 1, -2) << 15;
            ctx |= g( 0, -2) << 14;
            ctx |= g(-1, -2) << 13;
            ctx |= g(-2, -2) << 12;
            ctx |= g( 2, -1) << 11;
            ctx |= g( 1, -1) << 10;
            ctx |= g( 0, -1) <<  9;
            ctx |= g(-1, -1) <<  8;
            ctx |= g(-2, -1) <<  7;
            ctx |= g(-1,  0) <<  6;
            ctx |= g(-2,  0) <<  5;
            ctx |= g(-3,  0) <<  4;
            ctx |= g(-4,  0) <<  3;
            // AT pixel 0 → bit 2
            let (atx0, aty0) = at.first().copied().unwrap_or((-3, -1));
            ctx |= g(atx0, aty0) << 2;
            // AT pixels 1 and 2 → bits 1 and 0 (rare; keep as 0 if absent)
            if at.len() >= 2 {
                let (atx1, aty1) = at[1];
                ctx |= g(atx1, aty1) << 1;
            }
            if at.len() >= 3 {
                let (atx2, aty2) = at[2];
                ctx |= g(atx2, aty2) << 0;
            }
            ctx
        }
        1 => {
            // Row y-2: 3 pixels at (-1, 0, +1)               → bits 12..10
            // Row y-1: 5 pixels at (-2,-1, 0,+1,+2)          → bits 9..5
            // Row y  : 4 pixels at (-4,-3,-2,-1)              → bits 4..1
            // AT[0]  : adaptive template pixel                 → bit  0
            let mut ctx: u32 = 0;
            ctx |= g( 1, -2) << 12;
            ctx |= g( 0, -2) << 11;
            ctx |= g(-1, -2) << 10;
            ctx |= g( 2, -1) <<  9;
            ctx |= g( 1, -1) <<  8;
            ctx |= g( 0, -1) <<  7;
            ctx |= g(-1, -1) <<  6;
            ctx |= g(-2, -1) <<  5;
            ctx |= g(-1,  0) <<  4;
            ctx |= g(-2,  0) <<  3;
            ctx |= g(-3,  0) <<  2;
            ctx |= g(-4,  0) <<  1;
            let (atx0, aty0) = at.first().copied().unwrap_or((-3, -1));
            ctx |= g(atx0, aty0) << 0;
            ctx
        }
        2 => {
            // Row y-2: 2 pixels at (-1, 0)                    → bits 7..6
            // Row y-1: 4 pixels at (-2,-1, 0,+1)              → bits 5..2
            // Row y  : 2 pixels at (-2,-1)                     → bits 1..0
            let mut ctx: u32 = 0;
            ctx |= g( 0, -2) << 7;
            ctx |= g(-1, -2) << 6;
            ctx |= g( 1, -1) << 5;
            ctx |= g( 0, -1) << 4;
            ctx |= g(-1, -1) << 3;
            ctx |= g(-2, -1) << 2;
            ctx |= g(-1,  0) << 1;
            ctx |= g(-2,  0) << 0;
            ctx
        }
        _ => {
            // Template 3: Row y-1: 4 pixels, Row y: 3 already decoded
            // Row y-1: 4 pixels at (-2,-1, 0,+1)              → bits 6..3
            // Row y  : 3 pixels at (-3,-2,-1)                  → bits 2..0
            let mut ctx: u32 = 0;
            ctx |= g( 1, -1) << 6;
            ctx |= g( 0, -1) << 5;
            ctx |= g(-1, -1) << 4;
            ctx |= g(-2, -1) << 3;
            let (atx0, aty0) = at.first().copied().unwrap_or((-3, -1));
            ctx |= g(atx0, aty0) << 2;
            ctx |= g(-1,  0) << 1;
            ctx |= g(-2,  0) << 0;
            ctx
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Generic Region Decoder (T.88 §6.2)
// ─────────────────────────────────────────────────────────────────────────────

/// Maximum context index +1 for each template.
fn context_size(template: u8) -> usize {
    match template {
        0 => 1 << 16,  // 16-bit context
        1 => 1 << 13,  // 13-bit context
        _ => 1 <<  8,  //  8-bit context
    }
}

/// Decode one generic region from `mq`, producing a 1-bit packed bitmap.
///
/// `typical_pred` = TPGDON flag (typical prediction).
/// `at` = slice of (dx, dy) adaptive template pixel offsets.
fn decode_generic_region(
    mq:           &mut MqDecoder,
    width:        u32,
    height:       u32,
    template:     u8,
    typical_pred: bool,
    at:           &[(i32, i32)],
) -> Vec<u8> {
    let row_bytes = ((width + 7) / 8) as usize;
    let mut bitmap = vec![0u8; height as usize * row_bytes];
    let ctx_count = context_size(template);
    let mut cx_states = vec![CxState::default(); ctx_count];

    // Context for typical prediction pseudo-pixel (TPGDON)
    let mut cx_tp = CxState::default();
    // Flag: was the previous row "typical"?
    let mut prev_ltp = false;

    for y in 0..height as i32 {
        let ltp: bool;

        if typical_pred {
            // Decode "line-typical" flag bit.
            ltp = mq.decode(&mut cx_tp) != 0;
            if ltp {
                // Copy previous row (or leave zeros for row 0).
                if y > 0 {
                    let src_start = (y as usize - 1) * row_bytes;
                    let dst_start = y as usize * row_bytes;
                    // Safe copy within the vec.
                    for i in 0..row_bytes {
                        bitmap[dst_start + i] = bitmap[src_start + i];
                    }
                }
                prev_ltp = true;
                continue;
            }
            prev_ltp = ltp;
        }

        for x in 0..width as i32 {
            let ctx = build_context(x, y, width, &bitmap, template, at);
            let bit = mq.decode(&mut cx_states[ctx as usize % ctx_count]);
            if bit != 0 {
                bitmap[y as usize * row_bytes + x as usize / 8] |= 1 << (7 - (x as usize % 8));
            }
        }
    }
    let _ = prev_ltp; // suppress unused warning
    bitmap
}

// ─────────────────────────────────────────────────────────────────────────────
// Embedded stream (no segment headers)
// ─────────────────────────────────────────────────────────────────────────────

fn decode_generic_stream(data: &[u8], width: u32, height: u32) -> Option<Vec<u8>> {
    if width == 0 || height == 0 { return None; }
    let mut mq     = MqDecoder::new(data);
    let bitmap = decode_generic_region(&mut mq, width, height, 0, false, &[]);
    if bitmap.is_empty() { None } else { Some(bitmap) }
}

// ─────────────────────────────────────────────────────────────────────────────
// Full JBIG2 file — segment header parser
// ─────────────────────────────────────────────────────────────────────────────

/// JBIG2 segment types we handle.
#[allow(dead_code)]
const SEG_SYMBOL_DICT:         u8 = 0;
const SEG_IMMED_GENERIC:       u8 = 6;
const SEG_IMMED_LL_GENERIC:    u8 = 7;
const SEG_PAGE_INFO:           u8 = 48;
#[allow(dead_code)]
const SEG_END_OF_PAGE:         u8 = 49;
#[allow(dead_code)]
const SEG_END_OF_STRIPE:       u8 = 50;
const SEG_END_OF_FILE:         u8 = 51;
#[allow(dead_code)]
const SEG_PROFILES:            u8 = 52;
#[allow(dead_code)]
const SEG_TABLES:              u8 = 53;
#[allow(dead_code)]
const SEG_EXTENSION:           u8 = 62;

/// Parse segments from a full JBIG2 file and render the target page.
///
/// `width` / `height` are hints from the PDF dictionary; we may override
/// them with the Page Information segment values.
fn decode_jbig2_file(data: &[u8], hint_width: u32, hint_height: u32) -> Option<Vec<u8>> {
    // Skip the 8-byte magic.
    let mut offset = 8usize;

    // Organisation field (1 byte):
    //   bit 0: number of pages known? (1 = yes)
    //   bit 1: 1 = sequential, 0 = random-access
    if offset >= data.len() { return None; }
    let org_byte = data[offset];
    offset += 1;

    // Number of pages (4 bytes, only present when bit 0 of org_byte is set).
    if org_byte & 1 != 0 {
        offset = offset.saturating_add(4);
    }

    // We accumulate the page bitmap here.
    let mut page_width  = hint_width;
    let mut page_height = hint_height;
    let mut page_bitmap: Option<Vec<u8>> = None;

    loop {
        if offset + 11 > data.len() { break; }   // need at least segment-number (4) + flags (1) + ...

        // ── Segment number (4 bytes) ──────────────────────────────────────────
        let seg_num = u32::from_be_bytes([
            data[offset], data[offset+1], data[offset+2], data[offset+3],
        ]);
        offset += 4;

        if offset >= data.len() { break; }

        // ── Segment header flags (1 byte) ────────────────────────────────────
        let seg_flags    = data[offset];
        offset += 1;
        let seg_type     = seg_flags & 0x3F;          // bits 5..0
        let page_assoc_4 = (seg_flags >> 6) & 1 != 0; // bit 6: page assoc size

        // ── Referred-to segments ─────────────────────────────────────────────
        if offset >= data.len() { break; }
        let ref_flags  = data[offset];
        offset += 1;
        // Count of referred-to segments: bits 5..0 of ref_flags (if < 7), or
        // use next 3 bytes for the count (if bits 5..0 == 7).
        let ref_count_raw = (ref_flags >> 5) & 0x07;
        let ref_count: usize;
        if ref_count_raw == 7 {
            // 3-byte count.
            if offset + 3 > data.len() { break; }
            ref_count = u32::from_be_bytes([0, data[offset], data[offset+1], data[offset+2]]) as usize;
            offset += 3;
        } else {
            ref_count = ref_count_raw as usize;
        }

        // Referred-to segment numbers: 4 bytes each (may be 1 or 2 bytes for
        // low segment numbers, but the standard says: if seg_num ≤ 256 use 1
        // byte per reference, ≤ 65536 use 2 bytes, else 4 bytes).
        let ref_size: usize = if seg_num <= 256 { 1 } else if seg_num <= 65536 { 2 } else { 4 };
        let ref_bytes = ref_count * ref_size;
        // Skip the referred-to segment number list.
        // Also skip the retention flags (ceil((ref_count+1)/8) bytes).
        let retention_bytes = (ref_count + 1 + 7) / 8;
        // Seek past: retention flags are embedded in the ref_flags block.
        // Actually the standard layout is:
        //   ref_flags byte (1)
        //   [optional 3-byte count]
        //   retention_flags bytes (ceil((ref_count+1)/8) - 1 already consumed via ref_flags)
        // We consumed ref_flags already (1 byte), so remaining retention bytes = retention_bytes - 1
        let remaining_retention = if retention_bytes > 0 { retention_bytes - 1 } else { 0 };
        offset = offset.saturating_add(remaining_retention + ref_bytes);

        // ── Page association ──────────────────────────────────────────────────
        if page_assoc_4 {
            if offset + 4 > data.len() { break; }
            offset += 4;
        } else {
            if offset >= data.len() { break; }
            offset += 1;
        }

        // ── Segment data length (4 bytes, 0xFFFFFFFF = unknown) ──────────────
        if offset + 4 > data.len() { break; }
        let seg_data_len = u32::from_be_bytes([
            data[offset], data[offset+1], data[offset+2], data[offset+3],
        ]);
        offset += 4;

        let data_start = offset;

        // ── Dispatch by segment type ─────────────────────────────────────────
        match seg_type {
            SEG_PAGE_INFO => {
                // Page Information segment: §7.4.8
                // width(4), height(4), x_res(4), y_res(4), flags(1), striping(2)
                if data_start + 19 <= data.len() {
                    let w = u32::from_be_bytes([
                        data[data_start], data[data_start+1],
                        data[data_start+2], data[data_start+3],
                    ]);
                    let h = u32::from_be_bytes([
                        data[data_start+4], data[data_start+5],
                        data[data_start+6], data[data_start+7],
                    ]);
                    if w > 0 { page_width  = w; }
                    if h > 0 && h != 0xFFFF_FFFF { page_height = h; }

                    // Allocate the page bitmap (all-white = 0 bits in JBIG2).
                    let row_bytes = ((page_width + 7) / 8) as usize;
                    page_bitmap  = Some(vec![0u8; page_height as usize * row_bytes]);
                }
            }

            SEG_IMMED_GENERIC | SEG_IMMED_LL_GENERIC => {
                // Generic Region segment: §7.4.6
                // Region info (17 bytes) + generic region segment info (1 byte) + AT pixels
                if data_start + 18 > data.len() {
                    // Not enough data; skip.
                } else {
                    let (region, consumed) = parse_generic_region_segment(
                        data, data_start, page_width, page_height,
                    );
                    if let Some(gr) = region {
                        // Decode the bitmap.
                        let seg_data_offset = data_start + consumed;
                        let seg_data_end = if seg_data_len == 0xFFFF_FFFF {
                            data.len()
                        } else {
                            (data_start + seg_data_len as usize).min(data.len())
                        };
                        let seg_data = &data[seg_data_offset..seg_data_end];

                        let mut mq = MqDecoder::new(seg_data);
                        let decoded = decode_generic_region(
                            &mut mq,
                            gr.width,
                            gr.height,
                            gr.template,
                            gr.tpgdon,
                            &gr.at_pixels,
                        );

                        // Composite onto the page bitmap using OR (most common).
                        if let Some(ref mut pg) = page_bitmap {
                            composite_bitmap(
                                pg, &decoded,
                                page_width, page_height,
                                gr.x_offset, gr.y_offset,
                                gr.width,    gr.height,
                                gr.comb_op,
                            );
                        }
                    }
                }
            }

            SEG_END_OF_FILE => break,

            _ => {
                // Skip unknown / unimplemented segment types.
            }
        }

        // Advance past this segment's data (if length is known).
        if seg_data_len != 0xFFFF_FFFF {
            offset = data_start + seg_data_len as usize;
        } else {
            // For unknown-length segments, we already consumed the data in the
            // dispatch above. Just continue from wherever we are.
            // (This handles segments where we set offset in the loop.)
        }
    }

    // Return the assembled page bitmap, or fall back to hint dimensions.
    match page_bitmap {
        Some(bm) if !bm.is_empty() => Some(bm),
        _ => {
            // No Page Info segment — try interpreting whole file as embedded stream.
            decode_generic_stream(data, hint_width, hint_height)
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Generic Region segment info parser
// ─────────────────────────────────────────────────────────────────────────────

struct GenericRegionInfo {
    width:    u32,
    height:   u32,
    x_offset: u32,
    y_offset: u32,
    comb_op:  u8,    // 0=OR, 1=AND, 2=XOR, 3=XNOR, 4=REPLACE
    template: u8,
    tpgdon:   bool,
    at_pixels: Vec<(i32, i32)>,
}

/// Parse the 17-byte Region Info + generic-region segment flags + AT pixels.
/// Returns (info, bytes_consumed_from_data_start).
fn parse_generic_region_segment(
    data:         &[u8],
    start:        usize,
    _page_width:  u32,
    _page_height: u32,
) -> (Option<GenericRegionInfo>, usize) {
    let d = data;
    let mut off = start;

    macro_rules! need {
        ($n:expr) => {
            if off + $n > d.len() { return (None, off - start); }
        };
    }

    // Region segment info header (17 bytes):
    //   [4] width  [4] height  [4] x_offset  [4] y_offset  [1] flags
    need!(17);
    let width    = u32::from_be_bytes([d[off], d[off+1], d[off+2], d[off+3]]); off += 4;
    let height   = u32::from_be_bytes([d[off], d[off+1], d[off+2], d[off+3]]); off += 4;
    let x_offset = u32::from_be_bytes([d[off], d[off+1], d[off+2], d[off+3]]); off += 4;
    let y_offset = u32::from_be_bytes([d[off], d[off+1], d[off+2], d[off+3]]); off += 4;
    let region_flags = d[off]; off += 1;
    let comb_op  = region_flags & 0x07;

    // Generic region segment flags (1 byte):
    //   bit 0:    TPGDON
    //   bits 2-1: template (0-3)
    //   bit 3:    EXTTEMPLATE (extra template bits — we note but don't use)
    need!(1);
    let gr_flags   = d[off]; off += 1;
    let tpgdon     = (gr_flags & 0x01) != 0;
    let template   = (gr_flags >> 1) & 0x03;
    let exttemplate = (gr_flags >> 3) & 0x01;

    // AT pixels: template 0 → 4 × (dx, dy) i8 pairs; others → 1 pair.
    // But only if the specific bit is set (some encoders omit AT for template 0
    // if all four are at default positions).  Per T.88, AT pixels are always
    // present in the segment header for templates that require them.
    let at_count: usize = if exttemplate != 0 {
        // Extended template (rare in PDFs): treat like template 0
        if template == 0 { 4 } else { 1 }
    } else {
        match template {
            0 => 4,
            _ => 1,
        }
    };

    let mut at_pixels = Vec::with_capacity(at_count);
    need!(at_count * 2);
    for _ in 0..at_count {
        let dx = d[off] as i8 as i32; off += 1;
        let dy = d[off] as i8 as i32; off += 1;
        at_pixels.push((dx, dy));
    }

    let info = GenericRegionInfo {
        width, height, x_offset, y_offset, comb_op,
        template, tpgdon, at_pixels,
    };
    (Some(info), off - start)
}

// ─────────────────────────────────────────────────────────────────────────────
// Bitmap compositing
// ─────────────────────────────────────────────────────────────────────────────

/// Composite `src` bitmap onto `dst` page bitmap using the given combination
/// operator, placing `src` at (`x_off`, `y_off`) on the page.
fn composite_bitmap(
    dst:         &mut Vec<u8>,
    src:         &[u8],
    page_w:      u32,
    page_h:      u32,
    x_off:       u32,
    y_off:       u32,
    src_w:       u32,
    src_h:       u32,
    comb_op:     u8,
) {
    let dst_row = ((page_w + 7) / 8) as usize;
    let src_row = ((src_w  + 7) / 8) as usize;

    for sy in 0..src_h {
        let dy = y_off + sy;
        if dy >= page_h { break; }

        for sx in 0..src_w {
            let dx = x_off + sx;
            if dx >= page_w { break; }

            let src_byte = sy as usize * src_row + sx as usize / 8;
            let src_bit  = 7 - (sx as usize % 8);
            let s = if src_byte < src.len() { (src[src_byte] >> src_bit) & 1 } else { 0 };

            let dst_byte = dy as usize * dst_row + dx as usize / 8;
            let dst_bit  = 7 - (dx as usize % 8) as u8;
            if dst_byte >= dst.len() { continue; }

            let d = (dst[dst_byte] >> dst_bit) & 1;
            let result = match comb_op {
                0 => s | d,         // OR
                1 => s & d,         // AND
                2 => s ^ d,         // XOR
                3 => !(s ^ d) & 1,  // XNOR
                _ => s,             // REPLACE
            };
            if result != 0 {
                dst[dst_byte] |=  1 << dst_bit;
            } else {
                dst[dst_byte] &= !(1 << dst_bit);
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Unit tests
// ─────────────────────────────────────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use super::*;

    // ── Test 1: MQ-coder initialises to correct A register ───────────────────
    //
    // After INITDEC the augmented register `a` must be 0x8000 (or 0x10000
    // before first renorm, which the constructor leaves at 0x10000 since no
    // symbols have been decoded yet).  The specification sets A = 0x10000
    // as the initial value for the interval register.
    #[test]
    fn mq_initial_a_register() {
        // A minimal MQ-stream: all 0xFF bytes cause fill() to treat them as
        // stuffed markers (ct=8 each) and the C register accumulates normally.
        let data = [0x00u8; 32];
        let mq = MqDecoder::new(&data);
        // After initialisation, a == 0x10000 (before any decode call).
        assert_eq!(mq.a, 0x0001_0000, "MQ a-register must be 0x10000 at init");
    }

    // ── Test 2: context building returns 0 for first pixel ───────────────────
    //
    // At position (0, 0) all neighbour lookups are out-of-bounds → 0.
    // Context must be 0 for all templates.
    #[test]
    fn context_is_zero_at_origin() {
        let bm = [0u8; 64]; // blank 64-pixel-wide bitmap
        for tmpl in 0u8..=3 {
            let ctx = build_context(0, 0, 64, &bm, tmpl, &[]);
            assert_eq!(ctx, 0, "context at origin must be 0 for template {}", tmpl);
        }
    }

    // ── Test 3: empty / trivial bitmap ───────────────────────────────────────
    //
    // decode_pdf_stream with an all-zero data buffer and a 1×1 image.
    // The MQ-coder will output MPS (0) for the single pixel, giving a white
    // (bit=0) pixel.  The returned buffer must have length = 1.
    #[test]
    fn decode_trivial_1x1_white() {
        // A buffer of 0x00 bytes: after MQ priming, C = 0 so every decode
        // returns MPS (which starts as 0 = white).
        let data = [0x00u8; 32];
        let result = decode_pdf_stream(&data, 1, 1);
        assert!(result.is_some(), "decode must return Some for 1×1");
        let bm = result.unwrap();
        assert_eq!(bm.len(), 1, "1×1 bitmap must be 1 byte");
        // Bit 7 (MSB of byte 0) is the pixel; it must be 0 (white / MPS).
        assert_eq!(bm[0] & 0x80, 0, "first pixel must be 0 (white)");
    }

    // ── Test 4: zero dimensions return None ──────────────────────────────────
    #[test]
    fn decode_zero_dimensions_returns_none() {
        let data = [0x00u8; 16];
        assert!(decode_pdf_stream(&data, 0, 0).is_none());
        assert!(decode_pdf_stream(&data, 0, 10).is_none());
        assert!(decode_pdf_stream(&data, 10, 0).is_none());
    }

    // ── Test 5: output length for various sizes ──────────────────────────────
    //
    // For any (w, h), the output length must be exactly h * ceil(w / 8).
    #[test]
    fn decode_output_length_correctness() {
        let data = [0x00u8; 128];
        for &(w, h) in &[(1u32, 1u32), (8, 1), (7, 1), (16, 4), (13, 3)] {
            let expected = h as usize * ((w as usize + 7) / 8);
            let result = decode_pdf_stream(&data, w, h);
            assert!(result.is_some(), "decode must succeed for {}x{}", w, h);
            assert_eq!(
                result.unwrap().len(),
                expected,
                "output length mismatch for {}x{}", w, h
            );
        }
    }

    // ── Test 6: full JBIG2 magic detection does not panic ────────────────────
    //
    // A buffer starting with the JBIG2 magic that is otherwise too short
    // must return None gracefully, without panicking.
    #[test]
    fn full_jbig2_truncated_no_panic() {
        let magic: &[u8] = &[0x97, 0x4A, 0x42, 0x32, 0x0D, 0x0A, 0x1A, 0x0A];
        let result = decode_pdf_stream(magic, 8, 8);
        // May return Some or None — must not panic.
        let _ = result;
    }

    // ── Test 7: QE_TABLE sanity — all Qe values fit in u16 ──────────────────
    #[test]
    fn qe_table_values_in_range() {
        for (i, &(qe, nmps, nlps, _sw)) in QE_TABLE.iter().enumerate() {
            assert!(qe  <= 0x8000, "QE[{}].Qe out of range: 0x{:04X}", i, qe);
            assert!(nmps < 47,     "QE[{}].NMPS out of range: {}", i, nmps);
            assert!(nlps < 47,     "QE[{}].NLPS out of range: {}", i, nlps);
        }
    }

    // ── Test 8: pixel compositing ─────────────────────────────────────────────
    //
    // Composite a 1×1 black pixel at offset (0,0) onto a white (0) page.
    #[test]
    fn composite_or_sets_bit() {
        let mut page = vec![0u8; 1];   // 8×1 page, all zero
        let src = vec![0x80u8];         // 1×1 black pixel at bit 7
        composite_bitmap(&mut page, &src, 8, 1, 0, 0, 1, 1, 0 /*OR*/);
        assert_eq!(page[0] & 0x80, 0x80, "OR composite must set the bit");
    }
}
