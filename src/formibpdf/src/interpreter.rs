use std::collections::HashMap;
use crate::token::{Token, Tokenizer};
use crate::graphics_state::{BlendMode, SmaskDef, StateStack, Color, Matrix};
use crate::path::Path;
use crate::pdf_object::PdfObject;
use crate::resource::{PageResources, PatternEntry, XObjectKind};
use crate::shading::ShadingKind;

#[derive(Debug, Clone)]
pub enum RenderCommand {
    PaintPath {
        path:         Path,
        fill_color:   Option<Color>,
        stroke_color: Option<Color>,
        line_width:   f64,
        line_cap:     u8,
        line_join:    u8,
        even_odd:     bool,
        /// CTM at the time this path was painted — used by rasterizer to map
        /// user-space coordinates to screen pixels correctly.
        ctm:          Matrix,
        /// Blend mode at paint time (from gs /BM). Rasterizer uses this for compositing.
        blend_mode:   BlendMode,
        /// Active soft mask at paint time (from gs /SMask). None = fully opaque.
        smask:        Option<SmaskDef>,
    },
    DrawText {
        x: f64, y: f64, text: Vec<u8>, font_size: f64, color: Color,
        ctm: Matrix,
    },
    DrawGlyph {
        x:          f64,
        y:          f64,
        codepoints: Vec<char>,
        font_id:    u64,
        font_size:  f64,
        color:      Color,
        ctm:        Matrix,
        glyph_ids:  Vec<u16>, // new: may contain CID/gid indices
    },
    DrawImage {
        x: f64, y: f64,
        width_pts: f64, height_pts: f64,
        pixels: Vec<u8>,
        img_w: u32, img_h: u32,
    },
    /// Clip subsequent drawing to the given path (W / W* operators).
    SetClipPath {
        path:      Path,
        even_odd:  bool,
        ctm:       Matrix,
    },
    /// Push current graphics state (q operator) — rasterizer saves clip stack.
    SaveGraphicsState,
    /// Pop graphics state (Q operator) — rasterizer restores clip stack.
    RestoreGraphicsState,
    /// Paint a shading (gradient) — `sh` operator.
    DrawShading {
        shading: ShadingKind,
        ctm:     Matrix,
    },
    /// Paint a tiling pattern (PDF Type 1 Pattern) as fill or stroke.
    DrawTilingPattern {
        cell_cmds:      Vec<RenderCommand>,
        bbox:           (f64, f64, f64, f64),
        x_step:         f64,
        y_step:         f64,
        matrix:         Matrix,   // pattern space → user space
        ctm:            Matrix,   // CTM at paint time
        paint_type:     u8,       // 1=colored, 2=uncolored
        uncolored_fill: Option<Color>,
    },
    /// Paint a shading pattern (PDF Type 2 Pattern).
    DrawShadingPattern {
        shading: ShadingKind,
        ctm:     Matrix,
    },
}

/// Active colorspace for stroke or fill operations.
#[derive(Clone, Debug, PartialEq)]
pub enum ColorspaceKind {
    DeviceRGB, DeviceGray, DeviceCMYK,
    Lab,        // CIE L*a*b*
    Separation, // Spot color — approximated
    Pattern,    // PDF /Pattern colorspace — handled natively (tiling + shading)
    /// ICC-based colorspace with pre-resolved profile bytes and channel count.
    ICCBased { n_channels: u8, profile: Vec<u8> },
    Unknown,    // Flags has_complex_colorspace for genuinely unsupported colorspaces
}

/// A single piece of extracted text positioned in page space.
#[derive(Debug, Clone)]
pub struct TextItem {
    pub x:         f64,
    pub y:         f64,
    pub text:      String,
    pub font_size: f64,
}

pub struct Interpreter {
    state:                StateStack,
    current_path:         Path,
    commands:             Vec<RenderCommand>,
    operand_stack:        Vec<Token>,
    page_width:           f64,
    page_height:          f64,
    in_text:              bool,
    text_x:               f64,
    text_y:               f64,
    /// Text line matrix origin (Tlm) — set by Td/TD/T*/Tm, NOT advanced by glyphs.
    text_line_x:          f64,
    text_line_y:          f64,
    /// Full text matrix from Tm (preserves rotation/scale, not just translation).
    text_matrix:          Matrix,
    fill_color:           Color,
    stroke_color:         Color,
    /// Current stroke colorspace (affects SCN/scn Lab decoding).
    stroke_cs:            ColorspaceKind,
    /// Current fill colorspace.
    fill_cs:              ColorspaceKind,
    font_size:            f64,
    font_name:            String,
    leading:              f64,
    even_odd:             bool,
    resources:            Option<PageResources>,
    current_font_id:      u64,
    /// ToUnicode map for the current font (set by Tf).
    current_to_unicode:   Option<HashMap<u32, char>>,
    /// advance_width / font_size ratio for current font (from fontdue metrics).
    current_advance_ratio: f32,
    // CID CID CID state
    current_cid:          bool,
    // ── Inline image state ───────────────────────────────────────────────────
    in_inline_image:      bool,
    inline_image_dict:    HashMap<String, PdfObject>,
    // ── Fallback flags (has_clipping removed — now emits SetClipPath) ────────
    pub has_unsupported_image:  bool,
    pub has_complex_colorspace: bool,
    pub has_missing_font:       bool,
    /// Extracted text items accumulated during interpretation.
    pub text_items: Vec<TextItem>,
    /// Active fill pattern name (from /Pattern CS + scn).
    current_fill_pattern:   Option<String>,
    /// Active stroke pattern name (from /Pattern CS + SCN).
    current_stroke_pattern: Option<String>,
    /// Color components for uncolored fill pattern (paint_type=2).
    current_fill_pattern_color: Option<Color>,
}

impl Interpreter {
    pub fn new(page_width: f64, page_height: f64) -> Self {
        Self {
            state:                StateStack::new(),
            current_path:         Path::new(),
            commands:             Vec::new(),
            operand_stack:        Vec::new(),
            page_width,
            page_height,
            in_text:              false,
            text_x:               0.0,
            text_y:               0.0,
            text_line_x:          0.0,
            text_line_y:          0.0,
            text_matrix:          Matrix::identity(),
            fill_color:           Color::black(),
            stroke_color:         Color::black(),
            stroke_cs:            ColorspaceKind::DeviceGray,
            fill_cs:              ColorspaceKind::DeviceGray,
            font_size:            12.0,
            font_name:            String::new(),
            leading:              0.0,
            even_odd:             false,
            resources:            None,
            current_font_id:       0,
            current_to_unicode:    None,
            current_advance_ratio: 0.55,
            current_cid:            false,
            in_inline_image:      false,
            inline_image_dict:    HashMap::new(),
            has_unsupported_image:  false,
            has_complex_colorspace: false,
            has_missing_font:       false,
            text_items:             Vec::new(),
            current_fill_pattern:        None,
            current_stroke_pattern:      None,
            current_fill_pattern_color:  None,
        }
    }

    pub fn set_resources(&mut self, res: PageResources) {
        self.resources = Some(res);
    }

    pub fn interpret(&mut self, stream: &[u8]) -> &[RenderCommand] {
        let mut tok = Tokenizer::new(stream);
        while let Some(t) = tok.next_token() {
            match t {
                Token::Operator(op) => {
                    self.dispatch(&op.clone());
                    self.operand_stack.clear();
                }
                other => self.operand_stack.push(other),
            }
        }
        &self.commands
    }

    fn dispatch(&mut self, op: &str) {
        match op {
            // ── Graphics state ────────────────────────────────────────────────
            "q" => {
                self.state.push();
                self.commands.push(RenderCommand::SaveGraphicsState);
            }
            "Q" => {
                self.state.pop();
                self.commands.push(RenderCommand::RestoreGraphicsState);
            }
            "cm" => {
                let f = self.pop_f(); let e = self.pop_f(); let d = self.pop_f();
                let c = self.pop_f(); let b = self.pop_f(); let a = self.pop_f();
                let m = Matrix { a, b, c, d, e, f };
                let new = self.state.current().ctm.multiply(&m);
                self.state.current_mut().ctm = new;
            }
            "w" => { let v = self.pop_f(); self.state.current_mut().line_width = v; }
            "J" => { let v = self.pop_f() as u8; self.state.current_mut().line_cap  = v; }
            "j" => { let v = self.pop_f() as u8; self.state.current_mut().line_join = v; }

            // ── ExtGState (opacity, blend mode) ──────────────────────────────
            "gs" => {
                if let Some(Token::Name(name)) = self.operand_stack.pop() {
                    // Clone to avoid borrow conflict with self.state
                    let ext = self.resources.as_ref()
                        .and_then(|r| r.ext_gstates.get(&name))
                        . cloned();
                    if let Some(e) = ext {
                        let gs = self.state.current_mut();
                        if let Some(ca) = e.fill_alpha   { gs.fill_alpha   = ca; }
                        if let Some(ca) = e.stroke_alpha { gs.stroke_alpha = ca; }
                        if let Some(bm) = e.blend_mode   { gs.blend_mode   = bm; }
                        // /SMask — convert SmaskRawEntry → SmaskDef
                        if let Some(raw_sm) = &e.smask {
                            gs.smask = Some(SmaskDef {
                                smask_type:     raw_sm.smask_type,
                                form_content:   raw_sm.form_bytes.clone(),
                                bc:             raw_sm.bc,
                                form_width_pt:  raw_sm.form_width_pt,
                                form_height_pt: raw_sm.form_height_pt,
                            });
                        } else {
                            gs.smask = None; // /SMask /None clears any active mask
                        }
                    }
                }
            }

            // ── Color ─────────────────────────────────────────────────────────
            "G"  => { let g = self.pop_f(); self.stroke_color = Color::from_gray(g as f32); }
            "g"  => { let g = self.pop_f(); self.fill_color   = Color::from_gray(g as f32); }
            "RG" => {
                let b = self.pop_f(); let g = self.pop_f(); let r = self.pop_f();
                self.stroke_color = Color::from_rgb(r as f32, g as f32, b as f32);
            }
            "rg" => {
                let b = self.pop_f(); let g = self.pop_f(); let r = self.pop_f();
                self.fill_color = Color::from_rgb(r as f32, g as f32, b as f32);
            }
            "K"  => {
                let k = self.pop_f(); let y = self.pop_f();
                let m = self.pop_f(); let c = self.pop_f();
                self.stroke_color = Color::from_cmyk(c as f32, m as f32, y as f32, k as f32);
            }
            "k"  => {
                let k = self.pop_f(); let y = self.pop_f();
                let m = self.pop_f(); let c = self.pop_f();
                self.fill_color = Color::from_cmyk(c as f32, m as f32, y as f32, k as f32);
            }

            // SCN / scn — stroke / fill in current colorspace.
            // For Separation/DeviceN (spot color), approximate from tint operands.
            // For Pattern: capture pattern name and optional uncolored color.
            "SCN" => {
                if self.stroke_cs == ColorspaceKind::Pattern {
                    // Operands: [color_components]* pattern_name
                    let mut color_vals: Vec<f64> = Vec::new();
                    loop {
                        match self.operand_stack.last() {
                            Some(Token::Real(_)) | Some(Token::Integer(_)) => {
                                color_vals.push(self.pop_f());
                            }
                            _ => break,
                        }
                    }
                    if let Some(Token::Name(pat_name)) = self.operand_stack.pop() {
                        self.current_stroke_pattern = Some(pat_name);
                    }
                    // color_vals used for uncolored stroke patterns (rare)
                    self.stroke_color = decode_scn_color(&color_vals, &ColorspaceKind::DeviceRGB)
                        .unwrap_or_else(Color::black);
                } else {
                    let mut values: Vec<f64> = Vec::new();
                    loop {
                        match self.operand_stack.last() {
                            Some(Token::Real(_)) | Some(Token::Integer(_)) => {
                                values.push(self.pop_f());
                            }
                            _ => break,
                        }
                    }
                    if matches!(self.operand_stack.last(), Some(Token::Name(_))) {
                        self.operand_stack.pop();
                    }
                    self.stroke_color = decode_scn_color(&values, &self.stroke_cs)
                        .unwrap_or_else(|| { self.has_complex_colorspace = true; Color::black() });
                }
            }
            "scn" => {
                if self.fill_cs == ColorspaceKind::Pattern {
                    // Operands: [color_components]* pattern_name
                    let mut color_vals: Vec<f64> = Vec::new();
                    loop {
                        match self.operand_stack.last() {
                            Some(Token::Real(_)) | Some(Token::Integer(_)) => {
                                color_vals.push(self.pop_f());
                            }
                            _ => break,
                        }
                    }
                    if let Some(Token::Name(pat_name)) = self.operand_stack.pop() {
                        self.current_fill_pattern = Some(pat_name);
                    }
                    // Store uncolored fill color for paint_type=2 patterns
                    self.current_fill_pattern_color = decode_scn_color(&color_vals, &ColorspaceKind::DeviceRGB);
                    self.fill_color = Color::black();
                } else {
                    let mut values: Vec<f64> = Vec::new();
                    loop {
                        match self.operand_stack.last() {
                            Some(Token::Real(_)) | Some(Token::Integer(_)) => {
                                values.push(self.pop_f());
                            }
                            _ => break,
                        }
                    }
                    if matches!(self.operand_stack.last(), Some(Token::Name(_))) {
                        self.operand_stack.pop();
                    }
                    self.fill_color = decode_scn_color(&values, &self.fill_cs)
                        .unwrap_or_else(|| { self.has_complex_colorspace = true; Color::black() });
                }
            }

            // CS / cs — select colorspace. Track kind for SCN/scn decoding.
            "CS" => {
                if let Some(Token::Name(name)) = self.operand_stack.pop() {
                    self.stroke_cs = self.classify_colorspace(&name);
                    if self.stroke_cs == ColorspaceKind::Unknown {
                        self.has_complex_colorspace = true;
                    }
                    // Pattern colorspace is handled natively — do NOT set fallback flag.
                }
            }
            "cs" => {
                if let Some(Token::Name(name)) = self.operand_stack.pop() {
                    self.fill_cs = self.classify_colorspace(&name);
                    if self.fill_cs == ColorspaceKind::Unknown {
                        self.has_complex_colorspace = true;
                    }
                    // Pattern colorspace is handled natively — do NOT set fallback flag.
                }
            }

            // SC / sc — numeric color operands only, safe to ignore (color already set).
            "SC" | "sc" => {}

            // Clipping: emit SetClipPath for both simple and complex clip paths.
            // Rasterizer handles clip geometry via the command stream.
            "W" => {
                self.commands.push(RenderCommand::SetClipPath {
                    path:     self.current_path.clone(),
                    even_odd: false,
                    ctm:      self.state.current().ctm.clone(),
                });
            }
            "W*" => {
                self.commands.push(RenderCommand::SetClipPath {
                    path:     self.current_path.clone(),
                    even_odd: true,
                    ctm:      self.state.current().ctm.clone(),
                });
            }

            // ── Path construction ─────────────────────────────────────────────
            "m"  => { let y = self.pop_f(); let x = self.pop_f(); self.current_path.move_to(x, y); }
            "l"  => { let y = self.pop_f(); let x = self.pop_f(); self.current_path.line_to(x, y); }
            "c"  => {
                let y3 = self.pop_f(); let x3 = self.pop_f();
                let y2 = self.pop_f(); let x2 = self.pop_f();
                let y1 = self.pop_f(); let x1 = self.pop_f();
                self.current_path.curve_to(x1, y1, x2, y2, x3, y3);
            }
            "v"  => {
                let y3 = self.pop_f(); let x3 = self.pop_f();
                let y2 = self.pop_f(); let x2 = self.pop_f();
                let (cx, cy) = self.current_path.current_point().unwrap_or((0.0, 0.0));
                self.current_path.curve_to(cx, cy, x2, y2, x3, y3);
            }
            "y"  => {
                let y3 = self.pop_f(); let x3 = self.pop_f();
                let y1 = self.pop_f(); let x1 = self.pop_f();
                self.current_path.curve_to(x1, y1, x3, y3, x3, y3);
            }
            "h"  => self.current_path.close(),
            "re" => {
                let h = self.pop_f(); let w = self.pop_f();
                let y = self.pop_f(); let x = self.pop_f();
                self.current_path.rect(x, y, w, h);
            }

            // ── Path paint ────────────────────────────────────────────────────
            "S"   => self.emit(false, true,  false),
            "s"   => { self.current_path.close(); self.emit(false, true,  false); }
            "f"|"F" => self.emit(true,  false, false),
            "f*"  => { self.even_odd = true; self.emit(true, false, false); }
            "B"   => self.emit(true,  true,  false),
            "B*"  => { self.even_odd = true; self.emit(true, true, false); }
            "b"   => { self.current_path.close(); self.emit(true, true, false); }
            "b*"  => { self.even_odd = true; self.current_path.close(); self.emit(true, true, false); }
            "n"   => { self.current_path.clear(); }

            // ── Text ──────────────────────────────────────────────────────────
            "BT" => {
                self.in_text    = true;
                self.text_x     = 0.0;
                self.text_y     = 0.0;
                self.text_line_x = 0.0;
                self.text_line_y = 0.0;
                self.text_matrix = Matrix::identity();
            }
            "ET" => { self.in_text = false; }

            "Tf" => {
                let size = self.pop_f(); self.font_size = size.max(1.0);
                if let Some(Token::Name(name)) = self.operand_stack.pop() {
                    let font_res = self.resources.as_ref()
                        .and_then(|r| r.fonts.get(&name));
                    let font_id = font_res.map(|f| f.font_id).unwrap_or(0);
                    if font_id == 0 {
                        self.has_missing_font = true;
                    }
                    self.current_font_id    = font_id;
                    self.current_to_unicode = font_res
                        .map(|f| f.to_unicode.clone())
                        .filter(|m| !m.is_empty());
                    self.current_advance_ratio = font_res
                        .map(|f| f.advance_ratio)
                        .unwrap_or(0.55);
                    // CID handling: propagate current_cid flag from font resource
                    self.current_cid = font_res.map(|f| f.cid).unwrap_or(false);
                    self.font_name = name;
                }
            }

            "Td" => {
                let y = self.pop_f(); let x = self.pop_f();
                self.text_line_x += x;
                self.text_line_y += y;
                self.text_x = self.text_line_x;
                self.text_y = self.text_line_y;
                self.text_matrix.e = self.text_x;
                self.text_matrix.f = self.text_y;
            }
            "TD" => {
                let y = self.pop_f(); let x = self.pop_f();
                self.leading = -y;
                self.text_line_x += x;
                self.text_line_y += y;
                self.text_x = self.text_line_x;
                self.text_y = self.text_line_y;
                self.text_matrix.e = self.text_x;
                self.text_matrix.f = self.text_y;
            }
            "Tm" => {
                let f = self.pop_f(); let e = self.pop_f();
                let d = self.pop_f(); let c = self.pop_f();
                let b = self.pop_f(); let a = self.pop_f();
                self.text_matrix = Matrix { a, b, c, d, e, f };
                self.text_line_x = e;
                self.text_line_y = f;
                self.text_x = e;
                self.text_y = f;
            }
            "T*" => {
                let lead = if self.leading != 0.0 { self.leading } else { self.font_size };
                self.text_line_y -= lead;
                self.text_x = self.text_line_x;
                self.text_y = self.text_line_y;
                self.text_matrix.e = self.text_x;
                self.text_matrix.f = self.text_y;
            }

            // Text spacing operators
            "Tc" => { let _ = self.pop_f(); }
            "Tw" => { let _ = self.pop_f(); }
            "Tz" => { let _ = self.pop_f(); }
            "TL" => { let v = self.pop_f(); self.leading = v; }
            "Ts" => { let _ = self.pop_f(); }

            // Move-and-show
            "'" => {
                let lead = if self.leading != 0.0 { self.leading } else { self.font_size };
                self.text_line_y -= lead;
                self.text_x = self.text_line_x;
                self.text_y = self.text_line_y;
                self.text_matrix.e = self.text_x;
                self.text_matrix.f = self.text_y;
                let bytes = self.pop_bytes();
                self.emit_text_bytes(bytes);
            }
            "\"" => {
                let _tw = self.pop_f(); let _tc = self.pop_f();
                let lead = if self.leading != 0.0 { self.leading } else { self.font_size };
                self.text_line_y -= lead;
                self.text_x = self.text_line_x;
                self.text_y = self.text_line_y;
                self.text_matrix.e = self.text_x;
                self.text_matrix.f = self.text_y;
                let bytes = self.pop_bytes();
                self.emit_text_bytes(bytes);
            }

            "Tj" => {
                let bytes = self.pop_bytes();
                self.emit_text_bytes(bytes);
            }

            "TJ" => {
                let mut arr: Vec<Token> = Vec::new();
                while let Some(t) = self.operand_stack.pop() {
                    match t { Token::ArrayEnd => break, x => arr.push(x) }
                }
                arr.reverse();
                for t in arr {
                    match t {
                        Token::StringLit(bs) => {
                            self.emit_text_bytes(bs);
                        }
                        Token::Integer(i) => {
                            self.text_x -= i as f64 * (self.font_size / 1000.0);
                        }
                        Token::Real(r) => {
                            self.text_x -= r * (self.font_size / 1000.0);
                        }
                        _ => {}
                    }
                }
            }

            // ── XObject ───────────────────────────────────────────────────────
            "Do" => {
                let name = match self.operand_stack.pop() {
                    Some(Token::Name(n)) => n,
                    _ => return,
                };
                if let Some(res) = &self.resources {
                    match res.xobjects.get(&name).cloned() {
                        Some(XObjectKind::Image { dict, data }) => {
                            match crate::image_decoder::decode_image(&dict, &data) {
                                Some(img) => {
                                    let ctm = &self.state.current().ctm;
                                    let width_pts  = (ctm.a * ctm.a + ctm.b * ctm.b).sqrt();
                                    let height_pts = (ctm.c * ctm.c + ctm.d * ctm.d).sqrt();
                                    self.commands.push(RenderCommand::DrawImage {
                                        x: ctm.e, y: ctm.f,
                                        width_pts:  width_pts.max(0.1),
                                        height_pts: height_pts.max(0.1),
                                        pixels: img.pixels,
                                        img_w: img.width,
                                        img_h: img.height,
                                    });
                                }
                                None => {
                                    self.has_unsupported_image = true;
                                }
                            }
                        }
                        Some(XObjectKind::Form { content, .. }) => {
                            self.state.push();
                            let mut sub = Interpreter::new(self.page_width, self.page_height);
                            sub.fill_color         = self.fill_color.clone();
                            sub.stroke_color       = self.stroke_color.clone();
                            sub.font_size          = self.font_size;
                            sub.current_font_id    = self.current_font_id;
                            sub.current_to_unicode = self.current_to_unicode.clone();
                            sub.resources          = self.resources.clone();
                            let sub_cmds = sub.interpret(&content).to_vec();
                            self.commands.extend(sub_cmds);
                            // Propagate unsupported-feature flags and text items.
                            self.has_unsupported_image  |= sub.has_unsupported_image;
                            self.has_complex_colorspace |= sub.has_complex_colorspace;
                            self.has_missing_font       |= sub.has_missing_font;
                            self.text_items.extend(sub.text_items);
                            self.state.pop();
                        }
                        _ => {}
                    }
                }
            }

            // ── Inline images ─────────────────────────────────────────────────
            // Sequence: BI [key/value pairs] ID <InlineImageData token> EI
            // The tokenizer scans raw bytes after ID and stashes them as
            // Token::InlineImageData, so by the time we see "EI" the data is
            // already sitting at the top of the operand stack.

            "BI" => {
                self.in_inline_image = true;
                self.inline_image_dict.clear();
            }

            // After ID the tokenizer pushes InlineImageData onto the stream.
            // We collect all key/value Name pairs from the operand stack now.
            "ID" => {
                // Parse key-value pairs: the operand stack has alternating
                // Name(key), <value> tokens pushed before BI was dispatched.
                // Operand stack is cleared after every operator, so BI cleared
                // it, then key/value tokens were pushed for us.  We do the
                // pairing here; note they're in forward order on the stack
                // (last key/value pushed last) so we collect them reversed.
                let pairs: Vec<Token> = self.operand_stack.drain(..).collect();
                let mut i = 0;
                while i + 1 < pairs.len() {
                    if let Token::Name(key) = &pairs[i] {
                        let key = key.clone();
                        let val = token_to_pdf_object(&pairs[i + 1]);
                        self.inline_image_dict.insert(key, val);
                        i += 2;
                    } else {
                        i += 1;
                    }
                }
                // Do NOT clear operand_stack here — the InlineImageData token
                // will be pushed to it by the tokenizer loop in interpret().
            }

            "EI" => {
                // The operand stack should have InlineImageData at the top
                // (pushed by tokenizer after ID).
                let img_data = match self.operand_stack.pop() {
                    Some(Token::InlineImageData(d)) => d,
                    _ => {
                        self.inline_image_dict.clear();
                        self.in_inline_image = false;
                        return;
                    }
                };

                // Expand abbreviated keys/values to full PDF names.
                let full_dict = expand_inline_dict(&self.inline_image_dict);

                match crate::image_decoder::decode_image(&full_dict, &img_data) {
                    Some(img) => {
                        let ctm = &self.state.current().ctm;
                        let width_pts  = (ctm.a * ctm.a + ctm.b * ctm.b).sqrt();
                        let height_pts = (ctm.c * ctm.c + ctm.d * ctm.d).sqrt();
                        self.commands.push(RenderCommand::DrawImage {
                            x: ctm.e, y: ctm.f,
                            width_pts:  width_pts.max(0.1),
                            height_pts: height_pts.max(0.1),
                            pixels: img.pixels,
                            img_w: img.width,
                            img_h: img.height,
                        });
                    }
                    None => {
                        self.has_unsupported_image = true;
                    }
                }

                self.inline_image_dict.clear();
                self.in_inline_image = false;
            }

            // ── Shading (gradient) ────────────────────────────────────────────
            "sh" => {
                let name = match self.operand_stack.pop() {
                    Some(Token::Name(n)) => n,
                    _ => return,
                };
                if let Some(res) = &self.resources {
                    if let Some(shading) = res.shadings.get(&name).cloned() {
                        self.commands.push(RenderCommand::DrawShading {
                            shading,
                            ctm: self.state.current().ctm.clone(),
                        });
                    }
                }
            }

            _ => {} // unknown: ignore
        }
    }

    // ── Helper: classify colorspace name ─────────────────────────────────────

    fn classify_colorspace(&self, name: &str) -> ColorspaceKind {
        use crate::resource::ResolvedColorspace;

        // Check pre-resolved colorspaces from page resources first.
        // resource.rs resolves ICCBased streams so we get profile bytes here.
        if let Some(res) = self.resources.as_ref() {
            if let Some(rcs) = res.colorspaces.get(name) {
                return match rcs {
                    ResolvedColorspace::DeviceRGB  => ColorspaceKind::DeviceRGB,
                    ResolvedColorspace::DeviceGray => ColorspaceKind::DeviceGray,
                    ResolvedColorspace::DeviceCMYK => ColorspaceKind::DeviceCMYK,
                    ResolvedColorspace::Lab        => ColorspaceKind::Lab,
                    ResolvedColorspace::Separation => ColorspaceKind::Separation,
                    ResolvedColorspace::ICCBased { n_channels, profile } => {
                        ColorspaceKind::ICCBased { n_channels: *n_channels, profile: profile.clone() }
                    }
                    ResolvedColorspace::Pattern    => ColorspaceKind::Pattern,
                    ResolvedColorspace::Unknown    => ColorspaceKind::Unknown,
                };
            }
        }

        // Fallback: builtin names that don't need resource lookup.
        match name {
            "DeviceRGB" | "CalRGB" | "sRGB"      => ColorspaceKind::DeviceRGB,
            "DeviceGray" | "CalGray"              => ColorspaceKind::DeviceGray,
            "DeviceCMYK"                          => ColorspaceKind::DeviceCMYK,
            "Lab"                                 => ColorspaceKind::Lab,
            "Separation" | "DeviceN" | "Indexed"  => ColorspaceKind::Separation,
            "Pattern"                             => ColorspaceKind::Pattern,
            _                                     => ColorspaceKind::Unknown,
        }
    }

    // ── Helper: decode bytes and emit a DrawGlyph + TextItem ─────────────────

    fn emit_text_bytes(&mut self, bytes: Vec<u8>) {
        let cmap = self.current_to_unicode.as_ref();
        // decode to chars for ToUnicode-based text items
        let chars = decode_text_bytes(&bytes, cmap);
        // CID path: if current_cid, build glyph_ids from 2-byte codes
        let mut text_str: String = chars.iter().collect();
        let mut advance = chars.len() as f64 * self.font_size * self.current_advance_ratio as f64;

        // Combine CTM with text_matrix so Tm rotation/scale is honoured.
        let combined_ctm = self.state.current().ctm.multiply(&self.text_matrix);

        if self.current_cid {
            // Composite (Type0/CID) font. Embedded subset CID fonts often can't be
            // rasterised by glyph index via fontdue (returns empty glyphs), so prefer
            // the ToUnicode-decoded characters rendered with the fallback font
            // (font_id 0 -> system Arial/DejaVu). This makes text VISIBLE (correct
            // glyphs, fallback shapes) instead of blank.
            if cmap.is_some() && !chars.is_empty() {
                advance = chars.len() as f64 * self.font_size * self.current_advance_ratio as f64;
                self.commands.push(RenderCommand::DrawGlyph {
                    x:          0.0, // glyph origin; position carried by combined_ctm (text_matrix)
                    y:          0.0,
                    codepoints: chars.clone(),
                    font_id:    0, // force fallback font for shapes
                    font_size:  self.font_size,
                    color:      self.fill_color.clone(),
                    ctm:        combined_ctm,
                    glyph_ids:  Vec::new(),
                });
            } else {
                // No ToUnicode — best effort: treat 2-byte codes as GIDs in the embedded font.
                let mut gids: Vec<u16> = Vec::new();
                let mut i = 0;
                while i + 1 < bytes.len() {
                    gids.push(((bytes[i] as u16) << 8) | bytes[i + 1] as u16);
                    i += 2;
                }
                advance = gids.len() as f64 * self.font_size * self.current_advance_ratio as f64;
                self.commands.push(RenderCommand::DrawGlyph {
                    x:          0.0, // glyph origin; position carried by combined_ctm (text_matrix)
                    y:          0.0,
                    codepoints: Vec::new(),
                    font_id:    self.current_font_id,
                    font_size:  self.font_size,
                    color:      self.fill_color.clone(),
                    ctm:        combined_ctm,
                    glyph_ids:  gids,
                });
            }
        } else {
            // Non-CID path: same as before
            self.commands.push(RenderCommand::DrawGlyph {
                x:          0.0, // glyph origin; position carried by combined_ctm (text_matrix)
                y:          0.0,
                codepoints: chars,
                font_id:    self.current_font_id,
                font_size:  self.font_size,
                color:      self.fill_color.clone(),
                ctm:        combined_ctm,
                glyph_ids:  Vec::new(),
            });
        }

        if !text_str.trim().is_empty() {
            self.text_items.push(TextItem {
                x:         self.text_x,
                y:         self.text_y,
                text:      text_str,
                font_size: self.font_size,
            });
        }

        self.text_x += advance;
        // Keep text_matrix translation in sync with logical text position.
        self.text_matrix.e = self.text_x;
        self.text_matrix.f = self.text_y;
    }

    // ── Low-level stack helpers ───────────────────────────────────────────────

    fn pop_f(&mut self) -> f64 {
        match self.operand_stack.pop() {
            Some(Token::Real(r))    => r,
            Some(Token::Integer(i)) => i as f64,
            _ => 0.0,
        }
    }

    fn pop_bytes(&mut self) -> Vec<u8> {
        match self.operand_stack.pop() {
            Some(Token::StringLit(v)) => v,
            _ => Vec::new(),
        }
    }

    fn emit(&mut self, fill: bool, stroke: bool, _close: bool) {
        let path = std::mem::replace(&mut self.current_path, Path::new());
        let fa   = self.state.current().fill_alpha;
        let sa   = self.state.current().stroke_alpha;
        let bm   = self.state.current().blend_mode;
        let ctm  = self.state.current().ctm.clone();
        let eo   = self.even_odd;
        self.even_odd = false;

        // ── Pattern fill handling ─────────────────────────────────────────────
        if fill && self.fill_cs == ColorspaceKind::Pattern {
            if let Some(pat_name) = self.current_fill_pattern.clone() {
                let entry = self.resources.as_ref()
                    .and_then(|r| r.patterns.get(&pat_name))
                    .cloned();
                match entry {
                    Some(PatternEntry::Tiling { paint_type, tiling_type: _, x_step, y_step, bbox, matrix, content, resources: pat_res }) => {
                        // Interpret pattern cell content stream
                        let cell_w = (bbox.2 - bbox.0).abs().max(1.0);
                        let cell_h = (bbox.3 - bbox.1).abs().max(1.0);
                        let mut sub = Interpreter::new(cell_w, cell_h);
                        sub.fill_color   = self.fill_color.clone();
                        sub.stroke_color = self.stroke_color.clone();
                        // Use pattern's own resources if available, else parent
                        if let Some(pr) = pat_res {
                            // Build a minimal PageResources from the pattern's resource dict
                            // For simplicity, reuse parent resources (pattern's resources are rarely complex)
                            sub.resources = self.resources.clone();
                            let _ = pr; // TODO: merge pattern resources
                        } else {
                            sub.resources = self.resources.clone();
                        }
                        let cell_cmds = sub.interpret(&content).to_vec();
                        let uncolored_fill = if paint_type == 2 {
                            self.current_fill_pattern_color.clone()
                        } else {
                            None
                        };
                        self.commands.push(RenderCommand::DrawTilingPattern {
                            cell_cmds, bbox, x_step, y_step, matrix, ctm, paint_type, uncolored_fill,
                        });
                        // Also emit a PaintPath for stroke-only if stroke is requested
                        if stroke {
                            let mut sc = self.stroke_color.clone();
                            sc.a = (sc.a * sa).clamp(0.0, 1.0);
                            let lw = self.state.current().line_width;
                            let lc = self.state.current().line_cap;
                            let lj = self.state.current().line_join;
                            let sm = self.state.current().smask.clone();
                            let ctm2 = self.state.current().ctm.clone();
                            self.commands.push(RenderCommand::PaintPath {
                                path, fill_color: None, stroke_color: Some(sc),
                                line_width: lw, line_cap: lc, line_join: lj,
                                even_odd: eo, ctm: ctm2, blend_mode: bm, smask: sm,
                            });
                        }
                        return;
                    }
                    Some(PatternEntry::Shading { shading, matrix }) => {
                        let m = matrix.multiply(&ctm);
                        self.commands.push(RenderCommand::DrawShadingPattern { shading, ctm: m });
                        return;
                    }
                    None => {
                        // Pattern not found — fall through to normal PaintPath (white fill)
                    }
                }
            }
        }

        // ── Normal path paint ─────────────────────────────────────────────────
        let fc = if fill {
            let mut c = self.fill_color.clone();
            c.a = (c.a * fa).clamp(0.0, 1.0);
            Some(c)
        } else { None };
        let sc = if stroke {
            let mut c = self.stroke_color.clone();
            c.a = (c.a * sa).clamp(0.0, 1.0);
            Some(c)
        } else { None };
        let lw = self.state.current().line_width;
        let lc = self.state.current().line_cap;
        let lj = self.state.current().line_join;
        let sm = self.state.current().smask.clone();
        self.commands.push(RenderCommand::PaintPath {
            path, fill_color: fc, stroke_color: sc,
            line_width: lw, line_cap: lc, line_join: lj,
            even_odd: eo, ctm, blend_mode: bm, smask: sm,
        });
    }
}

// ── Inline image helpers ──────────────────────────────────────────────────────

/// Convert a Token (from the operand stack) to a PdfObject for inline dict.
fn token_to_pdf_object(tok: &Token) -> PdfObject {
    match tok {
        Token::Name(s)       => PdfObject::Name(s.clone()),
        Token::Integer(i)    => PdfObject::Integer(*i),
        Token::Real(r)       => PdfObject::Real(*r),
        Token::StringLit(v)  => PdfObject::Str(v.clone()),
        _                    => PdfObject::Null,
    }
}

/// Expand abbreviated inline image dict keys and colorspace values to their
/// full PDF equivalents.
fn expand_inline_dict(dict: &HashMap<String, PdfObject>) -> HashMap<String, PdfObject> {
    // Key abbreviations defined in PDF spec Table 89.
    let key_abbrevs: &[(&str, &str)] = &[
        ("W",   "Width"),
        ("H",   "Height"),
        ("CS",  "ColorSpace"),
        ("BPC", "BitsPerComponent"),
        ("F",   "Filter"),
        ("D",   "Decode"),
        ("IM",  "ImageMask"),
        ("I",   "Interpolate"),
    ];

    // Colorspace name abbreviations (Table 90).
    let cs_abbrevs: &[(&str, &str)] = &[
        ("G",    "DeviceGray"),
        ("RGB",  "DeviceRGB"),
        ("CMYK", "DeviceCMYK"),
        ("I",    "Indexed"),
    ];

    // Filter abbreviations (Table 92).
    let filter_abbrevs: &[(&str, &str)] = &[
        ("AHx", "ASCIIHexDecode"),
        ("A85", "ASCII85Decode"),
        ("LZW", "LZWDecode"),
        ("Fl",  "FlateDecode"),
        ("RL",  "RunLengthDecode"),
        ("CCF", "CCITTFaxDecode"),
        ("DCT", "DCTDecode"),
    ];

    let mut out = HashMap::new();

    for (k, v) in dict {
        let full_key = key_abbrevs.iter()
            .find(|(a, _)| *a == k.as_str())
            .map(|(_, b)| *b)
            .unwrap_or(k.as_str());

        let full_val = match full_key {
            "ColorSpace" => {
                if let PdfObject::Name(n) = v {
                    let expanded = cs_abbrevs.iter()
                        .find(|(a, _)| *a == n.as_str())
                        .map(|(_, b)| *b)
                        .unwrap_or(n.as_str());
                    PdfObject::Name(expanded.to_string())
                } else {
                    v.clone()
                }
            }
            "Filter" => {
                if let PdfObject::Name(n) = v {
                    let expanded = filter_abbrevs.iter()
                        .find(|(a, _)| *a == n.as_str())
                        .map(|(_, b)| *b)
                        .unwrap_or(n.as_str());
                    PdfObject::Name(expanded.to_string())
                } else {
                    v.clone()
                }
            }
            _ => v.clone(),
        };

        out.insert(full_key.to_string(), full_val);
    }

    out
}

// ── Text decoding ─────────────────────────────────────────────────────────────

/// Decode SCN/scn color operands based on active colorspace.
/// Returns None if the colorspace requires PDFium fallback (Pattern, Unknown).
fn decode_scn_color(values: &[f64], cs: &ColorspaceKind) -> Option<Color> {
    match cs {
        ColorspaceKind::Lab => {
            // PDF Lab: L ∈ [0,100], a ∈ [−128,127], b ∈ [−128,127]
            if values.len() >= 3 {
                Some(Color::from_lab(values[0] as f32, values[1] as f32, values[2] as f32))
            } else if values.len() == 1 {
                Some(Color::from_gray((values[0] as f32 / 100.0).clamp(0.0, 1.0)))
            } else { Some(Color::black()) }
        }
        ColorspaceKind::DeviceRGB => match values.len() {
            3 => Some(Color::from_rgb(values[0] as f32, values[1] as f32, values[2] as f32)),
            1 => Some(Color::from_gray(values[0] as f32)),
            _ => Some(Color::black()),
        },
        ColorspaceKind::DeviceGray | ColorspaceKind::Separation => match values.len() {
            1 => Some(Color::from_gray(1.0 - values[0] as f32)), // tint: 0=white, 1=ink
            3 => Some(Color::from_rgb(values[0] as f32, values[1] as f32, values[2] as f32)),
            4 => Some(Color::from_cmyk(values[0] as f32, values[1] as f32,
                                        values[2] as f32, values[3] as f32)),
            0 => Some(Color::black()),
            _ => Some(Color::black()),
        },
        ColorspaceKind::DeviceCMYK => match values.len() {
            4 => Some(Color::from_cmyk(values[0] as f32, values[1] as f32,
                                        values[2] as f32, values[3] as f32)),
            _ => Some(Color::black()),
        },
        ColorspaceKind::ICCBased { n_channels, profile } => {
            // Build a 1-pixel buffer and apply ICC transform → sRGB.
            let needed = *n_channels as usize;
            if values.len() < needed { return Some(Color::black()); }
            let pixel: Vec<u8> = (0..needed)
                .map(|i| (values[i].clamp(0.0, 1.0) * 255.0).round() as u8)
                .collect();
            if let Some(rgba) = crate::image_decoder::apply_icc_to_rgba(&pixel, profile, *n_channels, 1, 1) {
                Some(Color::from_rgb(
                    rgba[0] as f32 / 255.0,
                    rgba[1] as f32 / 255.0,
                    rgba[2] as f32 / 255.0,
                ))
            } else {
                Some(Color::black())
            }
        }
        ColorspaceKind::Pattern => Some(Color::black()), // color handled by pattern mechanism
        ColorspaceKind::Unknown => None, // triggers has_complex_colorspace
    }
}

/// Decode PDF string bytes to chars using the font's ToUnicode CMap when available.
///
/// Strategy:
///   1. If a ToUnicode map is present, try 2-byte CIDFont lookup first, then
///      single-byte lookup, then fall back to Latin-1 passthrough.
///   2. If no map, attempt UTF-8 decode then fall back to Latin-1.
pub fn decode_text_bytes(bytes: &[u8], to_unicode: Option<&HashMap<u32, char>>) -> Vec<char> {
    if let Some(map) = to_unicode {
        let mut chars = Vec::new();
        let mut i = 0;
        while i < bytes.len() {
            // Try 2-byte lookup first (CIDFont / Type0).
            if i + 1 < bytes.len() {
                let code2 = ((bytes[i] as u32) << 8) | (bytes[i + 1] as u32);
                if let Some(&ch) = map.get(&code2) {
                    chars.push(ch);
                    i += 2;
                    continue;
                }
            }
            // Try single-byte lookup.
            let code1 = bytes[i] as u32;
            if let Some(&ch) = map.get(&code1) {
                chars.push(ch);
                i += 1;
                continue;
            }
            // No map hit — use the byte as Latin-1.
            chars.push(bytes[i] as char);
            i += 1;
        }
        chars
    } else {
        // No ToUnicode: attempt UTF-8, fall back to Latin-1.
        String::from_utf8(bytes.to_vec())
            .unwrap_or_else(|_| bytes.iter().map(|&b| b as char).collect())
            .chars()
            .collect()
    }
}
