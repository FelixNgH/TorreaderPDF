#[derive(Clone, Debug)]
pub struct Matrix {
    pub a: f64, pub b: f64,
    pub c: f64, pub d: f64,
    pub e: f64, pub f: f64,
}

impl Matrix {
    pub fn identity() -> Self {
        Self { a: 1.0, b: 0.0, c: 0.0, d: 1.0, e: 0.0, f: 0.0 }
    }

    pub fn multiply(&self, o: &Matrix) -> Matrix {
        Matrix {
            a: self.a * o.a + self.c * o.b,
            b: self.b * o.a + self.d * o.b,
            c: self.a * o.c + self.c * o.d,
            d: self.b * o.c + self.d * o.d,
            e: self.a * o.e + self.c * o.f + self.e,
            f: self.b * o.e + self.d * o.f + self.f,
        }
    }

    pub fn transform_point(&self, x: f64, y: f64) -> (f64, f64) {
        (self.a * x + self.c * y + self.e,
         self.b * x + self.d * y + self.f)
    }

    /// Return the inverse matrix, or None if the matrix is singular.
    pub fn inverted(&self) -> Option<Matrix> {
        let det = self.a * self.d - self.b * self.c;
        if det.abs() < 1e-12 { return None; }
        let inv_det = 1.0 / det;
        Some(Matrix {
            a:  self.d  * inv_det,
            b: -self.b  * inv_det,
            c: -self.c  * inv_det,
            d:  self.a  * inv_det,
            e: (self.c * self.f - self.d * self.e) * inv_det,
            f: (self.b * self.e - self.a * self.f) * inv_det,
        })
    }
}

#[derive(Clone, Debug)]
pub struct Color {
    pub r: f32, pub g: f32, pub b: f32, pub a: f32,
}

impl Color {
    pub fn black() -> Self { Color { r: 0.0, g: 0.0, b: 0.0, a: 1.0 } }
    pub fn white() -> Self { Color { r: 1.0, g: 1.0, b: 1.0, a: 1.0 } }

    pub fn from_gray(g: f32) -> Self {
        Color { r: g, g, b: g, a: 1.0 }
    }

    pub fn from_rgb(r: f32, g: f32, b: f32) -> Self {
        Color { r, g, b, a: 1.0 }
    }

    pub fn from_cmyk(c: f32, m: f32, y: f32, k: f32) -> Self {
        Color {
            r: (1.0 - c) * (1.0 - k),
            g: (1.0 - m) * (1.0 - k),
            b: (1.0 - y) * (1.0 - k),
            a: 1.0,
        }
    }

    /// CIE L*a*b* → sRGB (D50 illuminant, Bradford chromatic adaptation to D65).
    /// L*: 0–100, a*: −128–127, b*: −128–127 (PDF normalized to 0–1 range per spec).
    /// Input is the PDF-normalized form: L ∈ [0,1], a ∈ [−0.5,0.5] approx.
    /// For PDF SCN operator: L is in [0,100], a/b in [−128,127] per PDF spec §8.6.5.7.
    pub fn from_lab(l: f32, a: f32, b: f32) -> Self {
        // Lab → XYZ with D50 white point
        let fy = (l + 16.0) / 116.0;
        let fx = a / 500.0 + fy;
        let fz = fy - b / 200.0;
        let f = |t: f32| -> f32 {
            if t > 0.206896552 { t * t * t }
            else { 3.0 * 0.042510881 * (t - 0.137931034) }
        };
        // D50 white point: Xn=0.9642, Yn=1.0000, Zn=0.8251
        let x = 0.9642 * f(fx);
        let y = 1.0000 * f(fy);
        let z = 0.8251 * f(fz);
        // XYZ D50 → linear sRGB D65 (combined Bradford + RGB matrix)
        let r =  3.134274799 * x - 1.617275932 * y - 0.490724699 * z;
        let g = -0.978795502 * x + 1.916227661 * y + 0.033444040 * z;
        let b_c = 0.071976988 * x - 0.228984974 * y + 1.405386585 * z;
        // Linear → sRGB gamma (IEC 61966-2-1)
        let gamma = |c: f32| -> f32 {
            let c = c.clamp(0.0, 1.0);
            if c <= 0.0031308 { 12.92 * c } else { 1.055 * c.powf(1.0 / 2.4) - 0.055 }
        };
        Color { r: gamma(r), g: gamma(g), b: gamma(b_c), a: 1.0 }
    }

    pub fn to_rgba8(&self) -> [u8; 4] {
        let clamp = |v: f32| (v.clamp(0.0, 1.0) * 255.0) as u8;
        [clamp(self.r), clamp(self.g), clamp(self.b), clamp(self.a)]
    }
}

/// SMask type: whether the mask is taken from the alpha or luminosity channel.
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum SmaskType { Alpha, Luminosity }

/// Soft mask definition — stored in GraphicsState when gs sets /SMask.
/// `form_content` holds the raw decoded content stream of the mask Form XObject.
/// The rasterizer runs a sub-interpreter on these bytes to produce the mask canvas.
#[derive(Clone, Debug)]
pub struct SmaskDef {
    pub smask_type:      SmaskType,
    pub form_content:    Vec<u8>,
    pub bc:              [f32; 3],   // backdrop color (default black = [0,0,0])
    pub form_width_pt:   f64,
    pub form_height_pt:  f64,
}

/// PDF standard blend modes (ISO 32000-2 §11.3.5).
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum BlendMode {
    Normal,
    Multiply,
    Screen,
    Overlay,
    Darken,
    Lighten,
    ColorDodge,
    ColorBurn,
    HardLight,
    SoftLight,
    Difference,
    Exclusion,
}

impl BlendMode {
    pub fn from_name(s: &str) -> Self {
        match s {
            "Multiply"   => BlendMode::Multiply,
            "Screen"     => BlendMode::Screen,
            "Overlay"    => BlendMode::Overlay,
            "Darken"     => BlendMode::Darken,
            "Lighten"    => BlendMode::Lighten,
            "ColorDodge" => BlendMode::ColorDodge,
            "ColorBurn"  => BlendMode::ColorBurn,
            "HardLight"  => BlendMode::HardLight,
            "SoftLight"  => BlendMode::SoftLight,
            "Difference" => BlendMode::Difference,
            "Exclusion"  => BlendMode::Exclusion,
            _            => BlendMode::Normal,
        }
    }
}

#[derive(Clone, Debug)]
pub struct GraphicsState {
    pub ctm:              Matrix,
    pub stroke_color:     Color,
    pub fill_color:       Color,
    pub line_width:       f64,
    pub line_cap:         u8,
    pub line_join:        u8,
    pub miter_limit:      f64,
    pub dash_array:       Vec<f64>,
    pub dash_phase:       f64,
    pub font_name:        String,
    pub font_size:        f64,
    pub text_matrix:      Matrix,
    pub text_line_matrix: Matrix,
    pub char_spacing:     f64,
    pub word_spacing:     f64,
    pub text_scale:       f64,
    pub text_rise:        f64,
    pub fill_rule_even_odd: bool,
    /// Axis-aligned clip rectangle in user space (x_min, y_min, x_max, y_max).
    /// Set by W/W* when the current path is a simple rectangle.
    pub clip_rect: Option<(f64, f64, f64, f64)>,
    /// Opacity for fill paint (0.0=transparent, 1.0=opaque). Set by gs /ca.
    pub fill_alpha:   f32,
    /// Opacity for stroke paint (0.0=transparent, 1.0=opaque). Set by gs /CA.
    pub stroke_alpha: f32,
    /// Current blend mode. Set by gs /BM. Default: Normal.
    pub blend_mode:   BlendMode,
    /// Active soft mask. Set by gs /SMask. None = no mask.
    pub smask:        Option<SmaskDef>,
    /// Active fill pattern name (from /Pattern CS + scn). None = solid color.
    pub fill_pattern:   Option<String>,
    /// Active stroke pattern name (from /Pattern CS + SCN). None = solid color.
    pub stroke_pattern: Option<String>,
}

impl GraphicsState {
    pub fn new() -> Self {
        GraphicsState {
            ctm:              Matrix::identity(),
            stroke_color:     Color::black(),
            fill_color:       Color::black(),
            line_width:       1.0,
            line_cap:         0,
            line_join:        0,
            miter_limit:      10.0,
            dash_array:       Vec::new(),
            dash_phase:       0.0,
            font_name:        "Helvetica".to_string(),
            font_size:        12.0,
            text_matrix:      Matrix::identity(),
            text_line_matrix: Matrix::identity(),
            char_spacing:     0.0,
            word_spacing:     0.0,
            text_scale:       1.0,
            text_rise:        0.0,
            fill_rule_even_odd: false,
            clip_rect:        None,
            fill_alpha:       1.0,
            stroke_alpha:     1.0,
            blend_mode:       BlendMode::Normal,
            smask:            None,
            fill_pattern:     None,
            stroke_pattern:   None,
        }
    }
}

impl Default for GraphicsState {
    fn default() -> Self { Self::new() }
}

pub struct StateStack {
    stack: Vec<GraphicsState>,
}

impl StateStack {
    pub fn new() -> Self {
        Self { stack: vec![GraphicsState::new()] }
    }

    pub fn current(&self) -> &GraphicsState {
        self.stack.last().unwrap()
    }

    pub fn current_mut(&mut self) -> &mut GraphicsState {
        self.stack.last_mut().unwrap()
    }

    pub fn push(&mut self) {
        let s = self.current().clone();
        self.stack.push(s);
    }

    pub fn pop(&mut self) {
        if self.stack.len() > 1 { self.stack.pop(); }
    }
}
