use crate::pdf_object::PdfObject;
use crate::xref::XRefTable;
use crate::font::FontCache;
use crate::cmap;
use crate::shading::ShadingKind;
use crate::graphics_state::{BlendMode, Matrix, SmaskType};
use std::collections::HashMap;

/// PDF Pattern resource entry — stores raw content for lazy interpretation.
#[derive(Debug, Clone)]
pub enum PatternEntry {
    Tiling {
        paint_type:  u8,
        tiling_type: u8,
        x_step:      f64,
        y_step:      f64,
        bbox:        (f64, f64, f64, f64),
        matrix:      Matrix,
        content:     Vec<u8>,
        resources:   Option<HashMap<String, PdfObject>>,
    },
    Shading {
        shading: ShadingKind,
        matrix:  Matrix,
    },
}

/// Raw SMask data extracted from the PDF /SMask dict in an ExtGState.
/// The form content bytes are pre-decoded (FlateDecode applied by XRef resolver).
#[derive(Debug, Clone)]
pub struct SmaskRawEntry {
    pub smask_type:     SmaskType,
    pub form_bytes:     Vec<u8>,   // decoded content stream of the mask Form XObject
    pub bc:             [f32; 3],  // backdrop color
    pub form_width_pt:  f64,
    pub form_height_pt: f64,
}

/// Parameters from a PDF ExtGState dict entry (/Resources/ExtGState).
/// Fields are Option because any subset may be present per ExtGState.
#[derive(Debug, Clone, Default)]
pub struct ExtGStateEntry {
    /// /ca — non-stroking (fill) alpha
    pub fill_alpha:   Option<f32>,
    /// /CA — stroking alpha
    pub stroke_alpha: Option<f32>,
    /// /BM — blend mode name
    pub blend_mode:   Option<BlendMode>,
    /// /SMask — soft mask definition (None if /SMask /None or absent)
    pub smask:        Option<SmaskRawEntry>,
}

/// A color space resolved to its canonical form (ICC profile bytes pre-extracted).
#[derive(Debug, Clone)]
pub enum ResolvedColorspace {
    DeviceRGB,
    DeviceGray,
    DeviceCMYK,
    Lab,
    Separation,
    Pattern,
    ICCBased { n_channels: u8, profile: Vec<u8> },
    Unknown,
}

#[derive(Debug, Clone)]
pub struct FontResource {
    pub font_id:       u64,
    pub base_font:     String,
    pub is_embedded:   bool,
    /// ToUnicode CMap: char code → Unicode char.
    pub to_unicode:    HashMap<u32, char>,
    /// advance_width / font_size ratio sampled from fontdue metrics.
    /// Used by interpreter for correct text_x advance (default 0.55 if unknown).
    pub advance_ratio: f32,
    /// CID flag: true for Type0 / CID-keyed (composite) fonts
    pub cid:           bool,
}

#[derive(Debug, Clone)]
pub enum XObjectKind {
    Image { dict: HashMap<String, PdfObject>, data: Vec<u8> },
    Form  { dict: HashMap<String, PdfObject>, content: Vec<u8> },
    Unknown,
}

#[derive(Default, Clone)]
pub struct PageResources {
    pub fonts:       HashMap<String, FontResource>,
    pub xobjects:    HashMap<String, XObjectKind>,
    pub shadings:    HashMap<String, ShadingKind>,
    /// Pre-resolved color spaces from /Resources/ColorSpace dict.
    pub colorspaces: HashMap<String, ResolvedColorspace>,
    /// Parsed ExtGState entries from /Resources/ExtGState dict.
    pub ext_gstates: HashMap<String, ExtGStateEntry>,
    /// Pattern resources from /Resources/Pattern dict.
    pub patterns:    HashMap<String, PatternEntry>,
}

impl PageResources {
    pub fn from_page(
        page_dict:   &HashMap<String, PdfObject>,
        xref:        &XRefTable,
        data:        &[u8],
        font_cache:  &mut FontCache,
    ) -> Self {
        let mut res = PageResources::default();

        let resources_obj = match page_dict.get("Resources") {
            Some(o) => xref.resolve(data, o),
            None    => return res,
        };
        let res_dict = match &resources_obj {
            PdfObject::Dict(d) => d.clone(),
            _ => return res,
        };

        // ── Fonts ─────────────────────────────────────────────────────────────
        if let Some(fonts_val) = res_dict.get("Font") {
            let fonts_obj = xref.resolve(data, fonts_val);
            if let PdfObject::Dict(fonts_map) = fonts_obj {
                for (alias, font_ref) in &fonts_map {
                    let fr = resolve_font(font_ref, xref, data, font_cache);
                    res.fonts.insert(alias.clone(), fr);
                }
            }
        }

        // ── XObjects ──────────────────────────────────────────────────────────
        if let Some(xobj_val) = res_dict.get("XObject") {
            let xobj_obj = xref.resolve(data, xobj_val);
            if let PdfObject::Dict(xobj_map) = xobj_obj {
                for (name, xref_val) in &xobj_map {
                    let resolved = xref.resolve(data, xref_val);
                    let kind = match resolved {
                        PdfObject::Stream { dict: sdict, data: sdata } => {
                            match sdict.get("Subtype").and_then(|o| o.as_name()) {
                                Some("Image") => XObjectKind::Image {
                                    dict: sdict.clone(), data: sdata.clone(),
                                },
                                Some("Form") => XObjectKind::Form {
                                    dict: sdict.clone(), content: sdata.clone(),
                                },
                                _ => XObjectKind::Unknown,
                            }
                        }
                        _ => XObjectKind::Unknown,
                    };
                    res.xobjects.insert(name.clone(), kind);
                }
            }
        }

        // ── Shadings ──────────────────────────────────────────────────────────
        if let Some(sh_val) = res_dict.get("Shading") {
            let sh_obj = xref.resolve(data, sh_val);
            if let PdfObject::Dict(sh_map) = sh_obj {
                for (name, sh_ref) in &sh_map {
                    let resolved = xref.resolve(data, sh_ref);
                    let (sh_dict, stream_bytes) = match &resolved {
                        PdfObject::Dict(d)                   => (d.clone(), None),
                        PdfObject::Stream { dict, data: sd } => (dict.clone(), Some(sd.clone())),
                        _ => continue,
                    };
                    if let Some(kind) = crate::shading::parse_shading(&sh_dict, stream_bytes.as_deref(), xref, data) {
                        res.shadings.insert(name.clone(), kind);
                    }
                }
            }
        }

        // ── Patterns ──────────────────────────────────────────────────────────
        if let Some(pat_val) = res_dict.get("Pattern") {
            let pat_obj = xref.resolve(data, pat_val);
            if let PdfObject::Dict(pat_map) = pat_obj {
                for (name, pat_ref) in &pat_map {
                    let resolved = xref.resolve(data, pat_ref);
                    let (pd, content) = match &resolved {
                        PdfObject::Stream { dict, data: sd } => (dict.clone(), sd.clone()),
                        PdfObject::Dict(d) => (d.clone(), Vec::new()),
                        _ => continue,
                    };
                    let pt = pd.get("PatternType").and_then(|o| o.as_i64()).unwrap_or(1);
                    match pt {
                        1 => {
                            let paint_type  = pd.get("PaintType").and_then(|o| o.as_i64()).unwrap_or(1) as u8;
                            let tiling_type = pd.get("TilingType").and_then(|o| o.as_i64()).unwrap_or(1) as u8;
                            let x_step = pd.get("XStep").and_then(|o| o.as_f64()).unwrap_or(1.0);
                            let y_step = pd.get("YStep").and_then(|o| o.as_f64()).unwrap_or(1.0);
                            let bbox = pd.get("BBox").and_then(|o| o.as_array())
                                .and_then(|a| if a.len() >= 4 {
                                    Some((a[0].as_f64()?, a[1].as_f64()?, a[2].as_f64()?, a[3].as_f64()?))
                                } else { None })
                                .unwrap_or((0.0, 0.0, 1.0, 1.0));
                            let matrix = pd.get("Matrix").and_then(|o| o.as_array())
                                .and_then(|a| if a.len() >= 6 {
                                    Some(Matrix {
                                        a: a[0].as_f64()?, b: a[1].as_f64()?,
                                        c: a[2].as_f64()?, d: a[3].as_f64()?,
                                        e: a[4].as_f64()?, f: a[5].as_f64()?,
                                    })
                                } else { None })
                                .unwrap_or_else(Matrix::identity);
                            let pattern_res = pd.get("Resources")
                                .map(|r| { let o = xref.resolve(data, r); if let PdfObject::Dict(d) = o { Some(d) } else { None } })
                                .flatten();
                            res.patterns.insert(name.clone(), PatternEntry::Tiling {
                                paint_type, tiling_type, x_step, y_step, bbox, matrix, content, resources: pattern_res,
                            });
                        }
                        2 => {
                            let sh_ref = pd.get("Shading");
                            if let Some(sr) = sh_ref {
                                let sh_obj = xref.resolve(data, sr);
                                let (sh_dict, sh_stream) = match &sh_obj {
                                    PdfObject::Stream { dict, data: sd } => (dict.clone(), Some(sd.clone())),
                                    PdfObject::Dict(d) => (d.clone(), None),
                                    _ => continue,
                                };
                                if let Some(shading) = crate::shading::parse_shading(&sh_dict, sh_stream.as_deref(), xref, data) {
                                    let matrix = pd.get("Matrix").and_then(|o| o.as_array())
                                        .and_then(|a| if a.len() >= 6 { Some(Matrix { a: a[0].as_f64()?, b: a[1].as_f64()?, c: a[2].as_f64()?, d: a[3].as_f64()?, e: a[4].as_f64()?, f: a[5].as_f64()? }) } else { None })
                                        .unwrap_or_else(Matrix::identity);
                                    res.patterns.insert(name.clone(), PatternEntry::Shading { shading, matrix });
                                }
                            }
                        }
                        _ => {}
                    }
                }
            }
        }

        // ── ExtGState ─────────────────────────────────────────────────────────
        if let Some(egs_val) = res_dict.get("ExtGState") {
            let egs_obj = xref.resolve(data, egs_val);
            if let PdfObject::Dict(egs_map) = egs_obj {
                for (name, egs_ref) in &egs_map {
                    let resolved = xref.resolve(data, egs_ref);
                    let gsd = match &resolved {
                        PdfObject::Dict(d)             => d.clone(),
                        PdfObject::Stream { dict, .. } => dict.clone(),
                        _ => continue,
                    };
                    let mut entry = ExtGStateEntry::default();
                    if let Some(v) = gsd.get("ca").and_then(|o| o.as_f64()) {
                        entry.fill_alpha = Some(v as f32);
                    }
                    if let Some(v) = gsd.get("CA").and_then(|o| o.as_f64()) {
                        entry.stroke_alpha = Some(v as f32);
                    }
                    if let Some(bm_name) = gsd.get("BM").and_then(|o| o.as_name()) {
                        entry.blend_mode = Some(BlendMode::from_name(bm_name));
                    }
                    // /SMask — soft mask dict: {/Type /Mask, /S /Alpha|/Luminosity, /G <form-ref>}
                    if let Some(sm_val) = gsd.get("SMask") {
                        let sm_obj = xref.resolve(data, sm_val);
                        // /SMask /None clears the mask — leave entry.smask = None
                        if let PdfObject::Dict(sm_dict) = sm_obj {
                            let smask_type = match sm_dict.get("S").and_then(|o| o.as_name()) {
                                Some("Luminosity") => SmaskType::Luminosity,
                                _                  => SmaskType::Alpha,
                            };
                            // /G — the Form XObject to use as mask
                            let (form_bytes, form_w, form_h) = if let Some(g_ref) = sm_dict.get("G") {
                                let form_obj = xref.resolve(data, g_ref);
                                if let PdfObject::Stream { dict: fd, data: fc } = form_obj {
                                    let bbox_w = fd.get("BBox")
                                        .and_then(|o| o.as_array())
                                        .and_then(|a| {
                                            let x0 = a.get(0).and_then(|v| v.as_f64()).unwrap_or(0.0);
                                            let x1 = a.get(2).and_then(|v| v.as_f64()).unwrap_or(0.0);
                                            Some((x1 - x0).abs())
                                        }).unwrap_or(0.0);
                                    let bbox_h = fd.get("BBox")
                                        .and_then(|o| o.as_array())
                                        .and_then(|a| {
                                            let y0 = a.get(1).and_then(|v| v.as_f64()).unwrap_or(0.0);
                                            let y1 = a.get(3).and_then(|v| v.as_f64()).unwrap_or(0.0);
                                            Some((y1 - y0).abs())
                                        }).unwrap_or(0.0);
                                    (fc, bbox_w, bbox_h)
                                } else { (Vec::new(), 0.0, 0.0) }
                            } else { (Vec::new(), 0.0, 0.0) };
                            // /BC — backdrop color [r g b], default black
                            let bc = if let Some(PdfObject::Array(bc_arr)) = sm_dict.get("BC") {
                                let r = bc_arr.first().and_then(|o| o.as_f64()).unwrap_or(0.0) as f32;
                                let g = bc_arr.get(1).and_then(|o| o.as_f64()).unwrap_or(0.0) as f32;
                                let b = bc_arr.get(2).and_then(|o| o.as_f64()).unwrap_or(0.0) as f32;
                                [r, g, b]
                            } else { [0.0, 0.0, 0.0] };
                            if !form_bytes.is_empty() {
                                entry.smask = Some(SmaskRawEntry {
                                    smask_type, form_bytes, bc,
                                    form_width_pt: form_w, form_height_pt: form_h,
                                });
                            }
                        }
                    }
                    res.ext_gstates.insert(name.clone(), entry);
                }
            }
        }

        // ── ColorSpace (ICCBased resolution) ──────────────────────────────────
        if let Some(cs_val) = res_dict.get("ColorSpace") {
            let cs_obj = xref.resolve(data, cs_val);
            if let PdfObject::Dict(cs_map) = cs_obj {
                for (alias, entry) in &cs_map {
                    let resolved = xref.resolve(data, entry);
                    res.colorspaces.insert(alias.clone(), resolve_colorspace_obj(&resolved, xref, data));
                }
            }
        }

        res
    }
}

/// Resolve a raw PdfObject (Name or Array) into a `ResolvedColorspace`.
/// Handles [/ICCBased stream-ref] by extracting profile bytes + N.
pub fn resolve_colorspace_obj(obj: &PdfObject, xref: &XRefTable, data: &[u8]) -> ResolvedColorspace {
    match obj {
        PdfObject::Array(arr) => {
            let first_name = arr.first().and_then(|o| o.as_name()).unwrap_or("");
            match first_name {
                "ICCBased" => {
                    if let Some(stream_ref) = arr.get(1) {
                        let stream = xref.resolve(data, stream_ref);
                        if let PdfObject::Stream { dict: sd, data: sd_data } = stream {
                            let n = sd.get("N").and_then(|o| o.as_i64()).unwrap_or(3) as u8;
                            return ResolvedColorspace::ICCBased { n_channels: n, profile: sd_data };
                        }
                    }
                    ResolvedColorspace::Unknown
                }
                "Lab"                        => ResolvedColorspace::Lab,
                "Pattern"                    => ResolvedColorspace::Pattern,
                "Separation" | "DeviceN"
                | "Indexed"                  => ResolvedColorspace::Separation,
                "DeviceRGB"  | "CalRGB"      => ResolvedColorspace::DeviceRGB,
                "DeviceGray" | "CalGray"     => ResolvedColorspace::DeviceGray,
                "DeviceCMYK"                 => ResolvedColorspace::DeviceCMYK,
                _                            => ResolvedColorspace::Unknown,
            }
        }
        PdfObject::Name(n) => match n.as_str() {
            "DeviceRGB"  | "CalRGB"  | "sRGB" => ResolvedColorspace::DeviceRGB,
            "DeviceGray" | "CalGray"           => ResolvedColorspace::DeviceGray,
            "DeviceCMYK"                       => ResolvedColorspace::DeviceCMYK,
            "Lab"                              => ResolvedColorspace::Lab,
            "Pattern"                          => ResolvedColorspace::Pattern,
            "Separation" | "DeviceN" | "Indexed" => ResolvedColorspace::Separation,
            _                                  => ResolvedColorspace::Unknown,
        },
        _ => ResolvedColorspace::Unknown,
    }
}

fn resolve_font(
    font_ref:   &PdfObject,
    xref:       &XRefTable,
    data:       &[u8],
    font_cache: &mut FontCache,
) -> FontResource {
    let mut font_id       = 0u64;
    let mut base_font     = String::new();
    let mut is_embedded   = false;
    let mut to_unicode: HashMap<u32, char> = HashMap::new();
    let mut advance_ratio = 0.55f32;
    let mut cid_bool: bool = false;

    let resolved = xref.resolve(data, font_ref);
    let fd = match resolved {
        PdfObject::Dict(d) => d,
        _ => return FontResource { font_id, base_font, is_embedded, to_unicode, advance_ratio, cid: cid_bool },
    };

    if let Some(bf) = fd.get("BaseFont").and_then(|o| o.as_name()) {
        base_font = bf.to_string();
    }

    // Determine Type0 CID font path
    let subtype_is_type0 = fd.get("Subtype").and_then(|o| o.as_name()) == Some("Type0");

    if subtype_is_type0 {
        // CIDFont dictionary is the first element of /DescendantFonts
        if let Some(descendant_fonts) = fd.get("DescendantFonts") {
            // /DescendantFonts is usually an INDIRECT REF to an array — resolve it first.
            let df_resolved = xref.resolve(data, descendant_fonts);
            let first_desc_owned = match &df_resolved {
                PdfObject::Array(arr) if !arr.is_empty() => Some(arr[0].clone()),
                _ => None,
            };
            let cid_font_dict = if let Some(ref_item) = first_desc_owned {
                let resolved = xref.resolve(data, &ref_item);
                if let PdfObject::Dict(d) = resolved { Some(d) } else { None }
            } else { None };

            if let Some(cid_font) = cid_font_dict {
                // Read FontDescriptor from CIDFont dict
                if let Some(fd_ref) = cid_font.get("FontDescriptor") {
                    let cid_fd = xref.resolve(data, fd_ref);
                    if let PdfObject::Dict(desc_dict) = cid_fd {
                        for key in &["FontFile2", "FontFile3"] {
                            if let Some(ff_ref) = desc_dict.get(*key) {
                                let ff = xref.resolve(data, ff_ref);
                                if let PdfObject::Stream { data: font_bytes, .. } = ff {
                                    is_embedded = true;
                                    if let Some((id, ratio)) = font_cache.register_with_advance(&font_bytes) {
                                        font_id = id;
                                        advance_ratio = ratio;
                                    }
                                    break;
                                }
                            }
                        }
                    }
                }
            }
        }
        // Encoding hints CID usage
        if let Some(enc) = fd.get("Encoding") {
            if let Some(enc_name) = enc.as_name() {
                if enc_name == "Identity-H" || enc_name == "Identity-V" {
                    cid_bool = true;
                    // If needed we could set from resolved font_id; nothing else to do here
                }
            } else {
                // When Encoding is not a Name, assume CID (2-byte) scenario
                cid_bool = true;
            }
        }
    } else {
        // Non-Type0 fonts - keep existing behavior
        if let Some(desc_ref) = fd.get("FontDescriptor") {
            let desc = xref.resolve(data, desc_ref);
            if let PdfObject::Dict(desc_dict) = desc {
                for key in &["FontFile2", "FontFile3", "FontFile"] {
                    if let Some(ff_ref) = desc_dict.get(*key) {
                        let ff = xref.resolve(data, ff_ref);
                        if let PdfObject::Stream { data: font_bytes, .. } = ff {
                            is_embedded = true;
                            if let Some((id, ratio)) = font_cache.register_with_advance(&font_bytes) {
                                font_id = id;
                                advance_ratio = ratio;
                            }
                            break;
                        }
                    }
                }
            }
        }
        // For non-Type0 fonts, encoding is typically single-byte; ToUnicode handles extraction.
    }

    // Parse /ToUnicode CMap stream if present.
    if let Some(tu_ref) = fd.get("ToUnicode") {
        let tu_obj = xref.resolve(data, tu_ref);
        if let PdfObject::Stream { data: cmap_data, .. } = tu_obj {
            to_unicode = cmap::parse_cmap(&cmap_data);
        }
    }

    FontResource {
        font_id,
        base_font,
        is_embedded,
        to_unicode,
        advance_ratio,
        cid: cid_bool,
    }
}
