// cmap.rs — Parse PDF ToUnicode CMap streams into char-code → Unicode char maps.
//
// Handles:
//   beginbfchar / endbfchar   : <src_hex> <dst_hex>
//   beginbfrange / endbfrange : <lo_hex> <hi_hex> <start_hex>
//                               <lo_hex> <hi_hex> [<c1_hex> <c2_hex> ...]
//
// Returns HashMap<u32, char>: char code (1–4 bytes big-endian) → Unicode scalar.

use std::collections::HashMap;

/// Parse a PDF ToUnicode CMap stream.  The input `data` is the raw (already
/// decompressed) stream bytes.  Returns a map from source code → Unicode char.
pub fn parse_cmap(data: &[u8]) -> HashMap<u32, char> {
    let mut map = HashMap::new();

    // Work with a &str view where possible; fall back gracefully on non-UTF-8.
    let text = String::from_utf8_lossy(data);

    // Split into lines and process state-machine style.
    let mut in_bfchar  = false;
    let mut in_bfrange = false;

    let lines: Vec<&str> = text.lines().collect();
    let mut i = 0;
    while i < lines.len() {
        let line = lines[i].trim();

        if line.contains("beginbfchar") {
            in_bfchar  = true;
            in_bfrange = false;
            i += 1;
            continue;
        }
        if line.contains("endbfchar") {
            in_bfchar = false;
            i += 1;
            continue;
        }
        if line.contains("beginbfrange") {
            in_bfrange = true;
            in_bfchar  = false;
            i += 1;
            continue;
        }
        if line.contains("endbfrange") {
            in_bfrange = false;
            i += 1;
            continue;
        }

        if in_bfchar {
            // Expect: <src_hex> <dst_hex>
            // Example: <0041> <0041>
            let tokens = collect_hex_tokens(line);
            if tokens.len() >= 2 {
                let src = parse_hex_token(&tokens[0]);
                if let Some(ch) = utf16be_to_char(&tokens[1]) {
                    map.insert(src, ch);
                }
            }
        } else if in_bfrange {
            // Expect: <lo_hex> <hi_hex> <start_hex>
            //    or:  <lo_hex> <hi_hex> [<c1_hex> <c2_hex> ...]
            // Collect all hex tokens on the line; check if an array follows.
            let tokens = collect_hex_tokens(line);
            if tokens.len() >= 3 && !line.contains('[') {
                // Scalar form: <lo> <hi> <start>
                let lo    = parse_hex_token(&tokens[0]);
                let hi    = parse_hex_token(&tokens[1]);
                let start = parse_hex_token(&tokens[2]);
                // start is a UTF-16BE code unit sequence; convert base char.
                let base_ch = utf16be_scalar(start);
                if let Some(base) = base_ch {
                    let count = hi.saturating_sub(lo);
                    for delta in 0..=count {
                        let code = lo + delta;
                        // Advance Unicode scalar by delta.
                        let unicode_val = (base as u32) + delta;
                        if let Some(ch) = char::from_u32(unicode_val) {
                            map.insert(code, ch);
                        }
                    }
                }
            } else if tokens.len() >= 2 && line.contains('[') {
                // Array form: may span multiple lines.
                // Collect tokens[0] and tokens[1] as lo/hi, then gather array elements.
                let lo = parse_hex_token(&tokens[0]);
                let hi = parse_hex_token(&tokens[1]);

                // Collect all hex tokens from the array (between '[' and ']').
                // The array might span multiple lines; scan forward.
                let mut array_tokens: Vec<String> = Vec::new();
                let mut combined = line.to_string();
                let mut j = i;
                // Keep reading lines until we find the closing ']'.
                while !combined.contains(']') && j + 1 < lines.len() {
                    j += 1;
                    combined.push(' ');
                    combined.push_str(lines[j].trim());
                }
                // Extract content between '[' and ']'.
                if let (Some(bracket_open), Some(bracket_close)) =
                    (combined.find('['), combined.rfind(']'))
                {
                    let inner = &combined[bracket_open + 1..bracket_close];
                    array_tokens = collect_hex_tokens(inner);
                }
                // Map lo+0 → array[0], lo+1 → array[1], …
                let count = hi.saturating_sub(lo) + 1;
                for delta in 0..(count as usize).min(array_tokens.len()) {
                    let code = lo + delta as u32;
                    if let Some(ch) = utf16be_to_char(&array_tokens[delta]) {
                        map.insert(code, ch);
                    }
                }
                // Advance past the lines we consumed for the array.
                i = j;
            }
        }

        i += 1;
    }

    map
}

// ── Internal helpers ──────────────────────────────────────────────────────────

/// Collect all `<HEXHEX...>` tokens from a string slice.
/// Returns the inner hex digit strings (without angle brackets).
fn collect_hex_tokens(s: &str) -> Vec<String> {
    let mut tokens = Vec::new();
    let mut chars = s.chars().peekable();
    while let Some(ch) = chars.next() {
        if ch == '<' {
            let mut hex = String::new();
            for c in chars.by_ref() {
                if c == '>' { break; }
                if c.is_ascii_hexdigit() { hex.push(c); }
            }
            if !hex.is_empty() {
                tokens.push(hex);
            }
        }
    }
    tokens
}

/// Parse a hex token string (e.g. "0041", "20", "FEFF0041") to u32 big-endian.
/// At most 8 hex digits (4 bytes).
fn parse_hex_token(hex: &str) -> u32 {
    let trimmed = if hex.len() > 8 { &hex[hex.len() - 8..] } else { hex };
    u32::from_str_radix(trimmed, 16).unwrap_or(0)
}

/// Convert a raw UTF-16BE u32 value to a Rust char.
/// The value was obtained from parse_hex_token on a dst_hex field.
/// Handles:
///   - 2-byte / single UTF-16 code unit  (e.g. 0x0041 → 'A')
///   - 4-byte UTF-16 surrogate pair       (e.g. 0xD83DDE00 → emoji)
fn utf16be_scalar(val: u32) -> Option<char> {
    if val <= 0xFFFF {
        // Single UTF-16 code unit.
        char::from_u32(val)
    } else {
        // Two UTF-16 code units packed as high16 | low16.
        let high = (val >> 16) as u16;
        let low  = (val & 0xFFFF) as u16;
        decode_utf16_pair(high, low)
    }
}

/// Parse a hex token directly to a char, treating the bytes as UTF-16BE.
fn utf16be_to_char(hex: &str) -> Option<char> {
    // Normalise length to even number of nibbles.
    let hex_padded: String = if hex.len() % 2 != 0 {
        format!("0{}", hex)
    } else {
        hex.to_string()
    };

    // Collect bytes.
    let bytes: Vec<u8> = hex_padded
        .as_bytes()
        .chunks(2)
        .filter_map(|chunk| {
            let s = std::str::from_utf8(chunk).ok()?;
            u8::from_str_radix(s, 16).ok()
        })
        .collect();

    if bytes.is_empty() { return None; }

    // Interpret as UTF-16BE.
    utf16be_bytes_to_char(&bytes)
}

/// Convert a byte slice representing UTF-16BE code units to a single char.
fn utf16be_bytes_to_char(bytes: &[u8]) -> Option<char> {
    match bytes.len() {
        0 => None,
        1 => char::from_u32(bytes[0] as u32),
        2 => {
            let unit = ((bytes[0] as u16) << 8) | (bytes[1] as u16);
            if (0xD800..=0xDBFF).contains(&unit) {
                // High surrogate without a following low surrogate.
                // Not valid as a standalone; return replacement character.
                Some('\u{FFFD}')
            } else {
                char::from_u32(unit as u32)
            }
        }
        3 => {
            // Three bytes: treat as best-effort.
            let val = ((bytes[0] as u32) << 16) | ((bytes[1] as u32) << 8) | (bytes[2] as u32);
            char::from_u32(val)
        }
        _ => {
            // 4+ bytes: try surrogate pair (high + low 16-bit units).
            let high = ((bytes[0] as u16) << 8) | (bytes[1] as u16);
            let low  = ((bytes[2] as u16) << 8) | (bytes[3] as u16);
            if (0xD800..=0xDBFF).contains(&high) && (0xDC00..=0xDFFF).contains(&low) {
                decode_utf16_pair(high, low)
            } else {
                // Fall back to first two bytes as a BMP character.
                char::from_u32(((bytes[0] as u32) << 8) | (bytes[1] as u32))
            }
        }
    }
}

/// Decode a UTF-16 surrogate pair to a char.
fn decode_utf16_pair(high: u16, low: u16) -> Option<char> {
    if !(0xD800..=0xDBFF).contains(&high) || !(0xDC00..=0xDFFF).contains(&low) {
        return None;
    }
    let code_point = 0x10000u32
        + (((high as u32) - 0xD800) << 10)
        + ((low as u32) - 0xDC00);
    char::from_u32(code_point)
}

// ── Tests ─────────────────────────────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_collect_hex_tokens() {
        let tokens = collect_hex_tokens("<0041> <0042>");
        assert_eq!(tokens, vec!["0041", "0042"]);
    }

    #[test]
    fn test_parse_hex_token() {
        assert_eq!(parse_hex_token("0041"), 0x0041u32);
        assert_eq!(parse_hex_token("20"),   0x20u32);
        assert_eq!(parse_hex_token("FEFF"), 0xFEFFu32);
    }

    #[test]
    fn test_utf16be_to_char_ascii() {
        assert_eq!(utf16be_to_char("0041"), Some('A'));
        assert_eq!(utf16be_to_char("0020"), Some(' '));
        assert_eq!(utf16be_to_char("41"),   Some('A'));
    }

    #[test]
    fn test_bfchar_simple() {
        let cmap = b"begincmap\n4 beginbfchar\n<0041> <0041>\n<0042> <0042>\n<0043> <0043>\n<0044> <0044>\nendbfchar\nendcmap\n";
        let map = parse_cmap(cmap);
        assert_eq!(map.get(&0x0041), Some(&'A'));
        assert_eq!(map.get(&0x0044), Some(&'D'));
    }

    #[test]
    fn test_bfrange_scalar() {
        let cmap = b"begincmap\n1 beginbfrange\n<0020> <0039> <0020>\nendbfrange\nendcmap\n";
        let map = parse_cmap(cmap);
        assert_eq!(map.get(&0x0020), Some(&'\u{0020}'));
        assert_eq!(map.get(&0x0039), Some(&'\u{0039}'));
    }

    #[test]
    fn test_bfrange_array() {
        let cmap = b"begincmap\n1 beginbfrange\n<0041> <0043> [<0041> <0042> <0043>]\nendbfrange\nendcmap\n";
        let map = parse_cmap(cmap);
        assert_eq!(map.get(&0x0041), Some(&'A'));
        assert_eq!(map.get(&0x0042), Some(&'B'));
        assert_eq!(map.get(&0x0043), Some(&'C'));
    }
}
