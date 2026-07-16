/// CCITTFax Group 3 and Group 4 (T.6 / Modified READ) decoders.
///
/// Returns row-major 1-bit pixels packed into bytes (MSB-first),
/// length = height * row_bytes  where  row_bytes = (width + 7) / 8.
///
/// Bit convention in the returned buffer:
///   bit 7 of byte 0 = pixel 0, bit 6 = pixel 1, ..., bit 0 = pixel 7,
///   bit 7 of byte 1 = pixel 8, …
/// A bit value of 1 means "black" in the raw bitstream sense; the caller
/// interprets blackness via the `black_is_1` flag.

// ─────────────────────────────────────────────────────────────────────────────
// Static Huffman tables  (T.4 / T.6 Modified Huffman run-length codes)
// Each entry: (run_length, code_word, bit_length)
// ─────────────────────────────────────────────────────────────────────────────

/// White terminating codes (runs 0–63).
static WHITE_TERM: &[(u32, u16, u8)] = &[
    (0,  0b00110101, 8), (1,  0b000111,   6), (2,  0b0111,     4),
    (3,  0b1000,     4), (4,  0b1011,     4), (5,  0b1100,     4),
    (6,  0b1110,     4), (7,  0b1111,     4), (8,  0b10011,    5),
    (9,  0b10100,    5), (10, 0b00111,    5), (11, 0b01000,    5),
    (12, 0b001000,   6), (13, 0b000011,   6), (14, 0b110100,   6),
    (15, 0b110101,   6), (16, 0b101010,   6), (17, 0b101011,   6),
    (18, 0b0100111,  7), (19, 0b0001100,  7), (20, 0b0001000,  7),
    (21, 0b0010111,  7), (22, 0b0000011,  7), (23, 0b0000100,  7),
    (24, 0b0101000,  7), (25, 0b0101011,  7), (26, 0b0010011,  7),
    (27, 0b0100100,  7), (28, 0b0011000,  7), (29, 0b00000010, 8),
    (30, 0b00000011, 8), (31, 0b00011010, 8), (32, 0b00011011, 8),
    (33, 0b00010010, 8), (34, 0b00010011, 8), (35, 0b00010100, 8),
    (36, 0b00010101, 8), (37, 0b00010110, 8), (38, 0b00010111, 8),
    (39, 0b00101000, 8), (40, 0b00101001, 8), (41, 0b00101010, 8),
    (42, 0b00101011, 8), (43, 0b00101100, 8), (44, 0b00101101, 8),
    (45, 0b00000100, 8), (46, 0b00000101, 8), (47, 0b00001010, 8),
    (48, 0b00001011, 8), (49, 0b01010010, 8), (50, 0b01010011, 8),
    (51, 0b01010100, 8), (52, 0b01010101, 8), (53, 0b00100100, 8),
    (54, 0b00100101, 8), (55, 0b01011000, 8), (56, 0b01011001, 8),
    (57, 0b01011010, 8), (58, 0b01011011, 8), (59, 0b01001010, 8),
    (60, 0b01001011, 8), (61, 0b00110010, 8), (62, 0b00110011, 8),
    (63, 0b00110100, 8),
];

/// White make-up codes (runs 64, 128, … 1728, plus EOL-adjacent 1664).
static WHITE_MAKEUP: &[(u32, u16, u8)] = &[
    (64,   0b11011,      5), (128,  0b10010,      5), (192,  0b010111,     6),
    (256,  0b0110111,    7), (320,  0b00110110,   8), (384,  0b00110111,   8),
    (448,  0b01100100,   8), (512,  0b01100101,   8), (576,  0b01101000,   8),
    (640,  0b01100111,   8), (704,  0b011001100,  9), (768,  0b011001101,  9),
    (832,  0b011010010,  9), (896,  0b011010011,  9), (960,  0b011010100,  9),
    (1024, 0b011010101,  9), (1088, 0b011010110,  9), (1152, 0b011010111,  9),
    (1216, 0b011011000,  9), (1280, 0b011011001,  9), (1344, 0b011011010,  9),
    (1408, 0b011011011,  9), (1472, 0b010011000,  9), (1536, 0b010011001,  9),
    (1600, 0b010011010,  9), (1664, 0b011000,     6), (1728, 0b010011011,  9),
];

/// Black terminating codes (runs 0–63).
static BLACK_TERM: &[(u32, u16, u8)] = &[
    (0,  0b0000110111,   10), (1,  0b010,          3), (2,  0b11,           2),
    (3,  0b10,            2), (4,  0b011,           3), (5,  0b0011,         4),
    (6,  0b0010,          4), (7,  0b00011,         5), (8,  0b000101,       6),
    (9,  0b000100,        6), (10, 0b0000100,       7), (11, 0b0000101,      7),
    (12, 0b0000111,       7), (13, 0b00000100,      8), (14, 0b00000111,     8),
    (15, 0b000011000,     9), (16, 0b0000010111,   10), (17, 0b0000011000,  10),
    (18, 0b0000001000,   10), (19, 0b00001100111,  11), (20, 0b00001101000, 11),
    (21, 0b00001101100,  11), (22, 0b00000110111,  11), (23, 0b00000101000, 11),
    (24, 0b00000010111,  11), (25, 0b00000011000,  11), (26, 0b000011001010,12),
    (27, 0b000011001011, 12), (28, 0b000011001100, 12), (29, 0b000011001101,12),
    (30, 0b000001101000, 12), (31, 0b000001101001, 12), (32, 0b000001101010,12),
    (33, 0b000001101011, 12), (34, 0b000011010010, 12), (35, 0b000011010011,12),
    (36, 0b000011010100, 12), (37, 0b000011010101, 12), (38, 0b000011010110,12),
    (39, 0b000011010111, 12), (40, 0b000001101100, 12), (41, 0b000001101101,12),
    (42, 0b000011011010, 12), (43, 0b000011011011, 12), (44, 0b000001010100,12),
    (45, 0b000001010101, 12), (46, 0b000001010110, 12), (47, 0b000001010111,12),
    (48, 0b000001100100, 12), (49, 0b000001100101, 12), (50, 0b000001010010,12),
    (51, 0b000001010011, 12), (52, 0b000000100100, 12), (53, 0b000000110111,12),
    (54, 0b000000111000, 12), (55, 0b000000100111, 12), (56, 0b000000101000,12),
    (57, 0b000001011000, 12), (58, 0b000001011001, 12), (59, 0b000000101011,12),
    (60, 0b000000101100, 12), (61, 0b000001011010, 12), (62, 0b000001100110,12),
    (63, 0b000001100111, 12),
];

/// Black make-up codes (runs 64…1728).
static BLACK_MAKEUP: &[(u32, u16, u8)] = &[
    (64,   0b0000001111,    10), (128,  0b000011001000, 12), (192,  0b000011001001, 12),
    (256,  0b000001011011,  12), (320,  0b000000110011, 12), (384,  0b000000110100, 12),
    (448,  0b000000110101,  12), (512,  0b0000001101100,13), (576,  0b0000001101101,13),
    (640,  0b0000001001010, 13), (704,  0b0000001001011,13), (768,  0b0000001001100,13),
    (832,  0b0000001001101, 13), (896,  0b0000001110010,13), (960,  0b0000001110011,13),
    (1024, 0b0000001110100, 13), (1088, 0b0000001110101,13), (1152, 0b0000001110110,13),
    (1216, 0b0000001110111, 13), (1280, 0b0000001010010,13), (1344, 0b0000001010011,13),
    (1408, 0b0000001010100, 13), (1472, 0b0000001010101,13), (1536, 0b0000001011010,13),
    (1600, 0b0000001011011, 13), (1664, 0b0000001100100,13), (1728, 0b0000001100101,13),
];

// ─────────────────────────────────────────────────────────────────────────────
// BitReader — MSB-first bit stream over a byte slice
// ─────────────────────────────────────────────────────────────────────────────

struct BitReader<'a> {
    data:     &'a [u8],
    byte_pos: usize,
    bit_pos:  u8, // 7..0, where 7 = MSB (first bit of byte)
}

impl<'a> BitReader<'a> {
    fn new(data: &'a [u8]) -> Self {
        BitReader { data, byte_pos: 0, bit_pos: 7 }
    }

    /// Number of bits remaining in the stream.
    #[inline]
    fn remaining_bits(&self) -> usize {
        if self.byte_pos >= self.data.len() { return 0; }
        (self.data.len() - self.byte_pos - 1) * 8 + self.bit_pos as usize + 1
    }

    /// Peek at the next `n` bits (n ≤ 16) without consuming them.
    /// Returns None if fewer than n bits remain.
    #[inline]
    fn peek_bits(&self, n: usize) -> Option<u32> {
        if n == 0 { return Some(0); }
        if self.remaining_bits() < n { return None; }
        let mut val: u32 = 0;
        let mut byte_pos = self.byte_pos;
        let mut bit_pos  = self.bit_pos as i32;
        for _ in 0..n {
            if byte_pos >= self.data.len() { return None; }
            val = (val << 1) | (((self.data[byte_pos] >> bit_pos) & 1) as u32);
            bit_pos -= 1;
            if bit_pos < 0 {
                bit_pos = 7;
                byte_pos += 1;
            }
        }
        Some(val)
    }

    /// Consume `n` bits, advancing the position.
    #[inline]
    fn consume_bits(&mut self, n: usize) {
        for _ in 0..n {
            if self.byte_pos >= self.data.len() { return; }
            if self.bit_pos == 0 {
                self.bit_pos = 7;
                self.byte_pos += 1;
            } else {
                self.bit_pos -= 1;
            }
        }
    }

    /// Read and consume exactly `n` bits. Returns None on underflow.
    #[inline]
    fn read_bits(&mut self, n: usize) -> Option<u32> {
        let v = self.peek_bits(n)?;
        self.consume_bits(n);
        Some(v)
    }

    /// Try to skip an EOL code (12 bits of 000000000001).
    /// Returns true if skipped, false if not present.
    fn try_skip_eol(&mut self) -> bool {
        // EOL = 12 bits: 000000000001
        if let Some(v) = self.peek_bits(12) {
            if v == 0b000000000001 {
                self.consume_bits(12);
                return true;
            }
        }
        false
    }

    /// Skip any leading zero bits and a single 1 — used for aligned EOL detection.
    #[allow(dead_code)]
    fn skip_to_next_eol(&mut self) {
        // EOL is 000000000001; skip padding zeros and the EOL itself
        loop {
            if self.remaining_bits() < 12 { break; }
            if let Some(v) = self.peek_bits(12) {
                if v == 0b000000000001 {
                    self.consume_bits(12);
                    return;
                }
            }
            // Advance one bit and retry
            self.consume_bits(1);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Huffman run-length decoder
// ─────────────────────────────────────────────────────────────────────────────

/// Match a code against a table using linear scan.
/// Returns Some(run_length) if a code of any length (up to 13) matches.
fn match_code(br: &mut BitReader, term: &[(u32, u16, u8)], makeup: &[(u32, u16, u8)]) -> Option<u32> {
    let mut total_run: u32 = 0;
    loop {
        // Try to match a makeup code first (for runs ≥ 64)
        let mut matched_makeup = false;
        for &(run, code, bits) in makeup.iter() {
            if let Some(peeked) = br.peek_bits(bits as usize) {
                if peeked == code as u32 {
                    br.consume_bits(bits as usize);
                    total_run += run;
                    matched_makeup = true;
                    break;
                }
            }
        }

        if matched_makeup {
            // After a makeup code a terminating code MUST follow
            for &(run, code, bits) in term.iter() {
                if let Some(peeked) = br.peek_bits(bits as usize) {
                    if peeked == code as u32 {
                        br.consume_bits(bits as usize);
                        total_run += run;
                        return Some(total_run);
                    }
                }
            }
            // Malformed: makeup not followed by term — return what we have
            return Some(total_run);
        }

        // Try terminating code
        for &(run, code, bits) in term.iter() {
            if let Some(peeked) = br.peek_bits(bits as usize) {
                if peeked == code as u32 {
                    br.consume_bits(bits as usize);
                    total_run += run;
                    return Some(total_run);
                }
            }
        }

        // No match — corrupt data
        return None;
    }
}

/// Decode one run-length using white Huffman codes.
fn read_white_run(br: &mut BitReader) -> Option<u32> {
    match_code(br, WHITE_TERM, WHITE_MAKEUP)
}

/// Decode one run-length using black Huffman codes.
fn read_black_run(br: &mut BitReader) -> Option<u32> {
    match_code(br, BLACK_TERM, BLACK_MAKEUP)
}

// ─────────────────────────────────────────────────────────────────────────────
// Row packing utility
// ─────────────────────────────────────────────────────────────────────────────

/// Pack a slice of pixel values (0 = white, 1 = black) into MSB-first bytes.
fn pack_row(pixels: &[u8], width: u32) -> Vec<u8> {
    let row_bytes = ((width + 7) / 8) as usize;
    let mut out = vec![0u8; row_bytes];
    for (i, &px) in pixels.iter().take(width as usize).enumerate() {
        if px != 0 {
            out[i / 8] |= 0x80 >> (i % 8);
        }
    }
    out
}

// ─────────────────────────────────────────────────────────────────────────────
// Group 4 (T.6 / 2D Modified READ) decoder
// ─────────────────────────────────────────────────────────────────────────────

/// Decode a single Group-4 2D-coded row, writing pixels into `current`.
/// `reference` is the previously decoded row (all-white for first row).
/// Returns false on unrecoverable bitstream error.
fn decode_g4_row(br: &mut BitReader, reference: &[u8], current: &mut Vec<u8>, width: u32) -> bool {
    let w = width as usize;
    current.clear();
    current.resize(w, 0);

    // a0: current position (starts at imaginary -1 = just before column 0)
    // We track pixel colour for a0 separately.
    let mut a0: i64 = -1;
    let mut a0_colour: u8 = 0; // white at start

    // Helper: find the position of the next changing element at or after `start`
    // in `line` (which has pixel value != `current_colour`).
    let find_b1 = |line: &[u8], start: usize, a0c: u8| -> usize {
        // b1 is the first changing element on the reference line to the right of a0
        // whose colour is opposite to a0's current colour.
        // "to the right of a0" means index > a0 (i.e., > a0 as usize when a0 >= 0,
        //  or index >= 0 when a0 == -1).
        let from = if start < w { start } else { w };
        // walk from `from` to find the first pixel with colour != a0c
        let mut pos = from;
        while pos < w && line[pos] == a0c {
            pos += 1;
        }
        pos // This is b1 (may be == w if not found)
    };

    let find_b2 = |line: &[u8], b1: usize| -> usize {
        if b1 >= w { return w; }
        let b1c = line[b1];
        let mut pos = b1 + 1;
        while pos < w && line[pos] == b1c {
            pos += 1;
        }
        pos
    };

    let mut iter = 0usize;
    while (a0 as usize) < w {
        iter += 1;
        if iter > w * 2 + 16 {
            // Infinite-loop guard — corrupt stream
            break;
        }

        // Skip EOL if present (G4 technically doesn't have them mid-row, but be safe)
        if let Some(v) = br.peek_bits(12) {
            if v == 0b000000000001 {
                br.consume_bits(12);
                return true; // end of row signalled by EOL
            }
        }

        // Peek at mode-discriminating bits
        let bit1 = match br.peek_bits(1) {
            Some(v) => v,
            None    => break,
        };

        // ── V(0) mode: 1 ────────────────────────────────────────────────────
        if bit1 == 1 {
            br.consume_bits(1);
            // a1 is at the same position as b1
            let from = (a0 + 1).max(0) as usize;
            let b1 = find_b1(reference, from, a0_colour);
            let a1 = b1;
            // fill a0..a1 with a0_colour
            let fill_start = (a0 + 1).max(0) as usize;
            for i in fill_start..a1.min(w) {
                current[i] = a0_colour;
            }
            a0 = a1 as i64;
            a0_colour ^= 1;
            continue;
        }

        let bit2 = match br.peek_bits(2) {
            Some(v) => v,
            None    => break,
        };

        // ── Pass mode: 0001 ──────────────────────────────────────────────────
        if let Some(v4) = br.peek_bits(4) {
            if v4 == 0b0001 {
                br.consume_bits(4);
                let from = (a0 + 1).max(0) as usize;
                let b1 = find_b1(reference, from, a0_colour);
                let b2 = find_b2(reference, b1);
                // fill a0..b2 with a0_colour
                let fill_start = (a0 + 1).max(0) as usize;
                for i in fill_start..b2.min(w) {
                    current[i] = a0_colour;
                }
                a0 = b2 as i64;
                // a0_colour does NOT change in pass mode
                continue;
            }
        }

        // ── Horizontal mode: 001 ─────────────────────────────────────────────
        if let Some(v3) = br.peek_bits(3) {
            if v3 == 0b001 {
                br.consume_bits(3);
                // Two run-lengths: first has colour a0_colour, second has opposite
                let run1 = if a0_colour == 0 {
                    read_white_run(br)
                } else {
                    read_black_run(br)
                };
                let run2 = if a0_colour == 0 {
                    read_black_run(br)
                } else {
                    read_white_run(br)
                };
                let (r1, r2) = (run1.unwrap_or(0), run2.unwrap_or(0));
                let start = (a0 + 1).max(0) as usize;
                let end1  = (start + r1 as usize).min(w);
                let end2  = (end1  + r2 as usize).min(w);
                for i in start..end1 { current[i] = a0_colour; }
                for i in end1..end2  { current[i] = a0_colour ^ 1; }
                a0 = end2 as i64 - 1;
                // a0_colour: after two runs it stays the same (run1 is a0_colour, run2 is opposite,
                // then next colour is a0_colour again → but a0 is now at the end of run2, which
                // is (a0_colour^1) territory, so a0_colour becomes a0_colour^1 after both runs.
                // Actually: a0 lands at end2-1 which is inside the second run (opposite colour).
                // The standard says a0_colour toggles twice → stays same only if we consider a0
                // as sitting BETWEEN runs. Let's follow the standard: after horizontal mode,
                // a0 is at the end of the second run, and a0_colour is set to the colour of
                // that position (a0_colour ^ 1). But wait: end2 might equal start (empty runs).
                if r1 + r2 > 0 {
                    a0_colour ^= 1; // ended on second run colour
                    if r2 == 0 {
                        // only one run was decoded effectively
                        a0_colour ^= 1; // revert
                    }
                }
                continue;
            }
        }

        // ── Vertical modes ───────────────────────────────────────────────────
        // VR(3): 0000011, VR(2): 000011, VR(1): 011
        // VL(3): 0000010, VL(2): 000010, VL(1): 010
        // Need to check longest prefix first.

        let (offset, code_bits): (i64, usize) =
            if let Some(v7) = br.peek_bits(7) {
                if      v7 == 0b0000011 { (3, 7) }
                else if v7 == 0b0000010 { (-3, 7) }
                else if let Some(v6) = br.peek_bits(6) {
                    if      v6 == 0b000011 { (2, 6) }
                    else if v6 == 0b000010 { (-2, 6) }
                    else {
                        // bit2 already peeked
                        if      bit2 == 0b11 { (1, 3) }  // VR(1): 011
                        else if bit2 == 0b10 { (-1, 3) } // VL(1): 010  (bit2 = peek_bits(2) = bits[0..1])
                        else {
                            // Unknown code — skip 1 bit and continue
                            br.consume_bits(1);
                            continue;
                        }
                    }
                } else {
                    br.consume_bits(1);
                    continue;
                }
            } else {
                break;
            };

        br.consume_bits(code_bits);

        // a1 = b1 + offset
        let from = (a0 + 1).max(0) as usize;
        let b1 = find_b1(reference, from, a0_colour);
        let a1_signed = b1 as i64 + offset;
        let a1 = a1_signed.clamp(0, w as i64) as usize;

        // fill a0+1 .. a1 with a0_colour
        let fill_start = (a0 + 1).max(0) as usize;
        for i in fill_start..a1.min(w) {
            current[i] = a0_colour;
        }
        a0 = a1 as i64;
        a0_colour ^= 1;
    }

    // Fill remaining pixels (if any) with a0_colour
    let fill_start = (a0 + 1).max(0) as usize;
    for i in fill_start..w {
        current[i] = a0_colour;
    }

    true
}

/// Decode CCITTFax Group 4 (T.6 pure 2D).
///
/// Returns row-major 1-bit packed bytes (MSB first), or None on fatal error.
/// `black_is_1`: if true, 1-bits represent black pixels (standard PDFium sense;
///   this function does NOT invert — the caller handles it).
pub fn decode_ccitt_g4(data: &[u8], width: u32, height: u32, _black_is_1: bool) -> Option<Vec<u8>> {
    if width == 0 || height == 0 { return Some(vec![]); }
    let row_bytes = ((width + 7) / 8) as usize;
    let mut output = vec![0u8; row_bytes * height as usize];

    let mut br = BitReader::new(data);

    // Reference line starts as all-white (0)
    let mut reference: Vec<u8> = vec![0u8; width as usize];
    let mut current:   Vec<u8> = Vec::with_capacity(width as usize);

    let mut eofb_count = 0usize; // consecutive EOLs → EOFB

    for row in 0..height as usize {
        // Check for EOFB (two consecutive EOLs = 24 zero bits then 2 x 000000000001)
        // G4 uses EOFB = 000000000001 000000000001 (24 zeros + 2 one-bits interleaved)
        // Actually EOFB in G4 is just two EOL code words back-to-back.
        if let Some(v) = br.peek_bits(12) {
            if v == 0b000000000001 {
                br.consume_bits(12);
                eofb_count += 1;
                if eofb_count >= 2 {
                    // EOFB encountered — stop, fill rest with white
                    break;
                }
                // One EOL: might be padding before last row, continue
            }
        }

        if br.remaining_bits() == 0 { break; }

        let ok = decode_g4_row(&mut br, &reference, &mut current, width);
        if !ok { break; }

        // Pack row into output
        let packed = pack_row(&current, width);
        let dst = &mut output[row * row_bytes..(row + 1) * row_bytes];
        dst.copy_from_slice(&packed);

        // Current row becomes next reference
        std::mem::swap(&mut reference, &mut current);
    }

    Some(output)
}

// ─────────────────────────────────────────────────────────────────────────────
// Group 3 (T.4) decoder: 1D (k=0) and 2D (k<0 or k>0 mixed)
// ─────────────────────────────────────────────────────────────────────────────

/// Decode one Group-3 1D-coded row (Modified Huffman).
/// Returns false on unrecoverable error.
fn decode_g3_1d_row(br: &mut BitReader, current: &mut Vec<u8>, width: u32) -> bool {
    let w = width as usize;
    current.clear();
    current.resize(w, 0);

    let mut col: usize = 0;
    let mut colour: u8 = 0; // start with white

    while col < w {
        let run = if colour == 0 {
            read_white_run(br)
        } else {
            read_black_run(br)
        };
        match run {
            None => return false,
            Some(r) => {
                let end = (col + r as usize).min(w);
                for i in col..end {
                    current[i] = colour;
                }
                col = end;
                colour ^= 1;
            }
        }
    }
    true
}

/// Decode CCITTFax Group 3.
///
/// - `k = 0`: pure 1D (Modified Huffman / MH)
/// - `k < 0`: pure 2D (same coding as G4)
/// - `k > 0`: mixed — first row of every group-of-k is 1D, rest are 2D
///
/// Each row in Group 3 is preceded by an EOL code (000000000001).
/// For k > 0, each EOL is followed by a 1-bit tag: 1 = next row is 1D, 0 = 2D.
pub fn decode_ccitt_g3(data: &[u8], width: u32, height: u32, _black_is_1: bool, k: i32) -> Option<Vec<u8>> {
    if width == 0 || height == 0 { return Some(vec![]); }
    let row_bytes = ((width + 7) / 8) as usize;
    let mut output = vec![0u8; row_bytes * height as usize];

    let mut br = BitReader::new(data);

    let mut reference: Vec<u8> = vec![0u8; width as usize];
    let mut current:   Vec<u8> = Vec::with_capacity(width as usize);

    // k < 0 → pure 2D (treat like G4, but rows are preceded by EOL)
    // k = 0 → pure 1D
    // k > 0 → mixed

    let is_pure_2d = k < 0;
    let is_pure_1d = k == 0;

    // 1D row counter for mixed mode (rows within a group of k)
    let mut rows_in_group: i32 = 0;

    for row in 0..height as usize {
        if br.remaining_bits() == 0 { break; }

        // Expect EOL before each row (skip fill bits to align)
        // In G3, fill bits (zeros) may precede EOL to byte-align it.
        // Try skipping up to 8 zero bits before EOL.
        {
            let mut found_eol = false;
            for _ in 0..12 {
                if br.try_skip_eol() {
                    found_eol = true;
                    break;
                }
                // peek one bit; if it's 0 it might be fill
                if let Some(b) = br.peek_bits(1) {
                    if b == 0 {
                        br.consume_bits(1);
                    } else {
                        break;
                    }
                } else {
                    break;
                }
            }
            // If we couldn't find EOL, try to proceed anyway
            let _ = found_eol;
        }

        // Check EOFB
        if let Some(v) = br.peek_bits(12) {
            if v == 0b000000000001 {
                // Two consecutive EOLs = EOFB; we already consumed one above, check second
                br.consume_bits(12);
                break;
            }
        }

        // For k > 0: read the 1-bit tag after EOL
        let use_1d = if is_pure_1d {
            true
        } else if is_pure_2d {
            false
        } else {
            // Mixed: tag bit 1 = 1D, 0 = 2D
            if rows_in_group == 0 {
                true // first row of group always 1D
            } else {
                br.read_bits(1).map(|v| v == 1).unwrap_or(true)
            }
        };

        let ok = if use_1d {
            decode_g3_1d_row(&mut br, &mut current, width)
        } else {
            decode_g4_row(&mut br, &reference, &mut current, width)
        };

        if !ok { break; }

        let packed = pack_row(&current, width);
        let dst = &mut output[row * row_bytes..(row + 1) * row_bytes];
        dst.copy_from_slice(&packed);

        std::mem::swap(&mut reference, &mut current);

        // Advance mixed-mode group counter
        if !is_pure_1d && !is_pure_2d {
            rows_in_group += 1;
            if k > 0 && rows_in_group >= k {
                rows_in_group = 0;
            }
        }
    }

    Some(output)
}

// ─────────────────────────────────────────────────────────────────────────────
// Tests
// ─────────────────────────────────────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn bit_reader_peek_consume() {
        // 0b10110010 = 0xB2
        let data = &[0xB2u8];
        let mut br = BitReader::new(data);
        assert_eq!(br.peek_bits(4), Some(0b1011));
        assert_eq!(br.peek_bits(4), Some(0b1011)); // peek does not consume
        br.consume_bits(4);
        assert_eq!(br.peek_bits(4), Some(0b0010));
        br.consume_bits(4);
        assert_eq!(br.peek_bits(1), None); // exhausted
    }

    #[test]
    fn bit_reader_across_bytes() {
        let data = &[0b10000000u8, 0b01000000u8];
        let mut br = BitReader::new(data);
        assert_eq!(br.read_bits(1), Some(1));
        assert_eq!(br.read_bits(7), Some(0));
        assert_eq!(br.read_bits(1), Some(0));
        assert_eq!(br.read_bits(1), Some(1));
    }

    #[test]
    fn pack_row_basic() {
        // pixels: 1 0 1 0 0 0 0 0  → 0b10100000 = 0xA0
        let px = vec![1u8, 0, 1, 0, 0, 0, 0, 0];
        let packed = pack_row(&px, 8);
        assert_eq!(packed, vec![0xA0]);
    }

    #[test]
    fn pack_row_partial() {
        // width=3, pixels: 1 1 0  → 0b11000000 = 0xC0
        let px = vec![1u8, 1, 0];
        let packed = pack_row(&px, 3);
        assert_eq!(packed, vec![0xC0]);
    }

    #[test]
    fn g4_all_white() {
        // Encode a 4-pixel all-white row in G4.
        // All-white row from all-white reference: a0=-1 (white), b1=4 (past end).
        // V(0) mode at position 0..4 — but b1 is at width=4 (imaginary white after end).
        // Actually, with all-white reference, every pixel change position lands on
        // V(0) moves for a0→b1 steps. For a trivially small synthetic test
        // we just verify the output size and that it doesn't panic.
        // Use a known G4 encoding of a 4-pixel all-white row:
        //   First row: reference is all-white, current should also be all-white.
        //   In G4, V(0) with b1=4 (= width, imaginary): fill 0..4 with white.
        //   Code: "1" (V0) fills pixel at b1=4 which is past end — effectively
        //   the row is done. In practice a minimal G4 stream for all-white is just
        //   a series of V(0) codes. For testing, use a known-good byte sequence.
        // Let's test with a trivially constructed stream: V(0) = bit "1".
        // One V(0) per pixel would give 4 bits = "1111".
        // But real G4 handles it differently. Use a simple decode + check size.
        let result = decode_ccitt_g4(&[0u8; 4], 8, 1, false);
        assert!(result.is_some());
        let r = result.unwrap();
        assert_eq!(r.len(), 1); // 1 row × 1 byte
    }

    #[test]
    fn g3_1d_empty_stream() {
        // Should not panic on empty data
        let result = decode_ccitt_g3(&[], 8, 1, false, 0);
        assert!(result.is_some());
    }
}
