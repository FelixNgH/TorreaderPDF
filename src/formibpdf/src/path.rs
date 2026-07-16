use crate::graphics_state::Matrix;

#[derive(Debug, Clone)]
pub enum PathCmd {
    MoveTo(f64, f64),
    LineTo(f64, f64),
    CurveTo(f64, f64, f64, f64, f64, f64),
    ClosePath,
    Rect(f64, f64, f64, f64),
}

#[derive(Debug, Clone, Default)]
pub struct Path {
    pub cmds: Vec<PathCmd>,
}

impl Path {
    pub fn new() -> Self { Self::default() }

    pub fn move_to(&mut self, x: f64, y: f64) {
        self.cmds.push(PathCmd::MoveTo(x, y));
    }
    pub fn line_to(&mut self, x: f64, y: f64) {
        self.cmds.push(PathCmd::LineTo(x, y));
    }
    pub fn curve_to(&mut self, x1: f64, y1: f64, x2: f64, y2: f64, x3: f64, y3: f64) {
        self.cmds.push(PathCmd::CurveTo(x1, y1, x2, y2, x3, y3));
    }
    pub fn close(&mut self) { self.cmds.push(PathCmd::ClosePath); }
    pub fn rect(&mut self, x: f64, y: f64, w: f64, h: f64) {
        self.cmds.push(PathCmd::Rect(x, y, w, h));
    }
    pub fn is_empty(&self) -> bool { self.cmds.is_empty() }
    pub fn clear(&mut self)        { self.cmds.clear(); }
    pub fn iter(&self) -> std::slice::Iter<'_, PathCmd> { self.cmds.iter() }

    /// If this path is a single Rect command, return (x_min, y_min, x_max, y_max).
    /// Used by W/W* to implement axis-aligned clipping without PDFium fallback.
    pub fn as_rect(&self) -> Option<(f64, f64, f64, f64)> {
        if self.cmds.len() == 1 {
            if let PathCmd::Rect(x, y, w, h) = self.cmds[0] {
                return Some((x.min(x+w), y.min(y+h), x.max(x+w), y.max(y+h)));
            }
        }
        None
    }

    pub fn current_point(&self) -> Option<(f64, f64)> {
        for cmd in self.cmds.iter().rev() {
            match cmd {
                PathCmd::MoveTo(x,y) | PathCmd::LineTo(x,y) => return Some((*x,*y)),
                PathCmd::CurveTo(_,_,_,_,x,y) => return Some((*x,*y)),
                _ => {}
            }
        }
        None
    }

    pub fn transformed(&self, m: &Matrix) -> Path {
        let mut out = Path::new();
        for cmd in &self.cmds {
            match *cmd {
                PathCmd::MoveTo(x, y) => {
                    let (nx, ny) = m.transform_point(x, y);
                    out.cmds.push(PathCmd::MoveTo(nx, ny));
                }
                PathCmd::LineTo(x, y) => {
                    let (nx, ny) = m.transform_point(x, y);
                    out.cmds.push(PathCmd::LineTo(nx, ny));
                }
                PathCmd::CurveTo(x1, y1, x2, y2, x3, y3) => {
                    let (nx1, ny1) = m.transform_point(x1, y1);
                    let (nx2, ny2) = m.transform_point(x2, y2);
                    let (nx3, ny3) = m.transform_point(x3, y3);
                    out.cmds.push(PathCmd::CurveTo(nx1, ny1, nx2, ny2, nx3, ny3));
                }
                PathCmd::ClosePath => out.cmds.push(PathCmd::ClosePath),
                PathCmd::Rect(x, y, w, h) => {
                    // Expand rect → transformed quadrilateral
                    let (x1, y1) = m.transform_point(x,     y);
                    let (x2, y2) = m.transform_point(x + w, y);
                    let (x3, y3) = m.transform_point(x + w, y + h);
                    let (x4, y4) = m.transform_point(x,     y + h);
                    out.cmds.push(PathCmd::MoveTo(x1, y1));
                    out.cmds.push(PathCmd::LineTo(x2, y2));
                    out.cmds.push(PathCmd::LineTo(x3, y3));
                    out.cmds.push(PathCmd::LineTo(x4, y4));
                    out.cmds.push(PathCmd::ClosePath);
                }
            }
        }
        out
    }
}
