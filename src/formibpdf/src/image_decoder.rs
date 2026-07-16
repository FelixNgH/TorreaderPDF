use crate::pdf_object::PdfObject;
use std::collections::HashMap;

#[derive(Debug, Clone)]
pub struct DecodedImage {
    pub pixels: Vec<u8>, // RGBA8 row-major
    pub width:  u32,
    pub height: u32,
}

/// Apply an ICC profile transform to raw pixel bytes → RGBA8 output.
/// `n_channels`: 1=Gray, 3=RGB, 4=CMYK.
/// Returns None if the profile is invalid, channel count unsupported, or data too short.
pub fn apply_icc_to_rgba(raw: &[u8], icc_bytes: &[u8], n_channels: u8, width: u32, height: u32) -> Option<Vec<u8>> {
    use lcms2::{Profile, PixelFormat, Transform, Intent};

    let src_fmt = match n_channels {
        1 => PixelFormat::GRAY_8,
        3 => PixelFormat::RGB_8,
        4 => PixelFormat::CMYK_8,
        _ => return None,
    };

    let n_pixels = (width as usize).checked_mul(height as usize)?;
    let expected  = n_pixels.checked_mul(n_channels as usize)?;
    if raw.len() < expected { return None; }

    let src_profile = Profile::new_icc(icc_bytes).ok()?;
    let dst_profile = Profile::new_srgb();

    let transform = Transform::new(
        &src_profile, src_fmt,
        &dst_profile, PixelFormat::RGB_8,
        Intent::Perceptual,
    ).ok()?; // returns None if the profile combination is invalid

    let mut rgb_out = vec![0u8; n_pixels * 3];
    transform.transform_pixels(&raw[..expected], &mut rgb_out);

    // RGB → RGBA (append opaque alpha)
    let mut rgba = Vec::with_capacity(n_pixels * 4);
    for chunk in rgb_out.chunks_exact(3) {
        rgba.extend_from_slice(chunk);
        rgba.push(255);
    }
    Some(rgba)
}

// ─────────────────────────────────────────────────────────────────────────────
// Public entry-point
// ─────────────────────────────────────────────────────────────────────────────

pub fn decode_image(dict: &HashMap<String, PdfObject>, data: &[u8]) -> Option<DecodedImage> {
    let width  = dict.get("Width") .and_then(|o| o.as_i64()).map(|v| v as u32)?;
    let height = dict.get("Height").and_then(|o| o.as_i64()).map(|v| v as u32)?;

    let bpc = dict.get("BitsPerComponent").and_then(|o| o.as_i64()).unwrap_or(8);

    let cs_obj = dict.get("ColorSpace");
    let cs = cs_obj.and_then(|o| match o {
        PdfObject::Name(n)  => Some(n.as_str()),
        PdfObject::Array(a) => a.first().and_then(|x| x.as_name()),
        _ => None,
    }).unwrap_or("DeviceRGB");

    // Handle Indexed colorspace: /Indexed /base hival lookup_table
    if cs == "Indexed" {
        return decode_indexed_image(dict, data, bpc as u8);
    }

    // 1-bit and 4-bit images: unpack bitfields
    if bpc == 1 { return decode_bilevel_image(dict, data, cs); }
    if bpc == 4 { return decode_4bit_image(dict, data, cs); }

    // Reject other non-8-bit (after CCITTFax check; those ignore bpc)
    // Note: CCITTFax images are inherently 1-bit; they may declare BPC=1 or omit it.
    // We check the filter first before bpc gating.
    let filter = get_filter(dict);

    // ── CCITTFaxDecode (Group 3 / Group 4) ──────────────────────────────────
    if filter == "CCITTFaxDecode" || filter == "CCF" {
        return decode_ccitt_image(dict, data);
    }

    // ── JBIG2Decode — native decoder ────────────────────────────────────────
    if filter == "JBIG2Decode" {
        return decode_jbig2_image(dict, data);
    }

    // ── JPXDecode — native JPEG 2000 decoder ────────────────────────────────
    if filter == "JPXDecode" {
        return decode_jpx_image(dict, data);
    }

    // ── Lab colorspace (raw, uncompressed) ──────────────────────────────────
    if cs == "Lab" {
        return decode_lab_image(dict, data);
    }

    // Everything below requires 8 bpc
    if bpc != 8 { return None; }

    // ── ICCBased: pre-resolved profile injected by resource.rs ───────────────
    // resource.rs embeds the profile bytes as a synthetic PdfObject::Str key.
    if let Some(PdfObject::Str(icc_bytes)) = dict.get("_ICCProfile") {
        let n_ch = dict.get("_ICCN").and_then(|o| o.as_i64()).unwrap_or(3) as u8;
        if let Some(rgba) = apply_icc_to_rgba(data, icc_bytes, n_ch, width, height) {
            return Some(DecodedImage { pixels: rgba, width, height });
        }
        // Fall through to channel-count raw decode on ICC failure
    }

    // ── DCTDecode (JPEG) ─────────────────────────────────────────────────────
    let is_dct = filter == "DCTDecode" || filter == "DCT"
        || dict.get("Filter").map(|f| match f {
            PdfObject::Array(a) => a.iter().any(|x| x.as_name() == Some("DCTDecode")),
            _ => false,
        }).unwrap_or(false);

    if is_dct {
        // The `image` crate's JPEG decoder converts CMYK to RGB internally, so
        // ColorType will always be one of: L8, La8, Rgb8, Rgba8.
        // CMYK raw data (non-DCT) is handled in the raw-pixel section below.
        let img = image::load_from_memory(data).ok()?;
        use image::ColorType;
        return match img.color() {
            ColorType::L8
            | ColorType::La8
            | ColorType::Rgb8
            | ColorType::Rgba8 => {
                let rgba = img.to_rgba8();
                let (w, h) = (rgba.width(), rgba.height());
                Some(DecodedImage { pixels: rgba.into_raw(), width: w, height: h })
            }
            _ => None, // exotic JPEG variant → PDFium fallback
        };
    }

    // ── CMYK raw (not compressed) — convert inline ───────────────────────────
    // (was previously returned as None; now we handle it)
    let channels: usize = match cs {
        "DeviceRGB"  | "RGB"  => 3,
        "DeviceGray" | "Gray" => 1,
        "DeviceCMYK" | "CMYK" => 4,
        _ => return None,
    };

    // Guard against integer overflow on malformed/crafted PDFs.
    let expected = (width as usize)
        .checked_mul(height as usize)
        .and_then(|n| n.checked_mul(channels))?;
    let rgba_len = (width as usize)
        .checked_mul(height as usize)
        .and_then(|n| n.checked_mul(4))?;

    if data.len() < expected { return None; }

    let mut pixels = vec![255u8; rgba_len];
    match channels {
        3 => {
            for (i, chunk) in data[..expected].chunks_exact(3).enumerate() {
                let base = i * 4;
                pixels[base]   = chunk[0];
                pixels[base+1] = chunk[1];
                pixels[base+2] = chunk[2];
            }
        }
        1 => {
            for (i, &g) in data[..expected].iter().enumerate() {
                let base = i * 4;
                pixels[base] = g; pixels[base+1] = g; pixels[base+2] = g;
            }
        }
        4 => {
            // Raw CMYK (not JPEG) — simple ink-coverage formula
            for (i, chunk) in data[..expected].chunks_exact(4).enumerate() {
                let (c, m, y, k) = (
                    chunk[0] as f32 / 255.0,
                    chunk[1] as f32 / 255.0,
                    chunk[2] as f32 / 255.0,
                    chunk[3] as f32 / 255.0,
                );
                let base = i * 4;
                pixels[base]   = ((1.0 - c) * (1.0 - k) * 255.0) as u8;
                pixels[base+1] = ((1.0 - m) * (1.0 - k) * 255.0) as u8;
                pixels[base+2] = ((1.0 - y) * (1.0 - k) * 255.0) as u8;
            }
        }
        _ => return None,
    }
    Some(DecodedImage { pixels, width, height })
}

// ─────────────────────────────────────────────────────────────────────────────
// Filter helpers
// ─────────────────────────────────────────────────────────────────────────────

/// Return the first (or only) filter name from the image dictionary.
fn get_filter<'a>(dict: &'a HashMap<String, PdfObject>) -> &'a str {
    dict.get("Filter")
        .map(|f| match f {
            PdfObject::Name(n)  => n.as_str(),
            PdfObject::Array(a) => a.first().and_then(|x| x.as_name()).unwrap_or(""),
            _ => "",
        })
        .unwrap_or("")
}

// ─────────────────────────────────────────────────────────────────────────────
// CCITTFax dispatcher
// ─────────────────────────────────────────────────────────────────────────────

fn decode_ccitt_image(dict: &HashMap<String, PdfObject>, data: &[u8]) -> Option<DecodedImage> {
    let width  = dict.get("Width") .and_then(|o| o.as_i64()).map(|v| v as u32)?;
    let height = dict.get("Height").and_then(|o| o.as_i64()).map(|v| v as u32)?;

    // DecodeParms may be a dict or an array-of-dicts; we read the first dict.
    let parms_obj = dict.get("DecodeParms");

    let get_parm_i64 = |key: &str| -> Option<i64> {
        let p = parms_obj?;
        match p {
            PdfObject::Dict(_) | PdfObject::Stream { .. } => {
                p.dict_get(key).and_then(|o| o.as_i64())
            }
            PdfObject::Array(a) => {
                a.first().and_then(|first| first.dict_get(key)).and_then(|o| o.as_i64())
            }
            _ => None,
        }
    };

    let get_parm_bool = |key: &str| -> bool {
        let p = match parms_obj { Some(p) => p, None => return false };
        let val_obj = match p {
            PdfObject::Dict(_) | PdfObject::Stream { .. } => p.dict_get(key),
            PdfObject::Array(a) => a.first().and_then(|first| first.dict_get(key)),
            _ => None,
        };
        val_obj.map(|o| matches!(o, PdfObject::Bool(true))).unwrap_or(false)
    };

    // /K  0 = G3 1D  |  negative = G4 (pure 2D)  |  positive = G3 mixed
    let k = get_parm_i64("K").unwrap_or(0);
    // /BlackIs1 — when true, 1-bit in stream = black pixel.
    // Default is false (0 = black, 1 = white → photographic negative).
    let black_is_1 = get_parm_bool("BlackIs1");

    // Decode to 1-bit packed rows
    let packed = if k < 0 {
        crate::ccitt::decode_ccitt_g4(data, width, height, black_is_1)?
    } else {
        crate::ccitt::decode_ccitt_g3(data, width, height, black_is_1, k as i32)?
    };

    // Unpack 1-bit packed rows → RGBA8
    let npix = (width * height) as usize;
    let row_bytes = ((width + 7) / 8) as usize;
    let mut pixels = vec![255u8; npix * 4];
    let mut pi = 0usize;

    for row in 0..height as usize {
        for col in 0..width as usize {
            let byte_idx = row * row_bytes + col / 8;
            let bit      = 7 - (col % 8);
            // Raw bit: 1 in the packed buffer always means "colour A" as decoded.
            // G4/G3 decoders treat black pixels as 1 in their row buffers,
            // then pack those. So a 1-bit = black in the packed output already.
            let raw_bit = if byte_idx < packed.len() {
                (packed[byte_idx] >> bit) & 1 == 1
            } else {
                false
            };
            // Apply BlackIs1 semantics:
            //   black_is_1 = true  → bit=1 means black   → grey=0
            //   black_is_1 = false → bit=1 means white   → grey=255
            // Our decoder stores 1 = black internally, so:
            //   if black_is_1 true  → no inversion needed
            //   if black_is_1 false → invert
            let is_black = if black_is_1 { raw_bit } else { !raw_bit };
            let grey = if is_black { 0u8 } else { 255u8 };
            pixels[pi]   = grey;
            pixels[pi+1] = grey;
            pixels[pi+2] = grey;
            // alpha already 255
            pi += 4;
        }
    }

    Some(DecodedImage { pixels, width, height })
}

// ─────────────────────────────────────────────────────────────────────────────
// JPX / JPEG 2000
// ─────────────────────────────────────────────────────────────────────────────

fn decode_jpx_image(_dict: &HashMap<String, PdfObject>, data: &[u8]) -> Option<DecodedImage> {
    // Dispatch to native j2k.rs decoder.
    // Falls back to PDFium (returns None) until j2k.rs is fully implemented.
    crate::j2k::decode(data)
}

// ─────────────────────────────────────────────────────────────────────────────
// Indexed colorspace
// ─────────────────────────────────────────────────────────────────────────────

fn decode_indexed_image(
    dict: &HashMap<String, PdfObject>,
    data: &[u8],
    bpc:  u8,
) -> Option<DecodedImage> {
    let width  = dict.get("Width") .and_then(|o| o.as_i64()).map(|v| v as u32)?;
    let height = dict.get("Height").and_then(|o| o.as_i64()).map(|v| v as u32)?;

    // Indexed array: [/Indexed /base hival table_bytes]
    let cs_arr = dict.get("ColorSpace").and_then(|o| {
        if let PdfObject::Array(a) = o { Some(a.as_slice()) } else { None }
    })?;
    if cs_arr.len() < 4 { return None; }

    let hival = cs_arr[2].as_i64().unwrap_or(255) as usize;
    let table_data: Vec<u8> = match &cs_arr[3] {
        PdfObject::Str(s)                    => s.clone(),
        PdfObject::Stream { data: d, .. }    => d.clone(),
        _ => return None,
    };

    let channels = 3usize; // assume RGB palette
    let palette: Vec<[u8; 3]> = table_data
        .chunks(channels)
        .take(hival + 1)
        .map(|c| [
            c.get(0).copied().unwrap_or(0),
            c.get(1).copied().unwrap_or(0),
            c.get(2).copied().unwrap_or(0),
        ])
        .collect();

    let npix = (width * height) as usize;
    let mut pixels = vec![255u8; npix * 4];

    match bpc {
        8 => {
            for (i, &idx) in data[..npix.min(data.len())].iter().enumerate() {
                let entry = palette.get(idx as usize).copied().unwrap_or([0, 0, 0]);
                let base = i * 4;
                pixels[base] = entry[0]; pixels[base+1] = entry[1]; pixels[base+2] = entry[2];
            }
        }
        1 => {
            let mut i = 0;
            'outer: for byte in data {
                for bit in (0..8).rev() {
                    if i >= npix { break 'outer; }
                    let idx   = ((byte >> bit) & 1) as usize;
                    let entry = palette.get(idx).copied().unwrap_or([0, 0, 0]);
                    let base  = i * 4;
                    pixels[base] = entry[0]; pixels[base+1] = entry[1]; pixels[base+2] = entry[2];
                    i += 1;
                }
            }
        }
        4 => {
            let mut i = 0;
            'outer: for byte in data {
                for nibble in [byte >> 4, byte & 0xF] {
                    if i >= npix { break 'outer; }
                    let entry = palette.get(nibble as usize).copied().unwrap_or([0, 0, 0]);
                    let base  = i * 4;
                    pixels[base] = entry[0]; pixels[base+1] = entry[1]; pixels[base+2] = entry[2];
                    i += 1;
                }
            }
        }
        _ => return None,
    }
    Some(DecodedImage { pixels, width, height })
}

// ─────────────────────────────────────────────────────────────────────────────
// 1-bit bilevel image
// ─────────────────────────────────────────────────────────────────────────────

fn decode_bilevel_image(
    dict: &HashMap<String, PdfObject>,
    data: &[u8],
    cs:   &str,
) -> Option<DecodedImage> {
    let width  = dict.get("Width") .and_then(|o| o.as_i64()).map(|v| v as u32)?;
    let height = dict.get("Height").and_then(|o| o.as_i64()).map(|v| v as u32)?;

    // DeviceGray: 0 = black by default (unless Decode says otherwise)
    let invert = cs == "DeviceGray";
    let black_is_1 = dict
        .get("Decode")
        .and_then(|o| if let PdfObject::Array(a) = o { a.first() } else { None })
        .and_then(|o| o.as_f64())
        .map(|v| v == 0.0)
        .unwrap_or(false);

    let npix      = (width * height) as usize;
    let row_bytes = (width as usize + 7) / 8;
    let mut pixels = vec![255u8; npix * 4];
    let mut i = 0usize;

    for row in 0..height as usize {
        let row_start = row * row_bytes;
        for col in 0..width as usize {
            if i >= npix { break; }
            let byte_idx = row_start + col / 8;
            let bit      = 7 - (col % 8);
            let val = if byte_idx < data.len() { (data[byte_idx] >> bit) & 1 } else { 0 };
            let grey = if (val == 0) ^ black_is_1 ^ invert { 0u8 } else { 255u8 };
            let base = i * 4;
            pixels[base] = grey; pixels[base+1] = grey; pixels[base+2] = grey;
            i += 1;
        }
    }
    Some(DecodedImage { pixels, width, height })
}

// ─────────────────────────────────────────────────────────────────────────────
// 4-bit grayscale image
// ─────────────────────────────────────────────────────────────────────────────

fn decode_4bit_image(
    dict:  &HashMap<String, PdfObject>,
    data:  &[u8],
    _cs:   &str,
) -> Option<DecodedImage> {
    let width  = dict.get("Width") .and_then(|o| o.as_i64()).map(|v| v as u32)?;
    let height = dict.get("Height").and_then(|o| o.as_i64()).map(|v| v as u32)?;
    let npix   = (width * height) as usize;
    let mut pixels = vec![255u8; npix * 4];
    let mut i = 0usize;

    'outer: for byte in data {
        for nibble in [byte >> 4, byte & 0xF] {
            if i >= npix { break 'outer; }
            let g    = nibble * 17; // scale 0-15 → 0-255
            let base = i * 4;
            pixels[base] = g; pixels[base+1] = g; pixels[base+2] = g;
            i += 1;
        }
    }
    Some(DecodedImage { pixels, width, height })
}

// ─────────────────────────────────────────────────────────────────────────────
// Lab colorspace image (raw, uncompressed)
// ─────────────────────────────────────────────────────────────────────────────

fn decode_lab_image(dict: &HashMap<String, PdfObject>, data: &[u8]) -> Option<DecodedImage> {
    let width  = dict.get("Width") .and_then(|o| o.as_i64()).map(|v| v as u32)?;
    let height = dict.get("Height").and_then(|o| o.as_i64()).map(|v| v as u32)?;
    let npix   = (width as usize).checked_mul(height as usize)?;
    if data.len() < npix * 3 { return None; }

    let range = dict.get("Range").and_then(|o| {
        if let PdfObject::Array(a) = o { Some(a.clone()) } else { None }
    });
    let get_r = |idx: usize, default: f32| -> f32 {
        range.as_ref().and_then(|a| a.get(idx)).and_then(|o| o.as_f64())
            .map(|v| v as f32).unwrap_or(default)
    };
    let (l_min, l_max) = (get_r(0, 0.0),    get_r(1, 100.0));
    let (a_min, a_max) = (get_r(2, -128.0), get_r(3, 127.0));
    let (b_min, b_max) = (get_r(4, -128.0), get_r(5, 127.0));

    let mut pixels = vec![255u8; npix * 4];
    for (i, chunk) in data[..npix * 3].chunks_exact(3).enumerate() {
        let l = l_min + (chunk[0] as f32 / 255.0) * (l_max - l_min);
        let a = a_min + (chunk[1] as f32 / 255.0) * (a_max - a_min);
        let b = b_min + (chunk[2] as f32 / 255.0) * (b_max - b_min);
        let c = crate::graphics_state::Color::from_lab(l, a, b);
        let base = i * 4;
        pixels[base]   = (c.r.clamp(0.0, 1.0) * 255.0) as u8;
        pixels[base+1] = (c.g.clamp(0.0, 1.0) * 255.0) as u8;
        pixels[base+2] = (c.b.clamp(0.0, 1.0) * 255.0) as u8;
    }
    Some(DecodedImage { pixels, width, height })
}

// ─────────────────────────────────────────────────────────────────────────────
// JBIG2 — dispatch to jbig2.rs module
// ─────────────────────────────────────────────────────────────────────────────

fn decode_jbig2_image(dict: &HashMap<String, PdfObject>, data: &[u8]) -> Option<DecodedImage> {
    let width  = dict.get("Width") .and_then(|o| o.as_i64()).map(|v| v as u32)?;
    let height = dict.get("Height").and_then(|o| o.as_i64()).map(|v| v as u32)?;
    let black_is_1 = dict.get("BlackIs1")
        .map(|o| matches!(o, PdfObject::Bool(true))).unwrap_or(false);
    let packed = crate::jbig2::decode_pdf_stream(data, width, height)?;
    Some(unpack_bilevel_to_rgba(&packed, width, height, black_is_1))
}

fn unpack_bilevel_to_rgba(packed: &[u8], width: u32, height: u32, black_is_1: bool) -> DecodedImage {
    let npix = (width * height) as usize;
    let row_bytes = ((width + 7) / 8) as usize;
    let mut pixels = vec![255u8; npix * 4];
    let mut pi = 0usize;
    for row in 0..height as usize {
        for col in 0..width as usize {
            let byte_idx = row * row_bytes + col / 8;
            let bit = 7 - (col % 8);
            let is_set = byte_idx < packed.len() && (packed[byte_idx] >> bit) & 1 == 1;
            let grey = if is_set ^ black_is_1 { 0u8 } else { 255u8 };
            pixels[pi] = grey; pixels[pi+1] = grey; pixels[pi+2] = grey;
            pi += 4;
        }
    }
    DecodedImage { pixels, width, height }
}
