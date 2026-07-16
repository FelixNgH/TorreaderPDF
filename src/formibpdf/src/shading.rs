// shading.rs — PDF shading (gradient) types
// Supports Type 2 (axial/linear), Type 3 (radial), and Types 4-7 (mesh) shadings.

use crate::graphics_state::Color;

/// A single triangle in a mesh shading (Types 4-7).
/// Colors are 4-component floats [c0..c3] (RGB+alpha or CMYK depending on context).
#[derive(Debug, Clone)]
pub struct ShadingTriangle {
    pub p0: (f64, f64),
    pub p1: (f64, f64),
    pub p2: (f64, f64),
    pub c0: [f32; 4],
    pub c1: [f32; 4],
    pub c2: [f32; 4],
}

// ── Color stop interpolation ──────────────────────────────────────────────────

fn lerp_color(c0: &[f32], c1: &[f32], t: f32) -> Color {
    let t = t.clamp(0.0, 1.0);
    match c0.len().min(c1.len()) {
        1 => Color::from_gray(c0[0] + t * (c1[0] - c0[0])),
        3 => Color::from_rgb(
            c0[0] + t * (c1[0] - c0[0]),
            c0[1] + t * (c1[1] - c0[1]),
            c0[2] + t * (c1[2] - c0[2]),
        ),
        4 => Color::from_cmyk(
            c0[0] + t * (c1[0] - c0[0]),
            c0[1] + t * (c1[1] - c0[1]),
            c0[2] + t * (c1[2] - c0[2]),
            c0[3] + t * (c1[3] - c0[3]),
        ),
        _ => Color::black(),
    }
}

// ── Shading kinds ─────────────────────────────────────────────────────────────

#[derive(Clone, Debug)]
pub enum ShadingKind {
    /// Type 2: Axial (linear) gradient — two endpoints, color interpolated along axis.
    Axial {
        x0: f64, y0: f64,   // start point (user space)
        x1: f64, y1: f64,   // end point   (user space)
        c0: Vec<f32>,        // color at t=0
        c1: Vec<f32>,        // color at t=1
        extend0: bool,       // extend beyond start
        extend1: bool,       // extend beyond end
    },
    /// Type 3: Radial gradient — two circles, color interpolated between them.
    Radial {
        x0: f64, y0: f64, r0: f64,  // inner circle (user space)
        x1: f64, y1: f64, r1: f64,  // outer circle
        c0: Vec<f32>,
        c1: Vec<f32>,
        extend0: bool,
        extend1: bool,
    },
    /// Types 4-7: Mesh shading (Gouraud / Coons) — triangulated representation.
    Mesh {
        triangles:   Vec<ShadingTriangle>,
        color_comps: u8,   // 1=gray, 3=RGB, 4=CMYK
    },
}

impl ShadingKind {
    /// Sample the gradient color at parameter t ∈ [0,1].
    pub fn sample(&self, t: f32) -> Color {
        match self {
            ShadingKind::Axial  { c0, c1, .. } => lerp_color(c0, c1, t),
            ShadingKind::Radial { c0, c1, .. } => lerp_color(c0, c1, t),
            ShadingKind::Mesh   { .. }          => Color::black(),
        }
    }

    /// Return the extend-start flag.
    pub fn extend0(&self) -> bool {
        match self {
            ShadingKind::Axial  { extend0, .. } => *extend0,
            ShadingKind::Radial { extend0, .. } => *extend0,
            ShadingKind::Mesh   { .. }           => false,
        }
    }

    /// Return the extend-end flag.
    pub fn extend1(&self) -> bool {
        match self {
            ShadingKind::Axial  { extend1, .. } => *extend1,
            ShadingKind::Radial { extend1, .. } => *extend1,
            ShadingKind::Mesh   { .. }           => false,
        }
    }
}

// ── Parser ────────────────────────────────────────────────────────────────────

use crate::pdf_object::PdfObject;
use crate::xref::XRefTable;
use std::collections::HashMap;

/// Parse a shading dictionary into a ShadingKind.
/// `stream_bytes` is the decoded stream data for mesh shadings (Types 4-7).
pub fn parse_shading(
    dict:         &HashMap<String, PdfObject>,
    stream_bytes: Option<&[u8]>,
    xref:         &XRefTable,
    data:         &[u8],
) -> Option<ShadingKind> {
    let shading_type = dict.get("ShadingType").and_then(|o| o.as_i64())?;

    // Parse extend flags ([extend0, extend1], default [false, false])
    let (extend0, extend1) = parse_extend(dict);

    // Parse the color function — we support the exponential (Type 2) function
    // and stitching (Type 3) as a wrapper. For stitching we just use the
    // endpoints of the first and last sub-functions.
    let (c0, c1) = parse_function_endpoints(dict, xref, data)?;

    match shading_type {
        2 => {
            // Axial: /Coords [x0 y0 x1 y1]
            let coords = parse_float_array(dict, "Coords")?;
            if coords.len() < 4 { return None; }
            Some(ShadingKind::Axial {
                x0: coords[0], y0: coords[1],
                x1: coords[2], y1: coords[3],
                c0, c1, extend0, extend1,
            })
        }
        3 => {
            // Radial: /Coords [x0 y0 r0 x1 y1 r1]
            let coords = parse_float_array(dict, "Coords")?;
            if coords.len() < 6 { return None; }
            Some(ShadingKind::Radial {
                x0: coords[0], y0: coords[1], r0: coords[2],
                x1: coords[3], y1: coords[4], r1: coords[5],
                c0, c1, extend0, extend1,
            })
        }
        4 | 5 | 6 | 7 => parse_mesh_shading(shading_type, stream_bytes.unwrap_or(&[]), dict),
        _ => None,
    }
}

// ── Mesh shading parser (Types 4-7) ──────────────────────────────────────────

fn parse_mesh_shading(
    shading_type: i64,
    stream_bytes: &[u8],
    dict:         &HashMap<String, PdfObject>,
) -> Option<ShadingKind> {
    let bpc_coord  = dict.get("BitsPerCoordinate").and_then(|o| o.as_i64()).unwrap_or(8) as u32;
    let bpc_color  = dict.get("BitsPerComponent").and_then(|o| o.as_i64()).unwrap_or(8) as u32;
    let decode_arr: Vec<f64> = dict.get("Decode")
        .and_then(|o| o.as_array())
        .map(|a| a.iter().filter_map(|x| x.as_f64()).collect())
        .unwrap_or_else(|| vec![0.0, 1.0, 0.0, 1.0]);

    // Determine number of color components from decode array
    let n_coord_decode = 4usize; // x_min,x_max,y_min,y_max
    let color_comps = ((decode_arr.len().saturating_sub(n_coord_decode)) / 2).max(1).min(4) as u8;

    let x_min = decode_arr.get(0).copied().unwrap_or(0.0);
    let x_max = decode_arr.get(1).copied().unwrap_or(1.0);
    let y_min = decode_arr.get(2).copied().unwrap_or(0.0);
    let y_max = decode_arr.get(3).copied().unwrap_or(1.0);

    let mut c_min = [0.0f32; 4];
    let mut c_max = [1.0f32; 4];
    for i in 0..(color_comps as usize).min(4) {
        c_min[i] = decode_arr.get(4 + i*2).copied().unwrap_or(0.0) as f32;
        c_max[i] = decode_arr.get(5 + i*2).copied().unwrap_or(1.0) as f32;
    }

    let max_coord  = ((1u64 << bpc_coord)  - 1) as f64;
    let max_color  = ((1u64 << bpc_color)  - 1) as f64;

    // MSB-first bit reader
    let mut br_pos  = 0usize;
    let mut br_bits = 0u32;
    let mut br_buf  = 0u32;

    let mut read_bits = |n: u32| -> u32 {
        while br_bits < n {
            if br_pos >= stream_bytes.len() { return 0; }
            br_buf  = (br_buf << 8) | stream_bytes[br_pos] as u32;
            br_bits += 8;
            br_pos  += 1;
        }
        br_bits -= n;
        (br_buf >> br_bits) & ((1 << n) - 1)
    };

    let read_vertex = |read_bits: &mut dyn FnMut(u32) -> u32, has_flag: bool| -> Option<((f64, f64), [f32; 4], u8)> {
        let flag = if has_flag { read_bits(8) as u8 } else { 0 };
        let rx = read_bits(bpc_coord) as f64 / max_coord;
        let ry = read_bits(bpc_coord) as f64 / max_coord;
        let px = x_min + rx * (x_max - x_min);
        let py = y_min + ry * (y_max - y_min);
        let mut color = [0f32; 4];
        for i in 0..(color_comps as usize) {
            let raw = read_bits(bpc_color) as f64 / max_color;
            color[i] = c_min[i] + (raw as f32) * (c_max[i] - c_min[i]);
        }
        Some(((px, py), color, flag))
    };

    let mut triangles: Vec<ShadingTriangle> = Vec::new();

    match shading_type {
        4 => {
            // Free-form Gouraud: flag + vertex, build strips
            let mut verts: Vec<((f64, f64), [f32; 4])> = Vec::new();
            loop {
                match read_vertex(&mut read_bits, true) {
                    Some((p, c, 0)) => {
                        verts.clear();
                        verts.push((p, c));
                    }
                    Some((p, c, 1)) if verts.len() >= 2 => {
                        let (p1, c1) = verts[verts.len()-2].clone();
                        let (p2, c2) = verts[verts.len()-1].clone();
                        triangles.push(ShadingTriangle { p0: p1, p1: p2, p2: p, c0: c1, c1: c2, c2: c });
                        verts.push((p, c));
                    }
                    Some((p, c, 2)) if verts.len() >= 2 => {
                        let (p1, c1) = verts[verts.len()-2].clone();
                        let (p2, c2) = verts.last().unwrap().clone();
                        triangles.push(ShadingTriangle { p0: p1, p1: p, p2: p2, c0: c1, c1: c, c2: c2 });
                        verts.push((p, c));
                    }
                    None | Some((_, _, _)) => break,
                }
                if triangles.len() > 100_000 { break; }
            }
        }
        5 => {
            // Lattice Gouraud: vertices per row, build quads→triangles
            let vpr = dict.get("VerticesPerRow").and_then(|o| o.as_i64()).unwrap_or(2) as usize;
            if vpr < 2 { return Some(ShadingKind::Mesh { triangles, color_comps }); }
            let mut rows: Vec<Vec<((f64,f64),[f32;4])>> = Vec::new();
            loop {
                let mut row = Vec::new();
                for _ in 0..vpr {
                    match read_vertex(&mut read_bits, false) {
                        Some((p, c, _)) => row.push((p, c)),
                        None => break,
                    }
                }
                if row.len() < vpr { break; }
                rows.push(row);
                if rows.len() > 10_000 { break; }
            }
            for r in 0..rows.len().saturating_sub(1) {
                for c in 0..vpr.saturating_sub(1) {
                    let (p00, c00) = rows[r  ][c  ].clone();
                    let (p10, c10) = rows[r  ][c+1].clone();
                    let (p01, c01) = rows[r+1][c  ].clone();
                    let (p11, c11) = rows[r+1][c+1].clone();
                    triangles.push(ShadingTriangle { p0: p00, p1: p10, p2: p01, c0: c00, c1: c10, c2: c01 });
                    triangles.push(ShadingTriangle { p0: p10, p1: p11, p2: p01, c0: c10, c1: c11, c2: c01 });
                }
            }
        }
        6 | 7 => {
            // Coons/Tensor patch: 12 control points + 4 corner colors, subdivide
            let n_ctrl = if shading_type == 7 { 16 } else { 12 };
            loop {
                let _flag = read_bits(8);
                let mut pts = Vec::new();
                for _ in 0..n_ctrl {
                    let rx = read_bits(bpc_coord) as f64 / max_coord;
                    let ry = read_bits(bpc_coord) as f64 / max_coord;
                    pts.push((x_min + rx*(x_max-x_min), y_min + ry*(y_max-y_min)));
                }
                let mut colors = [[0f32; 4]; 4];
                for corner in &mut colors {
                    for i in 0..(color_comps as usize) {
                        let raw = read_bits(bpc_color) as f64 / max_color;
                        corner[i] = c_min[i] + (raw as f32) * (c_max[i] - c_min[i]);
                    }
                }
                if pts.len() < n_ctrl { break; }
                // Subdivide patch to 4×4 = 16 quads → 32 triangles (de Casteljau level 4)
                let res = 4usize;
                for j in 0..res {
                    for i in 0..res {
                        let u0 = i as f64 / res as f64;
                        let u1 = (i+1) as f64 / res as f64;
                        let v0 = j as f64 / res as f64;
                        let v1 = (j+1) as f64 / res as f64;
                        let p00 = bilinear_patch(&pts, u0, v0);
                        let p10 = bilinear_patch(&pts, u1, v0);
                        let p01 = bilinear_patch(&pts, u0, v1);
                        let p11 = bilinear_patch(&pts, u1, v1);
                        let c00 = lerp_corner_colors(&colors, u0 as f32, v0 as f32);
                        let c10 = lerp_corner_colors(&colors, u1 as f32, v0 as f32);
                        let c01 = lerp_corner_colors(&colors, u0 as f32, v1 as f32);
                        let c11 = lerp_corner_colors(&colors, u1 as f32, v1 as f32);
                        triangles.push(ShadingTriangle { p0: p00, p1: p10, p2: p01, c0: c00, c1: c10, c2: c01 });
                        triangles.push(ShadingTriangle { p0: p10, p1: p11, p2: p01, c0: c10, c1: c11, c2: c01 });
                    }
                }
                if triangles.len() > 100_000 { break; }
            }
        }
        _ => {}
    }

    Some(ShadingKind::Mesh { triangles, color_comps })
}

fn bilinear_patch(pts: &[(f64,f64)], u: f64, v: f64) -> (f64, f64) {
    // Simplified bilinear from 4 corner points (0, 3, n-4, n-1 or corners of pts)
    let n = pts.len();
    if n < 4 { return pts.first().copied().unwrap_or((0.0, 0.0)); }
    let p00 = pts[0]; let p30 = pts[3];
    let p03 = pts[n-4]; let p33 = pts[n-1];
    let x = (1.0-u)*(1.0-v)*p00.0 + u*(1.0-v)*p30.0 + (1.0-u)*v*p03.0 + u*v*p33.0;
    let y = (1.0-u)*(1.0-v)*p00.1 + u*(1.0-v)*p30.1 + (1.0-u)*v*p03.1 + u*v*p33.1;
    (x, y)
}

fn lerp_corner_colors(colors: &[[f32;4]; 4], u: f32, v: f32) -> [f32; 4] {
    let mut out = [0f32; 4];
    for i in 0..4 {
        out[i] = (1.0-u)*(1.0-v)*colors[0][i] + u*(1.0-v)*colors[1][i]
               + (1.0-u)*v*colors[2][i] + u*v*colors[3][i];
    }
    out
}

// ── Helpers ───────────────────────────────────────────────────────────────────

fn parse_extend(dict: &HashMap<String, PdfObject>) -> (bool, bool) {
    let arr = match dict.get("Extend") {
        Some(PdfObject::Array(a)) => a,
        _ => return (false, false),
    };
    let b = |o: Option<&PdfObject>| matches!(o, Some(PdfObject::Bool(true)));
    (b(arr.get(0)), b(arr.get(1)))
}

fn parse_float_array(dict: &HashMap<String, PdfObject>, key: &str) -> Option<Vec<f64>> {
    if let Some(PdfObject::Array(a)) = dict.get(key) {
        Some(a.iter().filter_map(|o| o.as_f64()).collect())
    } else {
        None
    }
}

/// Extract the c0 and c1 color stop vectors from the shading function.
/// Supports PDF Function Type 2 (exponential) and Type 3 (stitching).
fn parse_function_endpoints(
    shading_dict: &HashMap<String, PdfObject>,
    xref:         &XRefTable,
    data:         &[u8],
) -> Option<(Vec<f32>, Vec<f32>)> {
    let func_obj = shading_dict.get("Function")?;
    let func_resolved = xref.resolve(data, func_obj);
    let func_dict: &HashMap<String, PdfObject> = match &func_resolved {
        PdfObject::Dict(d)             => d,
        PdfObject::Stream { dict, .. } => dict,
        _ => return None,
    };
    // Use a local copy to allow returning references
    let func_dict_clone: HashMap<String, PdfObject> = func_dict.clone();
    extract_endpoints(&func_dict_clone, xref, data)
}

fn extract_endpoints(
    fd:   &HashMap<String, PdfObject>,
    xref: &XRefTable,
    data: &[u8],
) -> Option<(Vec<f32>, Vec<f32>)> {
    let func_type = fd.get("FunctionType").and_then(|o| o.as_i64()).unwrap_or(2);

    match func_type {
        2 => {
            // Exponential interpolation: C0 and C1 arrays
            let c0 = array_to_f32(fd.get("C0"));
            let c1 = array_to_f32(fd.get("C1"));
            // If C0/C1 absent, defaults are 0.0 and 1.0 (single component)
            let c0 = if c0.is_empty() { vec![0.0f32] } else { c0 };
            let c1 = if c1.is_empty() { vec![1.0f32] } else { c1 };
            Some((c0, c1))
        }
        3 => {
            // Stitching: /Functions array of sub-functions
            // Use first function's C0 and last function's C1
            if let Some(PdfObject::Array(funcs)) = fd.get("Functions") {
                let first = funcs.first().map(|o| xref.resolve(data, o));
                let last  = funcs.last().map(|o|  xref.resolve(data, o));

                let c0 = first.as_ref().and_then(|o| {
                    if let PdfObject::Dict(d) = o { Some(array_to_f32(d.get("C0"))) }
                    else { None }
                }).unwrap_or_else(|| vec![0.0]);

                let c1 = last.as_ref().and_then(|o| {
                    if let PdfObject::Dict(d) = o { Some(array_to_f32(d.get("C1"))) }
                    else { None }
                }).unwrap_or_else(|| vec![1.0]);

                Some((c0, c1))
            } else {
                None
            }
        }
        _ => None,
    }
}

fn array_to_f32(obj: Option<&PdfObject>) -> Vec<f32> {
    match obj {
        Some(PdfObject::Array(a)) => a.iter().filter_map(|o| o.as_f64().map(|v| v as f32)).collect(),
        Some(PdfObject::Integer(i)) => vec![*i as f32],
        Some(PdfObject::Real(r))    => vec![*r as f32],
        _ => Vec::new(),
    }
}
