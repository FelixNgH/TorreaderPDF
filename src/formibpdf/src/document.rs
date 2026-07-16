use crate::pdf_object::PdfObject;
use crate::xref::XRefTable;
use std::collections::HashMap;

#[derive(Debug, Clone)]
pub struct PageInfo {
    pub width_pt:  f64,
    pub height_pt: f64,
    pub content:   Vec<u8>, // decompressed, concatenated content stream bytes
    pub page_dict: HashMap<String, PdfObject>, // raw page dict for resource loading
}

pub struct PdfDocumentReader {
    pub data:    Vec<u8>,
    pub xref:    XRefTable,
    pub pages:   Vec<PageInfo>,
    pub catalog: HashMap<String, PdfObject>,
}

impl PdfDocumentReader {
    pub fn open(data: Vec<u8>) -> Result<Self, String> {
        let xref = XRefTable::parse(&data)?;

        // /Root → Catalog
        let root_ref = xref.trailer.get("Root")
            .cloned()
            .ok_or("no /Root in trailer")?;
        let catalog = xref.resolve(&data, &root_ref);

        // /Pages → root Pages node
        let pages_ref = catalog.dict_get("Pages")
            .ok_or("no /Pages in catalog")?.clone();
        let pages_root = xref.resolve(&data, &pages_ref);

        // Traverse page tree
        let mut pages_raw: Vec<PdfObject> = Vec::new();
        collect_pages(&xref, &data, &pages_root, &mut pages_raw, 0);

        // Build PageInfo
        let mut pages: Vec<PageInfo> = Vec::new();
        for page_obj in pages_raw {
            let info = build_page_info(&xref, &data, &page_obj);
            pages.push(info);
        }

        let catalog_dict = match catalog {
            PdfObject::Dict(d) => d,
            _ => HashMap::new(),
        };
        Ok(PdfDocumentReader { data, xref, pages, catalog: catalog_dict })
    }

    pub fn page_count(&self) -> usize { self.pages.len() }
    pub fn page(&self, i: usize) -> Option<&PageInfo> { self.pages.get(i) }
}

// Traverse /Pages tree recursively, collect leaf /Page nodes.
fn collect_pages(xref: &XRefTable, data: &[u8], node: &PdfObject,
                 out: &mut Vec<PdfObject>, depth: u8) {
    if depth > 32 { return; } // guard against malformed PDFs

    // Resolve ref if needed
    let resolved;
    let obj = match node {
        PdfObject::Ref(id, _) => {
            resolved = xref.load_object(data, *id).unwrap_or(PdfObject::Null);
            &resolved
        }
        other => other,
    };

    match obj {
        PdfObject::Dict(d) => {
            let typ = d.get("Type").and_then(|t| t.as_name()).unwrap_or("");
            match typ {
                "Pages" => {
                    if let Some(PdfObject::Array(kids)) = d.get("Kids") {
                        let kids = kids.clone(); // clone to avoid borrow issues
                        for kid in &kids {
                            collect_pages(xref, data, kid, out, depth + 1);
                        }
                    }
                }
                "Page" => {
                    out.push(obj.clone());
                }
                _ => {
                    // Unknown type — check if it has /Kids (might be a Pages node without /Type)
                    if d.contains_key("Kids") {
                        if let Some(PdfObject::Array(kids)) = d.get("Kids") {
                            let kids = kids.clone();
                            for kid in &kids { collect_pages(xref, data, kid, out, depth+1); }
                        }
                    } else if d.contains_key("Contents") || d.contains_key("MediaBox") {
                        out.push(obj.clone()); // looks like a page
                    }
                }
            }
        }
        _ => {}
    }
}

fn build_page_info(xref: &XRefTable, data: &[u8], page_obj: &PdfObject) -> PageInfo {
    // MediaBox → [x0 y0 x1 y1] (raw edges, not swapped)
    let (mb_x0, mb_y0, mb_x1, mb_y1) = {
        let mb = page_obj.dict_get("MediaBox");
        if let Some(PdfObject::Array(arr)) = mb {
            let v: Vec<f64> = arr.iter().map(|x| x.as_f64().unwrap_or(0.0)).collect();
            (v.get(0).copied().unwrap_or(0.0), v.get(1).copied().unwrap_or(0.0),
             v.get(2).copied().unwrap_or(612.0), v.get(3).copied().unwrap_or(792.0))
        } else {
            (0.0, 0.0, 612.0, 792.0) // default Letter
        }
    };

    // CropBox → visible region. Absent → fall back to MediaBox. (Bug E: ignoring
    // CropBox on architectural drawings produced extra top/bottom margins.)
    let (cb_x0, cb_y0, cb_x1, cb_y1) = {
        let cb = page_obj.dict_get("CropBox");
        if let Some(PdfObject::Array(arr)) = cb {
            let v: Vec<f64> = arr.iter().map(|x| x.as_f64().unwrap_or(0.0)).collect();
            (v.get(0).copied().unwrap_or(mb_x0), v.get(1).copied().unwrap_or(mb_y0),
             v.get(2).copied().unwrap_or(mb_x1), v.get(3).copied().unwrap_or(mb_y1))
        } else {
            (mb_x0, mb_y0, mb_x1, mb_y1)
        }
    };

    // Effective box = CropBox intersected with MediaBox (crop can never exceed media).
    let (bx0, by0, bx1, by1) = (
        cb_x0.max(mb_x0).min(mb_x1),
        cb_y0.max(mb_y0).min(mb_y1),
        cb_x1.max(mb_x0).min(mb_x1),
        cb_y1.max(mb_y0).min(mb_y1),
    );
    let crop_w = (bx1 - bx0).abs();
    let crop_h = (by1 - by0).abs();

    // /Rotate — degrees CCW to rotate when displaying.
    let rotate = page_obj.dict_get("Rotate")
        .and_then(|o| o.as_i64())
        .unwrap_or(0) as u32 % 360;

    // Content transform: first translate so the effective box lower-left maps to
    // user-space origin (0,0), then apply rotation (uses crop dims, not media).
    let cm_prefix: Vec<u8> = {
        let mut v = format!("1 0 0 1 {} {} cm\n", -bx0, -by0).into_bytes();
        match rotate {
            90  => v.extend_from_slice(format!("0 1 -1 0 {} 0 cm\n", crop_h).as_bytes()),
            180 => v.extend_from_slice(format!("-1 0 0 -1 {} {} cm\n", crop_w, crop_h).as_bytes()),
            270 => v.extend_from_slice(format!("0 -1 1 0 0 {} cm\n", crop_w).as_bytes()),
            _   => {}
        }
        v
    };

    // Effective page dimensions after rotation.
    let (width_pt, height_pt) = match rotate {
        90 | 270 => (crop_h, crop_w), // landscape swap
        _        => (crop_w, crop_h),
    };

    // /Contents — single stream ref or array of refs
    let raw = extract_contents(xref, data, page_obj);
    let mut content = cm_prefix;
    content.extend_from_slice(&raw);

    let page_dict = match page_obj {
        PdfObject::Dict(d) => d.clone(),
        _ => HashMap::new(),
    };
    PageInfo { width_pt, height_pt, content, page_dict }
}

fn extract_contents(xref: &XRefTable, data: &[u8], page_obj: &PdfObject) -> Vec<u8> {
    let contents = match page_obj.dict_get("Contents") {
        Some(c) => c.clone(),
        None    => return Vec::new(),
    };

    // /Contents may be: a stream ref, an array of stream refs, OR an indirect ref
    // to an array (Revit/Bluebeam exports). Resolve a lone Ref first: if it loads
    // to a Stream use it directly; if it loads to an Array fall through to the
    // array path. (Previously a Ref→Array returned empty → whole page blank.)
    let contents = match &contents {
        PdfObject::Ref(id, _) => {
            match xref.load_object(data, *id) {
                Some(PdfObject::Stream { data: d, .. }) => return d,
                Some(other @ PdfObject::Array(_)) => other,
                _ => return Vec::new(),
            }
        }
        other => other.clone(),
    };

    match &contents {
        PdfObject::Ref(id, _) => {
            load_stream_data(xref, data, *id)
        }
        PdfObject::Array(arr) => {
            let mut merged = Vec::new();
            for item in arr {
                if let PdfObject::Ref(id, _) = item {
                    let bytes = load_stream_data(xref, data, *id);
                    if !bytes.is_empty() {
                        if !merged.is_empty() { merged.push(b' '); }
                        merged.extend(bytes);
                    }
                }
            }
            merged
        }
        PdfObject::Stream { data: d, .. } => d.clone(),
        _ => Vec::new(),
    }
}

fn load_stream_data(xref: &XRefTable, data: &[u8], obj_id: u32) -> Vec<u8> {
    match xref.load_object(data, obj_id) {
        Some(PdfObject::Stream { data: d, .. }) => d,
        _ => Vec::new(),
    }
}
