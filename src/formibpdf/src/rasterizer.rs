use crate::interpreter::RenderCommand;
use crate::path::{Path, PathCmd};
use crate::graphics_state::{BlendMode, SmaskDef, SmaskType, Color, Matrix};
use crate::font::FontCache;
use crate::shading::ShadingKind;
use std::sync::Arc;

// ── Polygon clip mask ─────────────────────────────────────────────────────────

/// A per-pixel boolean mask: `true` means the pixel is inside the clip region
/// and should be drawn; `false` means it is outside and should be skipped.
///
/// Masks are combined by intersection (AND): a pixel must be inside *every*
/// accumulated clip path to receive paint.  The initial state (all-inside) is
/// created by `ClipMask::new_all_inside`.
#[derive(Clone)]
pub struct ClipMask {
    pub width:  u32,
    pub height: u32,
    /// Row-major, length == width * height.
    pub pixels: Vec<bool>,
}

impl ClipMask {
    /// Create a mask where every pixel is considered inside (no clip active).
    pub fn new_all_inside(width: u32, height: u32) -> Self {
        ClipMask {
            width,
            height,
            pixels: vec![true; (width * height) as usize],
        }
    }

    /// Rasterise `path` (user-space coordinates, transformed via `ctm`) into a
    /// temporary coverage mask, then AND it with `self.pixels`.
    ///
    /// `even_odd` selects the fill rule:
    ///   - `true`  → even-odd rule  (`W*` operator)
    ///   - `false` → non-zero winding rule (`W` operator)
    pub fn apply_path(&mut self, path: &Path, ctm: &Matrix,
                      scale: f64, page_h: f64, even_odd: bool) {
        let polylines = collect_polylines(path, ctm, scale, page_h);

        let mut new_mask = vec![false; (self.width * self.height) as usize];

        for y in 0..self.height as i32 {
            let yf = y as f64 + 0.5;

            if even_odd {
                // ---- Even-odd rule: collect x-crossings, fill alternate spans ----
                let mut xs: Vec<f64> = Vec::new();
                for poly in &polylines {
                    let n = poly.len();
                    for i in 0..n.saturating_sub(1) {
                        let (x0, y0) = poly[i];
                        let (x1, y1) = poly[i + 1];
                        if (y0 <= yf && y1 > yf) || (y1 <= yf && y0 > yf) {
                            let t = (yf - y0) / (y1 - y0);
                            xs.push(x0 + t * (x1 - x0));
                        }
                    }
                    // Close the subpath edge
                    if n >= 2 {
                        let (x0, y0) = poly[n - 1];
                        let (x1, y1) = poly[0];
                        if (y0 <= yf && y1 > yf) || (y1 <= yf && y0 > yf) {
                            let t = (yf - y0) / (y1 - y0);
                            xs.push(x0 + t * (x1 - x0));
                        }
                    }
                }
                xs.sort_by(|a, b| a.partial_cmp(b).unwrap_or(std::cmp::Ordering::Equal));
                let mut k = 0;
                while k + 1 < xs.len() {
                    let xa = xs[k].floor() as i32;
                    let xb = xs[k + 1].ceil() as i32;
                    for x in xa.max(0)..xb.min(self.width as i32) {
                        new_mask[(y * self.width as i32 + x) as usize] = true;
                    }
                    k += 2;
                }
            } else {
                // ---- Non-zero winding rule: track signed winding count ----
                // Collect (x_intersection, winding_delta) pairs
                let mut crossings: Vec<(f64, i32)> = Vec::new();
                for poly in &polylines {
                    let n = poly.len();
                    // Segment pairs within the polyline
                    for i in 0..n.saturating_sub(1) {
                        let (x0, y0) = poly[i];
                        let (x1, y1) = poly[i + 1];
                        if y0 <= yf && y1 > yf {
                            // Upward crossing
                            let t = (yf - y0) / (y1 - y0);
                            crossings.push((x0 + t * (x1 - x0), 1));
                        } else if y1 <= yf && y0 > yf {
                            // Downward crossing
                            let t = (yf - y0) / (y1 - y0);
                            crossings.push((x0 + t * (x1 - x0), -1));
                        }
                    }
                    // Closing edge
                    if n >= 2 {
                        let (x0, y0) = poly[n - 1];
                        let (x1, y1) = poly[0];
                        if y0 <= yf && y1 > yf {
                            let t = (yf - y0) / (y1 - y0);
                            crossings.push((x0 + t * (x1 - x0), 1));
                        } else if y1 <= yf && y0 > yf {
                            let t = (yf - y0) / (y1 - y0);
                            crossings.push((x0 + t * (x1 - x0), -1));
                        }
                    }
                }
                crossings.sort_by(|a, b| a.0.partial_cmp(&b.0).unwrap_or(std::cmp::Ordering::Equal));

                // Walk pixels left-to-right, maintaining winding count
                let mut winding = 0i32;
                let mut ci = 0usize;
                for x in 0..self.width as i32 {
                    let xf = x as f64 + 0.5;
                    // Consume all crossings to the left of xf
                    while ci < crossings.len() && crossings[ci].0 <= xf {
                        winding += crossings[ci].1;
                        ci += 1;
                    }
                    if winding != 0 {
                        new_mask[(y * self.width as i32 + x) as usize] = true;
                    }
                }
            }
        }

        // AND: a pixel is inside only if it was already inside AND is inside
        // the new path.
        for (old, new) in self.pixels.iter_mut().zip(new_mask.iter()) {
            *old = *old && *new;
        }
    }
}

// ── Canvas ────────────────────────────────────────────────────────────────────

pub struct Canvas {
    pub pixels: Vec<u8>,
    pub width:  u32,
    pub height: u32,
    /// Fast axis-aligned rect clip (x_min, y_min, x_max, y_max) in pixel space.
    clip_rect: Option<(i32, i32, i32, i32)>,
    /// Polygon clip mask for complex (non-rectangular) clip paths.
    clip_mask: Option<ClipMask>,
    /// Graphics-state clip stack – pushed/popped by SaveState/RestoreState.
    clip_stack: Vec<(Option<(i32, i32, i32, i32)>, Option<ClipMask>)>,
    /// Current blend mode — set from RenderCommand::PaintPath.blend_mode.
    blend_mode: BlendMode,
}

impl Canvas {
    pub fn new(width: u32, height: u32) -> Self {
        Canvas {
            pixels:     vec![255u8; (width * height * 4) as usize],
            width, height,
            clip_rect:  None, clip_mask: None, clip_stack: Vec::new(),
            blend_mode: BlendMode::Normal,
        }
    }

    /// Like new() but starts fully transparent (all pixels = 0). Used for offscreen rendering.
    pub fn new_transparent(width: u32, height: u32) -> Self {
        Canvas {
            pixels:     vec![0u8; (width * height * 4) as usize],
            width, height,
            clip_rect:  None, clip_mask: None, clip_stack: Vec::new(),
            blend_mode: BlendMode::Normal,
        }
    }

    // ── Clip management ───────────────────────────────────────────────────────

    /// Set an axis-aligned rect clip from a user-space rectangle.
    /// Converts via CTM + scale + Y-flip to pixel-space AABB, then stores in
    /// `clip_rect`.  (Kept for backward compatibility with the simple W case.)
    pub fn set_clip_user(&mut self, rect: (f64, f64, f64, f64),
                         ctm: &Matrix, scale: f64, page_h: f64) {
        let (x0, y0) = to_screen(rect.0, rect.1, ctm, scale, page_h);
        let (x1, y1) = to_screen(rect.2, rect.1, ctm, scale, page_h);
        let (x2, y2) = to_screen(rect.2, rect.3, ctm, scale, page_h);
        let (x3, y3) = to_screen(rect.0, rect.3, ctm, scale, page_h);
        let sx = x0.min(x1).min(x2).min(x3).floor() as i32;
        let sy = y0.min(y1).min(y2).min(y3).floor() as i32;
        let ex = x0.max(x1).max(x2).max(x3).ceil()  as i32;
        let ey = y0.max(y1).max(y2).max(y3).ceil()  as i32;
        self.clip_rect = Some((sx, sy, ex, ey));
    }

    /// Apply a polygon clip path (W / W* operators for non-rectangular paths).
    /// Creates the mask lazily (all-inside on first call), then ANDs the path
    /// into the existing mask so successive clips narrow the region.
    pub fn set_clip_path(&mut self, path: &Path, ctm: &Matrix,
                         scale: f64, page_h: f64, even_odd: bool) {
        let mask = self.clip_mask.get_or_insert_with(||
            ClipMask::new_all_inside(self.width, self.height));
        mask.apply_path(path, ctm, scale, page_h, even_odd);
    }

    /// Save the current clip state onto the internal stack (called for `q`).
    pub fn save_clip(&mut self) {
        self.clip_stack.push((self.clip_rect, self.clip_mask.clone()));
    }

    /// Restore the clip state from the top of the internal stack (called for `Q`).
    pub fn restore_clip(&mut self) {
        if let Some((rect, mask)) = self.clip_stack.pop() {
            self.clip_rect = rect;
            self.clip_mask = mask;
        }
    }

    /// Remove all clip state (reset to full-canvas drawing).
    pub fn clear_clip(&mut self) {
        self.clip_rect = None;
        self.clip_mask = None;
    }

    // ── Render ────────────────────────────────────────────────────────────────

    /// Render all commands onto the canvas.  `font_cache` persists across pages.
    pub fn render(&mut self, cmds: &[RenderCommand], page_height: f64, scale: f64,
                  font_cache: &FontCache) {
        for cmd in cmds {
            match cmd {
                RenderCommand::PaintPath { path, fill_color, stroke_color,
                                           line_width, line_cap, line_join, ctm,
                                           blend_mode, smask, .. } => {
                    if let Some(sm) = smask {
                        // SMask: render path to offscreen canvas, compute mask, composite.
                        let mut tmp = Canvas::new_transparent(self.width, self.height);
                        tmp.blend_mode = *blend_mode;
                        if let Some(c) = fill_color {
                            tmp.fill_path_ctm(path, c, ctm, scale, page_height);
                        }
                        if let Some(c) = stroke_color {
                            let ctm_scale = (ctm.a * ctm.a + ctm.b * ctm.b).sqrt().max(0.01);
                            let screen_lw = (line_width * ctm_scale * scale)
                                .max(0.5)
                                .min(self.width.max(self.height) as f64);
                            tmp.stroke_path_ctm(path, c, screen_lw, *line_cap, *line_join,
                                                ctm, scale, page_height);
                        }
                        let mask_alpha = render_smask_alpha(sm, self.width, self.height, scale, page_height, font_cache);
                        composite_with_mask(&mut self.pixels, &tmp.pixels, &mask_alpha, self.width, self.height);
                    } else {
                        self.blend_mode = *blend_mode;
                        if let Some(c) = fill_color {
                            self.fill_path_ctm(path, c, ctm, scale, page_height);
                        }
                        if let Some(c) = stroke_color {
                            let ctm_scale = (ctm.a * ctm.a + ctm.b * ctm.b).sqrt().max(0.01);
                            let screen_lw = (line_width * ctm_scale * scale)
                                .max(0.5)
                                .min(self.width.max(self.height) as f64);
                            self.stroke_path_ctm(path, c, screen_lw, *line_cap, *line_join,
                                                 ctm, scale, page_height);
                        }
                        self.blend_mode = BlendMode::Normal;
                    }
                }
                RenderCommand::DrawText { x, y, text, font_size, color, ctm } => {
                    let ctm_scale = (ctm.a * ctm.a + ctm.b * ctm.b).sqrt().max(0.01);
                    self.draw_text_placeholder(*x, *y, text.len(),
                        *font_size * ctm_scale, color, ctm, scale, page_height);
                }
                RenderCommand::DrawGlyph { x, y, codepoints, glyph_ids, font_id,
                                           font_size, color, ctm } => {
                    let ctm_scale = (ctm.a * ctm.a + ctm.b * ctm.b).sqrt().max(0.01);
                    let px_size   = ((*font_size * ctm_scale * scale) as f32).max(4.0);
                    self.draw_glyphs(*x, *y, codepoints, glyph_ids, *font_id, px_size,
                                     color, ctm, scale, page_height, font_cache);
                }
                RenderCommand::DrawImage { x, y, width_pts, height_pts,
                                           pixels, img_w, img_h } => {
                    let identity = Matrix::identity();
                    self.draw_image(*x, *y, *width_pts, *height_pts,
                                    pixels, *img_w, *img_h,
                                    &identity, scale, page_height);
                }
                RenderCommand::SetClipPath { path, even_odd, ctm } => {
                    self.set_clip_path(path, ctm, scale, page_height, *even_odd);
                }
                RenderCommand::SaveGraphicsState => {
                    self.save_clip();
                }
                RenderCommand::RestoreGraphicsState => {
                    self.restore_clip();
                }
                RenderCommand::DrawShading { shading, ctm } => {
                    self.draw_shading(shading, ctm, scale, page_height);
                }
                RenderCommand::DrawTilingPattern {
                    cell_cmds, bbox, x_step, y_step, matrix, ctm, paint_type, uncolored_fill
                } => {
                    self.draw_tiling_pattern(
                        cell_cmds, *bbox, *x_step, *y_step,
                        matrix, ctm, scale, page_height,
                        *paint_type, uncolored_fill.as_ref(), font_cache,
                    );
                }
                RenderCommand::DrawShadingPattern { shading, ctm } => {
                    self.draw_shading(shading, ctm, scale, page_height);
                }
            }
        }
    }

    // ── Fill (CTM-aware) ──────────────────────────────────────────────────────

    fn fill_path_ctm(&mut self, path: &Path, color: &Color,
                     ctm: &Matrix, scale: f64, page_h: f64) {
        let subpaths = collect_subpaths_screen(path, ctm, scale, page_h);
        for poly in subpaths {
            if poly.len() < 3 { continue; }
            self.scanline_fill_screen(&poly, color);
        }
    }

    fn scanline_fill_screen(&mut self, poly: &[(f64, f64)], color: &Color) {
        let n = poly.len();
        let y_min = poly.iter().map(|p| p.1).fold(f64::MAX, f64::min);
        let y_max = poly.iter().map(|p| p.1).fold(f64::MIN, f64::max);
        let y0 = y_min.floor() as i32;
        let y1 = y_max.ceil()  as i32;

        for py in y0..=y1 {
            let yf = py as f64 + 0.5;
            let mut xs: Vec<f64> = Vec::new();
            for i in 0..n {
                let (x1, y1p) = poly[i];
                let (x2, y2p) = poly[(i + 1) % n];
                if (y1p <= yf && y2p > yf) || (y2p <= yf && y1p > yf) {
                    let t = (yf - y1p) / (y2p - y1p);
                    xs.push(x1 + t * (x2 - x1));
                }
            }
            xs.sort_by(|a, b| a.partial_cmp(b).unwrap_or(std::cmp::Ordering::Equal));
            let mut k = 0;
            while k + 1 < xs.len() {
                let xa = xs[k].ceil()  as i32;
                let xb = xs[k + 1].floor() as i32;
                for px in xa..=xb {
                    self.put_pixel(px, py, color);
                }
                k += 2;
            }
        }
    }

    // ── Stroke (CTM-aware) ────────────────────────────────────────────────────

    /// Stroke `path` with the given screen-space line width, cap style, and join
    /// style.
    ///
    /// `line_cap` values:
    ///   - `0` Butt   — stroke ends exactly at the endpoint (default)
    ///   - `1` Round  — filled semicircle of radius `screen_lw/2` at each endpoint
    ///   - `2` Square — stroke extended by `screen_lw/2` beyond each endpoint
    ///
    /// `line_join` values:
    ///   - `0` Miter  — outer edges meet at a point (handled naturally by thick-line rendering)
    ///   - `1` Round  — filled circle of radius `screen_lw/2` at each interior vertex
    ///   - `2` Bevel  — triangle fill clips the miter at each interior vertex
    fn stroke_path_ctm(&mut self, path: &Path, color: &Color,
                       screen_lw: f64, line_cap: u8, line_join: u8,
                       ctm: &Matrix, scale: f64, page_h: f64) {
        // Collect the flat list of screen-space points; NaN pairs mark sub-path breaks.
        let pts = flatten_to_segments_screen(path, ctm, scale, page_h);

        // Split into continuous sub-paths so we can handle joins properly.
        let subpaths = split_into_subpaths(&pts);

        for sp in &subpaths {
            if sp.is_empty() { continue; }

            let n = sp.len();

            // Draw each segment.
            for i in 0..n.saturating_sub(1) {
                let (x0, y0) = sp[i];
                let (x1, y1) = sp[i + 1];

                // For square caps: extend the segment endpoints by lw/2 outward.
                let (ex0, ey0, ex1, ey1) = if line_cap == 2 {
                    let dx = x1 - x0;
                    let dy = y1 - y0;
                    let len = dx.hypot(dy).max(1e-9);
                    let ux = dx / len * (screen_lw * 0.5);
                    let uy = dy / len * (screen_lw * 0.5);
                    // Extend only at true endpoints (first and last segment of sub-path).
                    let (lx0, ly0) = if i == 0 { (x0 - ux, y0 - uy) } else { (x0, y0) };
                    let (lx1, ly1) = if i == n - 2 { (x1 + ux, y1 + uy) } else { (x1, y1) };
                    (lx0, ly0, lx1, ly1)
                } else {
                    (x0, y0, x1, y1)
                };

                self.draw_thick_line(ex0, ey0, ex1, ey1, screen_lw, color);

                // Round cap at the segment endpoints (only at true sub-path ends).
                if line_cap == 1 {
                    if i == 0 {
                        self.draw_filled_circle(x0, y0, screen_lw * 0.5, color);
                    }
                    if i == n - 2 {
                        self.draw_filled_circle(x1, y1, screen_lw * 0.5, color);
                    }
                }
            }

            // Apply line joins at interior vertices (between consecutive segments).
            if n >= 3 {
                for i in 1..n.saturating_sub(1) {
                    let (vx, vy) = sp[i];
                    match line_join {
                        1 => {
                            // Round join: fill circle at the vertex.
                            self.draw_filled_circle(vx, vy, screen_lw * 0.5, color);
                        }
                        2 => {
                            // Bevel join: fill a triangle between the two segment ends
                            // and the vertex to cover the gap left by thick-line clipping.
                            let (ax, ay) = sp[i - 1];
                            let (bx, by) = sp[i + 1];
                            // Direction vectors (unit) pointing away from vertex along each arm.
                            let da = ((ax - vx).hypot(ay - vy)).max(1e-9);
                            let db = ((bx - vx).hypot(by - vy)).max(1e-9);
                            let half = screen_lw * 0.5;
                            // Perpendicular offsets at each arm.
                            let n0x = -(ay - vy) / da * half;
                            let n0y =  (ax - vx) / da * half;
                            let n1x =  (by - vy) / db * half;
                            let n1y = -(bx - vx) / db * half;
                            // Triangle vertices: vertex itself + two offset points.
                            let tri = [
                                (vx + n0x, vy + n0y),
                                (vx + n1x, vy + n1y),
                                (vx, vy),
                            ];
                            self.scanline_fill_screen(&tri, color);
                            // Mirror triangle for the other side.
                            let tri2 = [
                                (vx - n0x, vy - n0y),
                                (vx - n1x, vy - n1y),
                                (vx, vy),
                            ];
                            self.scanline_fill_screen(&tri2, color);
                        }
                        _ => {
                            // Miter (0) or unknown: no extra fill needed — thick-line
                            // segments already overlap naturally at shared vertices.
                        }
                    }
                }
            }
        }
    }

    fn draw_thick_line(&mut self, x0: f64, y0: f64, x1: f64, y1: f64,
                       w: f64, color: &Color) {
        if !(x0.is_finite() && y0.is_finite() && x1.is_finite() && y1.is_finite() && w.is_finite()) { return; }
        let dx = x1 - x0; let dy = y1 - y0;
        let len2 = dx * dx + dy * dy;
        if len2 < 1e-9 { self.put_pixel(x0 as i32, y0 as i32, color); return; }
        let half = w * 0.5 + 0.5;
        let minx = (x0.min(x1) - half).floor() as i32;
        let maxx = (x0.max(x1) + half).ceil()  as i32;
        let miny = (y0.min(y1) - half).floor() as i32;
        let maxy = (y0.max(y1) + half).ceil()  as i32;
        for py in miny..=maxy {
            for px in minx..=maxx {
                let t = (((px as f64 - x0) * dx + (py as f64 - y0) * dy) / len2)
                    .clamp(0.0, 1.0);
                let cx = x0 + t * dx; let cy = y0 + t * dy;
                let d  = (px as f64 - cx).hypot(py as f64 - cy);
                if d <= w * 0.5 { self.put_pixel(px, py, color); }
            }
        }
    }

    /// Draw a filled circle at (`cx`, `cy`) with radius `r`.
    /// Used for round line caps and round line joins.
    fn draw_filled_circle(&mut self, cx: f64, cy: f64, r: f64, color: &Color) {
        let x0 = (cx - r).floor() as i32;
        let x1 = (cx + r).ceil()  as i32;
        let y0 = (cy - r).floor() as i32;
        let y1 = (cy + r).ceil()  as i32;
        let r2 = r * r;
        for py in y0..=y1 {
            for px in x0..=x1 {
                let dx = px as f64 + 0.5 - cx;
                let dy = py as f64 + 0.5 - cy;
                if dx * dx + dy * dy <= r2 {
                    self.put_pixel(px, py, color);
                }
            }
        }
    }

    // ── Glyph rendering ───────────────────────────────────────────────────────

    fn draw_glyphs(&mut self, pdf_x: f64, pdf_y: f64, chars: &[char], glyph_ids: &[u16],
                   font_id: u64, px_size: f32,
                   color: &Color, ctm: &Matrix, scale: f64, page_h: f64,
                   font_cache: &FontCache) {
        let (mut sx, sy) = to_screen(pdf_x, pdf_y, ctm, scale, page_h);
        let sy = sy as i32;
        // CID / Type0 path: render by glyph index (GID).
        if !glyph_ids.is_empty() {
            for &gid in glyph_ids {
                if let Some(glyph) = font_cache.get_glyph_index(font_id, gid, px_size) {
                    let glyph = glyph.clone();
                    let gx = sx as i32 + glyph.bearing_x;
                    let gy = sy - glyph.bearing_y;
                    for row in 0..glyph.height {
                        for col in 0..glyph.width {
                            let alpha = glyph.bitmap[row * glyph.width + col] as f32 / 255.0;
                            if alpha > 0.01 {
                                let c = Color { r: color.r, g: color.g, b: color.b, a: color.a * alpha };
                                self.put_pixel(gx + col as i32, gy + row as i32, &c);
                            }
                        }
                    }
                    sx += glyph.advance_x as f64;
                } else {
                    sx += px_size as f64 * 0.6;
                }
            }
            return;
        }
        for &ch in chars {
            if (ch as u32) < 32 { sx += px_size as f64 * 0.6; continue; }
            if let Some(glyph) = font_cache.get_glyph(font_id, ch, px_size) {
                let glyph = glyph.clone();
                let gx = sx as i32 + glyph.bearing_x;
                let gy = sy - glyph.bearing_y;
                for row in 0..glyph.height {
                    for col in 0..glyph.width {
                        let alpha = glyph.bitmap[row * glyph.width + col] as f32 / 255.0;
                        if alpha > 0.01 {
                            let c = Color {
                                r: color.r, g: color.g, b: color.b,
                                a: color.a * alpha,
                            };
                            self.put_pixel(gx + col as i32, gy + row as i32, &c);
                        }
                    }
                }
                sx += glyph.advance_x as f64;
            } else {
                sx += px_size as f64 * 0.6;
            }
        }
    }

    // ── Image rendering ───────────────────────────────────────────────────────

    fn draw_image(&mut self, pdf_x: f64, pdf_y: f64,
                  w_pts: f64, h_pts: f64,
                  pixels: &[u8], img_w: u32, img_h: u32,
                  ctm: &Matrix, scale: f64, page_h: f64) {
        let (sx0, sy0) = to_screen(pdf_x,         pdf_y,         ctm, scale, page_h);
        let (sx1, _sy1) = to_screen(pdf_x + w_pts, pdf_y,         ctm, scale, page_h);
        let (_sx2, sy2) = to_screen(pdf_x,         pdf_y + h_pts, ctm, scale, page_h);
        let dst_x = sx0.min(sx1).min(sx0).floor() as i32;
        let dst_y = sy0.min(sy2).floor() as i32;
        let dst_w = (w_pts * scale) as i32;
        let dst_h = (h_pts * scale) as i32;
        if dst_w <= 0 || dst_h <= 0 || img_w == 0 || img_h == 0 { return; }

        for dy in 0..dst_h {
            for dx in 0..dst_w {
                let sx = (dx as f32 / dst_w as f32 * img_w as f32) as usize;
                let sy = (dy as f32 / dst_h as f32 * img_h as f32) as usize;
                let si = (sy.min(img_h as usize - 1) * img_w as usize
                         + sx.min(img_w as usize - 1)) * 4;
                if si + 3 >= pixels.len() { continue; }
                let c = Color {
                    r: pixels[si]     as f32 / 255.0,
                    g: pixels[si + 1] as f32 / 255.0,
                    b: pixels[si + 2] as f32 / 255.0,
                    a: pixels[si + 3] as f32 / 255.0,
                };
                self.put_pixel(dst_x + dx, dst_y + dy, &c);
            }
        }
    }

    // ── Text placeholder (legacy DrawText) ────────────────────────────────────

    fn draw_text_placeholder(&mut self, x: f64, y: f64, len: usize, size: f64,
                              color: &Color, ctm: &Matrix, scale: f64, page_h: f64) {
        let w = len as f64 * size * 0.6;
        let (sx0, sy) = to_screen(x,           y, ctm, scale, page_h);
        let (sx1, _)  = to_screen(x + w / scale, y, ctm, scale, page_h);
        self.draw_thick_line(sx0, sy, sx1, sy, 1.0, color);
    }

    // ── Pixel ─────────────────────────────────────────────────────────────────

    fn put_pixel(&mut self, x: i32, y: i32, color: &Color) {
        if x < 0 || y < 0 || x >= self.width as i32 || y >= self.height as i32 { return; }
        if self.width == 0 { return; }
        if let Some((cx0, cy0, cx1, cy1)) = self.clip_rect {
            if x < cx0 || y < cy0 || x > cx1 || y > cy1 { return; }
        }
        if let Some(ref mask) = self.clip_mask {
            let idx = (y as u32).checked_mul(mask.width)
                .and_then(|r| r.checked_add(x as u32))
                .map(|i| i as usize);
            match idx {
                Some(i) if i < mask.pixels.len() => { if !mask.pixels[i] { return; } }
                _ => return,
            }
        }
        let idx = match (y as usize)
            .checked_mul(self.width as usize)
            .and_then(|r| r.checked_add(x as usize))
            .and_then(|r| r.checked_mul(4)) {
            Some(i) if i + 3 < self.pixels.len() => i,
            _ => return,
        };

        // Porter-Duff Over with blend mode (ISO 32000-2 §11.3.5)
        // All values non-premultiplied [0,1].
        let sa = color.a.clamp(0.0, 1.0);
        let da = self.pixels[idx + 3] as f32 / 255.0;
        let dr = self.pixels[idx]     as f32 / 255.0;
        let dg = self.pixels[idx + 1] as f32 / 255.0;
        let db = self.pixels[idx + 2] as f32 / 255.0;

        // Blend source colour with destination using current blend mode.
        let bm = self.blend_mode;
        let bs_r = blend_component(color.r, dr, bm);
        let bs_g = blend_component(color.g, dg, bm);
        let bs_b = blend_component(color.b, db, bm);

        // Composite: out_a = sa + da*(1-sa)
        let out_a = (sa + da * (1.0 - sa)).clamp(0.0, 1.0);
        let to_u8 = |v: f32| (v.clamp(0.0, 1.0) * 255.0) as u8;
        if out_a < 1e-6 {
            self.pixels[idx + 3] = 0;
            return;
        }
        // out_c = (Cs*sa + Cd*da*(1-sa)) / out_a
        self.pixels[idx]     = to_u8((bs_r * sa + dr * da * (1.0 - sa)) / out_a);
        self.pixels[idx + 1] = to_u8((bs_g * sa + dg * da * (1.0 - sa)) / out_a);
        self.pixels[idx + 2] = to_u8((bs_b * sa + db * da * (1.0 - sa)) / out_a);
        self.pixels[idx + 3] = to_u8(out_a);
    }

    // ── Shading (gradient) rasteriser ────────────────────────────────────────

    fn draw_shading(&mut self, shading: &ShadingKind, ctm: &Matrix, scale: f64, page_h: f64) {
        // Restrict pixel iteration to clip_rect AABB if set — avoids painting
        // the entire canvas when the shading is clipped to a small region.
        let (px_min, py_min, px_max, py_max) = if let Some((cx0,cy0,cx1,cy1)) = self.clip_rect {
            (cx0.max(0), cy0.max(0), cx1.min(self.width as i32), cy1.min(self.height as i32))
        } else {
            (0, 0, self.width as i32, self.height as i32)
        };

        match shading {
            ShadingKind::Axial { x0, y0, x1, y1, extend0, extend1, .. } => {
                // Convert gradient axis endpoints to screen space
                let (sx0, sy0) = to_screen(*x0, *y0, ctm, scale, page_h);
                let (sx1, sy1) = to_screen(*x1, *y1, ctm, scale, page_h);
                let dx = sx1 - sx0;
                let dy = sy1 - sy0;
                let len2 = dx * dx + dy * dy;
                if len2 < 1e-10 { return; }
                // For each pixel in clip region, project onto gradient axis → t
                for py in py_min..py_max {
                    for px in px_min..px_max {
                        let fx = px as f64 + 0.5 - sx0;
                        let fy = py as f64 + 0.5 - sy0;
                        let t  = (fx * dx + fy * dy) / len2;
                        let tc = if t < 0.0 {
                            if !extend0 { continue; }
                            0.0f32
                        } else if t > 1.0 {
                            if !extend1 { continue; }
                            1.0f32
                        } else {
                            t as f32
                        };
                        let color = shading.sample(tc);
                        self.put_pixel(px, py, &color);
                    }
                }
            }
            ShadingKind::Mesh { triangles, .. } => {
                for tri in triangles {
                    self.rasterize_triangle(tri, ctm, scale, page_h);
                }
                return;
            }
            ShadingKind::Radial { x0, y0, r0, x1, y1, r1, extend0, extend1, .. } => {
                let (sx0, sy0) = to_screen(*x0, *y0, ctm, scale, page_h);
                let (sx1, sy1) = to_screen(*x1, *y1, ctm, scale, page_h);
                let sr0 = r0 * scale;
                let sr1 = r1 * scale;
                // For each pixel in clip region, solve for t.
                for py in py_min..py_max {
                    for px in px_min..px_max {
                        let fx = px as f64 + 0.5;
                        let fy = py as f64 + 0.5;
                        // Solve for t: |P - C(t)|² = R(t)²  where C(t) = C0 + t*(C1-C0), R(t)=r0+t*(r1-r0)
                        let dx_c = sx1 - sx0;
                        let dy_c = sy1 - sy0;
                        let dr   = sr1 - sr0;
                        let dx_p = fx  - sx0;
                        let dy_p = fy  - sy0;
                        // Quadratic: (dx_c²+dy_c²-dr²)t² - 2(dx_p*dx_c+dy_p*dy_c+sr0*dr)t + (dx_p²+dy_p²-sr0²) = 0
                        let a_q = dx_c*dx_c + dy_c*dy_c - dr*dr;
                        let b_q = -2.0 * (dx_p*dx_c + dy_p*dy_c + sr0*dr);
                        let c_q = dx_p*dx_p + dy_p*dy_p - sr0*sr0;
                        let t = if a_q.abs() < 1e-10 {
                            // Linear equation
                            if b_q.abs() < 1e-10 { continue; }
                            -c_q / b_q
                        } else {
                            let disc = b_q*b_q - 4.0*a_q*c_q;
                            if disc < 0.0 { continue; }
                            // Choose larger t (outermost valid circle)
                            let t1 = (-b_q + disc.sqrt()) / (2.0 * a_q);
                            let t2 = (-b_q - disc.sqrt()) / (2.0 * a_q);
                            t1.max(t2)
                        };
                        let tc = if t < 0.0 {
                            if !extend0 { continue; }
                            0.0f32
                        } else if t > 1.0 {
                            if !extend1 { continue; }
                            1.0f32
                        } else {
                            t as f32
                        };
                        let color = shading.sample(tc);
                        self.put_pixel(px, py, &color);
                    }
                }
            }
        }
    }

    // ── Tiling pattern renderer ───────────────────────────────────────────────

    pub fn draw_tiling_pattern(
        &mut self,
        cell_cmds:      &[RenderCommand],
        bbox:           (f64,f64,f64,f64),
        x_step:         f64,
        y_step:         f64,
        pattern_matrix: &Matrix,
        ctm:            &Matrix,
        scale:          f64,
        _page_h:        f64,
        _paint_type:    u8,
        uncolored_fill: Option<&Color>,
        fc:             &FontCache,
    ) {
        use crate::pattern::tile_origins;

        if x_step.abs() < 1e-9 || y_step.abs() < 1e-9 { return; }

        // Combined transform: pattern space → device space
        let combined = pattern_matrix.multiply(ctm);

        // Canvas bounding box in user space
        let canvas_w = self.width  as f64 / scale;
        let canvas_h = self.height as f64 / scale;
        let clip_bbox = (0.0, 0.0, canvas_w, canvas_h);

        let origins = tile_origins(bbox, x_step, y_step, clip_bbox, &combined);

        let cell_w = (bbox.2 - bbox.0).abs().max(1.0);
        let cell_h = (bbox.3 - bbox.1).abs().max(1.0);
        let cell_px_w = (cell_w * combined.a.abs() * scale).round() as u32;
        let cell_px_h = (cell_h * combined.d.abs() * scale).round() as u32;
        if cell_px_w == 0 || cell_px_h == 0 { return; }
        let cell_px_w = cell_px_w.min(4096);
        let cell_px_h = cell_px_h.min(4096);

        for (tx, ty) in origins {
            // Build tile CTM: translate origin in pattern space, then combined transform
            let tile_ctm = Matrix {
                a: combined.a, b: combined.b,
                c: combined.c, d: combined.d,
                e: combined.a * (bbox.0 + tx) + combined.c * (bbox.1 + ty) + combined.e,
                f: combined.b * (bbox.0 + tx) + combined.d * (bbox.1 + ty) + combined.f,
            };

            // Render cell to offscreen canvas using tile CTM
            let tile_page_h = cell_h;
            let tile_scale  = cell_px_h as f64 / cell_h;

            let mut tile = Canvas::new_transparent(cell_px_w, cell_px_h);
            // Render uses the shared font cache
            tile.render(cell_cmds, tile_page_h, tile_scale, fc);

            // If uncolored pattern (paint_type=2), tint the tile
            if let Some(tint) = uncolored_fill {
                for i in (0..tile.pixels.len()).step_by(4) {
                    if i + 3 < tile.pixels.len() && tile.pixels[i + 3] > 0 {
                        tile.pixels[i]     = (tile.pixels[i]     as f32 * tint.r / 255.0 * 255.0) as u8;
                        tile.pixels[i + 1] = (tile.pixels[i + 1] as f32 * tint.g / 255.0 * 255.0) as u8;
                        tile.pixels[i + 2] = (tile.pixels[i + 2] as f32 * tint.b / 255.0 * 255.0) as u8;
                    }
                }
            }

            // Blit tile at position (tx, ty) in device space
            let (dst_x0, dst_y0) = to_screen(bbox.0 + tx, bbox.1 + ty, &tile_ctm, 1.0, tile_page_h);
            let dst_x = dst_x0.round() as i32;
            let dst_y = dst_y0.round() as i32;

            for row in 0..cell_px_h as i32 {
                for col in 0..cell_px_w as i32 {
                    let si = (row as usize * cell_px_w as usize + col as usize) * 4;
                    if si + 3 >= tile.pixels.len() { continue; }
                    let alpha = tile.pixels[si + 3];
                    if alpha == 0 { continue; }
                    let c = Color {
                        r: tile.pixels[si]     as f32 / 255.0,
                        g: tile.pixels[si + 1] as f32 / 255.0,
                        b: tile.pixels[si + 2] as f32 / 255.0,
                        a: alpha as f32 / 255.0,
                    };
                    self.put_pixel(dst_x + col, dst_y + row, &c);
                }
            }
        }
    }

    // ── Mesh triangle rasterizer ──────────────────────────────────────────────

    fn rasterize_triangle(
        &mut self,
        tri:    &crate::shading::ShadingTriangle,
        ctm:    &Matrix,
        scale:  f64,
        page_h: f64,
    ) {
        let (sx0, sy0) = to_screen(tri.p0.0, tri.p0.1, ctm, scale, page_h);
        let (sx1, sy1) = to_screen(tri.p1.0, tri.p1.1, ctm, scale, page_h);
        let (sx2, sy2) = to_screen(tri.p2.0, tri.p2.1, ctm, scale, page_h);

        let xmin = sx0.min(sx1).min(sx2).floor() as i32;
        let xmax = sx0.max(sx1).max(sx2).ceil()  as i32;
        let ymin = sy0.min(sy1).min(sy2).floor() as i32;
        let ymax = sy0.max(sy1).max(sy2).ceil()  as i32;

        // Denominator for barycentric coords
        let denom = (sy1 - sy2) * (sx0 - sx2) + (sx2 - sx1) * (sy0 - sy2);
        if denom.abs() < 1e-9 { return; }
        let inv_d = 1.0 / denom;

        for py in ymin..=ymax {
            for px in xmin..=xmax {
                let fx = px as f64 + 0.5;
                let fy = py as f64 + 0.5;
                let u = ((sy1 - sy2) * (fx - sx2) + (sx2 - sx1) * (fy - sy2)) * inv_d;
                let v = ((sy2 - sy0) * (fx - sx2) + (sx0 - sx2) * (fy - sy2)) * inv_d;
                let w = 1.0 - u - v;
                if u < -1e-6 || v < -1e-6 || w < -1e-6 { continue; }
                let uf = u as f32; let vf = v as f32; let wf = w as f32;
                let r = uf*tri.c0[0] + vf*tri.c1[0] + wf*tri.c2[0];
                let g = uf*tri.c0[1] + vf*tri.c1[1] + wf*tri.c2[1];
                let b = uf*tri.c0[2] + vf*tri.c1[2] + wf*tri.c2[2];
                let a = uf*tri.c0[3] + vf*tri.c1[3] + wf*tri.c2[3];
                self.put_pixel(px, py, &Color { r, g, b, a: a.clamp(0.0, 1.0) });
            }
        }
    }
}

// ── Coordinate transform ──────────────────────────────────────────────────────

// ── SMask helpers ─────────────────────────────────────────────────────────────

/// Run a sub-interpreter on the SMask form content and return per-pixel mask alpha [0,1].
/// Uses an empty PageResources (no fonts/xobjects) which handles path-based masks correctly.
/// Width/height are the OUTPUT canvas dimensions so the mask is already at the right scale.
fn render_smask_alpha(
    smask:        &SmaskDef,
    width:        u32,
    height:       u32,
    _scale:       f64,
    _page_height: f64,
    fc:           &FontCache,
) -> Vec<f32> {
    let n = (width * height) as usize;
    if smask.form_content.is_empty() || smask.form_width_pt < 1.0 || smask.form_height_pt < 1.0 {
        return vec![1.0f32; n]; // empty form → fully opaque mask
    }
    use crate::interpreter::Interpreter;

    let form_scale = (width as f64 / smask.form_width_pt)
        .min(height as f64 / smask.form_height_pt);
    let form_ph    = smask.form_height_pt;

    let mut interp = Interpreter::new(smask.form_width_pt, smask.form_height_pt);
    // No resources for mask form (simple gradient/path masks don't need fonts/XObjects)
    let cmds = interp.interpret(&smask.form_content).to_vec();

    let mut mask_canvas = Canvas::new_transparent(width, height);
    mask_canvas.render(&cmds, form_ph, form_scale, fc);

    compute_mask_alpha(&mask_canvas.pixels, width, height, smask.smask_type)
}

/// Extract per-pixel mask value (Alpha or Luminosity) from a rendered canvas.
fn compute_mask_alpha(px: &[u8], width: u32, height: u32, mode: SmaskType) -> Vec<f32> {
    let n = (width * height) as usize;
    let mut alpha = Vec::with_capacity(n);
    for i in 0..n {
        let base = i * 4;
        let v = match mode {
            SmaskType::Alpha => px.get(base + 3).copied().unwrap_or(0) as f32 / 255.0,
            SmaskType::Luminosity => {
                let r = px.get(base    ).copied().unwrap_or(0) as f32 / 255.0;
                let g = px.get(base + 1).copied().unwrap_or(0) as f32 / 255.0;
                let b = px.get(base + 2).copied().unwrap_or(0) as f32 / 255.0;
                0.2126 * r + 0.7152 * g + 0.0722 * b
            }
        };
        alpha.push(v);
    }
    alpha
}

/// Composite `src` canvas onto `dst` canvas using per-pixel `mask_alpha` (Porter-Duff Over).
/// dst and src are RGBA8 pixel buffers; mask_alpha has one f32 per pixel.
fn composite_with_mask(dst: &mut [u8], src: &[u8], mask: &[f32], width: u32, height: u32) {
    let n = (width * height) as usize;
    for i in 0..n {
        let ma = mask.get(i).copied().unwrap_or(1.0).clamp(0.0, 1.0);
        if ma < 1e-6 { continue; } // fully transparent — skip
        let si = i * 4;
        let sa = (src.get(si + 3).copied().unwrap_or(0) as f32 / 255.0) * ma;
        if sa < 1e-6 { continue; }
        let da = dst[si + 3] as f32 / 255.0;
        let out_a = (sa + da * (1.0 - sa)).clamp(0.0, 1.0);
        if out_a < 1e-6 { continue; }
        for c in 0..3 {
            let sc = src.get(si + c).copied().unwrap_or(0) as f32 / 255.0;
            let dc = dst[si + c] as f32 / 255.0;
            dst[si + c] = ((sc * sa + dc * da * (1.0 - sa)) / out_a * 255.0).clamp(0.0, 255.0) as u8;
        }
        dst[si + 3] = (out_a * 255.0) as u8;
    }
}

// ── Blend mode per-channel formula (ISO 32000-2 §11.3.5) ─────────────────────
// All inputs and output are non-premultiplied, clamped [0,1].
#[inline]
fn blend_component(cs: f32, cb: f32, mode: BlendMode) -> f32 {
    let cs = cs.clamp(0.0, 1.0);
    let cb = cb.clamp(0.0, 1.0);
    match mode {
        BlendMode::Normal     => cs,
        BlendMode::Multiply   => cs * cb,
        BlendMode::Screen     => cs + cb - cs * cb,
        BlendMode::Overlay    => if cb <= 0.5 { 2.0*cs*cb } else { 1.0 - 2.0*(1.0-cs)*(1.0-cb) },
        BlendMode::Darken     => cs.min(cb),
        BlendMode::Lighten    => cs.max(cb),
        BlendMode::ColorDodge => if cs >= 1.0 { 1.0 } else { (cb / (1.0 - cs)).min(1.0) },
        BlendMode::ColorBurn  => if cs <= 0.0 { 0.0 } else { 1.0 - ((1.0 - cb) / cs).min(1.0) },
        BlendMode::HardLight  => if cs <= 0.5 { 2.0*cs*cb } else { 1.0 - 2.0*(1.0-cs)*(1.0-cb) },
        BlendMode::SoftLight  => {
            // PDF spec formula (exact)
            if cs <= 0.5 {
                cb - (1.0 - 2.0*cs) * cb * (1.0 - cb)
            } else {
                let d = if cb <= 0.25 { ((16.0*cb - 12.0)*cb + 4.0)*cb } else { cb.sqrt() };
                cb + (2.0*cs - 1.0) * (d - cb)
            }
        }
        BlendMode::Difference => (cb - cs).abs(),
        BlendMode::Exclusion  => cs + cb - 2.0*cs*cb,
    }
}

/// Convert user-space (PDF) coordinate to screen-pixel coordinate.
/// Applies the CTM (user → page space), then the render scale, then a Y-flip
/// so that PDF's bottom-left origin maps to the top-left of the pixel buffer.
#[inline]
fn to_screen(x: f64, y: f64, ctm: &Matrix, scale: f64, page_h: f64) -> (f64, f64) {
    let (px, py) = ctm.transform_point(x, y);
    (px * scale, (page_h - py) * scale)
}

// ── Path → screen-space polygon helpers ──────────────────────────────────────

/// Flatten `path` into a list of closed screen-space polylines suitable for
/// scanline fill.  Each subpath becomes one `Vec<(f64,f64)>`.
fn collect_subpaths_screen(path: &Path, ctm: &Matrix, scale: f64, page_h: f64)
    -> Vec<Vec<(f64, f64)>> {
    let mut result = Vec::new();
    let mut cur: Vec<(f64, f64)> = Vec::new();
    let mut start: Option<(f64, f64)> = None;

    for cmd in path.iter() {
        match cmd {
            PathCmd::MoveTo(x, y) => {
                if cur.len() >= 2 { result.push(cur.clone()); }
                cur.clear();
                let sp = to_screen(*x, *y, ctm, scale, page_h);
                cur.push(sp);
                start = Some(sp);
            }
            PathCmd::LineTo(x, y) => {
                cur.push(to_screen(*x, *y, ctm, scale, page_h));
            }
            PathCmd::CurveTo(x1, y1, x2, y2, x3, y3) => {
                if let Some(&last) = cur.last() {
                    let p1 = to_screen(*x1, *y1, ctm, scale, page_h);
                    let p2 = to_screen(*x2, *y2, ctm, scale, page_h);
                    let p3 = to_screen(*x3, *y3, ctm, scale, page_h);
                    flatten_bezier(last, p1, p2, p3, &mut cur, 8);
                }
            }
            PathCmd::ClosePath => {
                if let Some(s) = start { cur.push(s); }
                if cur.len() >= 3 { result.push(cur.clone()); }
                cur.clear(); start = None;
            }
            PathCmd::Rect(rx, ry, rw, rh) => {
                let p0 = to_screen(*rx,        *ry,        ctm, scale, page_h);
                let p1 = to_screen(*rx + rw,   *ry,        ctm, scale, page_h);
                let p2 = to_screen(*rx + rw,   *ry + rh,   ctm, scale, page_h);
                let p3 = to_screen(*rx,        *ry + rh,   ctm, scale, page_h);
                result.push(vec![p0, p1, p2, p3]);
            }
        }
    }
    if cur.len() >= 3 { result.push(cur); }
    result
}

/// Flatten `path` into a flat sequence of screen-space points for stroking.
/// Sub-path boundaries are indicated by `(NAN, NAN)` sentinel values.
fn flatten_to_segments_screen(path: &Path, ctm: &Matrix, scale: f64, page_h: f64)
    -> Vec<(f64, f64)> {
    let mut pts: Vec<(f64, f64)> = Vec::new();
    let mut cur_pt: Option<(f64, f64)> = None;
    let nan = (f64::NAN, f64::NAN);

    for cmd in path.iter() {
        match cmd {
            PathCmd::MoveTo(x, y) => {
                pts.push(nan);
                let sp = to_screen(*x, *y, ctm, scale, page_h);
                pts.push(sp);
                cur_pt = Some(sp);
            }
            PathCmd::LineTo(x, y) => {
                let sp = to_screen(*x, *y, ctm, scale, page_h);
                pts.push(sp);
                cur_pt = Some(sp);
            }
            PathCmd::CurveTo(x1, y1, x2, y2, x3, y3) => {
                if let Some(p0) = cur_pt {
                    let p1 = to_screen(*x1, *y1, ctm, scale, page_h);
                    let p2 = to_screen(*x2, *y2, ctm, scale, page_h);
                    let p3 = to_screen(*x3, *y3, ctm, scale, page_h);
                    let mut seg = Vec::new();
                    flatten_bezier(p0, p1, p2, p3, &mut seg, 8);
                    pts.extend_from_slice(&seg);
                    cur_pt = Some(p3);
                }
            }
            PathCmd::ClosePath => {}
            PathCmd::Rect(rx, ry, rw, rh) => {
                pts.push(nan);
                let p0 = to_screen(*rx,        *ry,        ctm, scale, page_h);
                let p1 = to_screen(*rx + rw,   *ry,        ctm, scale, page_h);
                let p2 = to_screen(*rx + rw,   *ry + rh,   ctm, scale, page_h);
                let p3 = to_screen(*rx,        *ry + rh,   ctm, scale, page_h);
                pts.extend_from_slice(&[p0, p1, p2, p3, p0]);
                cur_pt = Some(p0);
            }
        }
    }
    pts
}

/// Helper: flatten `path` into screen-space open polylines (one per subpath).
/// Used by `ClipMask::apply_path`.  Unlike `collect_subpaths_screen` this
/// function does NOT close subpaths — the caller handles the closing edge.
fn collect_polylines(path: &Path, ctm: &Matrix, scale: f64, page_h: f64)
    -> Vec<Vec<(f64, f64)>> {
    let mut result: Vec<Vec<(f64, f64)>> = Vec::new();
    let mut cur: Vec<(f64, f64)> = Vec::new();

    for cmd in path.iter() {
        match cmd {
            PathCmd::MoveTo(x, y) => {
                if !cur.is_empty() { result.push(cur.clone()); }
                cur.clear();
                cur.push(to_screen(*x, *y, ctm, scale, page_h));
            }
            PathCmd::LineTo(x, y) => {
                cur.push(to_screen(*x, *y, ctm, scale, page_h));
            }
            PathCmd::CurveTo(x1, y1, x2, y2, x3, y3) => {
                if let Some(&last) = cur.last() {
                    let p1 = to_screen(*x1, *y1, ctm, scale, page_h);
                    let p2 = to_screen(*x2, *y2, ctm, scale, page_h);
                    let p3 = to_screen(*x3, *y3, ctm, scale, page_h);
                    flatten_bezier(last, p1, p2, p3, &mut cur, 8);
                }
            }
            PathCmd::ClosePath => {
                // Close the polyline by repeating the first point so edge
                // detection in apply_path sees the closing segment.
                if cur.len() >= 2 { let first = cur[0]; cur.push(first); }
                result.push(cur.clone()); cur.clear();
            }
            PathCmd::Rect(rx, ry, rw, rh) => {
                // Expand inline rect as a closed loop.
                if !cur.is_empty() { result.push(cur.clone()); cur.clear(); }
                let p0 = to_screen(*rx,        *ry,        ctm, scale, page_h);
                let p1 = to_screen(*rx + rw,   *ry,        ctm, scale, page_h);
                let p2 = to_screen(*rx + rw,   *ry + rh,   ctm, scale, page_h);
                let p3 = to_screen(*rx,        *ry + rh,   ctm, scale, page_h);
                result.push(vec![p0, p1, p2, p3, p0]);
            }
        }
    }
    if !cur.is_empty() { result.push(cur); }
    result
}

/// Split a flat point list (with NaN sentinels from `flatten_to_segments_screen`)
/// into individual sub-path point vectors, filtering out empty sub-paths.
fn split_into_subpaths(pts: &[(f64, f64)]) -> Vec<Vec<(f64, f64)>> {
    let mut result: Vec<Vec<(f64, f64)>> = Vec::new();
    let mut cur: Vec<(f64, f64)> = Vec::new();
    for &(x, y) in pts {
        if x.is_nan() || y.is_nan() {
            if cur.len() >= 2 { result.push(cur.clone()); }
            cur.clear();
        } else {
            cur.push((x, y));
        }
    }
    if cur.len() >= 2 { result.push(cur); }
    result
}

// ── Bezier flattening (screen space) ─────────────────────────────────────────

/// Recursively subdivide a cubic Bezier curve (de Casteljau) and append the
/// resulting line-segment endpoints to `out`.  `depth` limits recursion.
/// Early exit when the curve is within 0.2 px of a straight line.
fn flatten_bezier(p0: (f64, f64), p1: (f64, f64), p2: (f64, f64), p3: (f64, f64),
                  out: &mut Vec<(f64, f64)>, depth: u32) {
    if depth == 0 { out.push(p3); return; }
    let mid_x  = (p0.0 + p3.0) * 0.5;
    let mid_y  = (p0.1 + p3.1) * 0.5;
    let ctrl_x = (p1.0 + p2.0) * 0.5;
    let ctrl_y = (p1.1 + p2.1) * 0.5;
    let dx = ctrl_x - mid_x; let dy = ctrl_y - mid_y;
    if dx * dx + dy * dy < 0.04 { out.push(p3); return; } // 0.2 px threshold
    let m01  = ((p0.0 + p1.0) * 0.5, (p0.1 + p1.1) * 0.5);
    let m12  = ((p1.0 + p2.0) * 0.5, (p1.1 + p2.1) * 0.5);
    let m23  = ((p2.0 + p3.0) * 0.5, (p2.1 + p3.1) * 0.5);
    let m012 = ((m01.0 + m12.0) * 0.5, (m01.1 + m12.1) * 0.5);
    let m123 = ((m12.0 + m23.0) * 0.5, (m12.1 + m23.1) * 0.5);
    let mid  = ((m012.0 + m123.0) * 0.5, (m012.1 + m123.1) * 0.5);
    flatten_bezier(p0,  m01,  m012, mid,  out, depth - 1);
    flatten_bezier(mid, m123, m23,  p3,   out, depth - 1);
}
