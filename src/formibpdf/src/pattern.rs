// pattern.rs — Tiling pattern tile origin computation helper

use crate::graphics_state::Matrix;

/// Compute tile (tx, ty) offsets that cover `clip_bbox` given the tiling pattern parameters.
/// Returns at most 10000 tile origins.
pub fn tile_origins(
    bbox:      (f64, f64, f64, f64),
    x_step:    f64,
    y_step:    f64,
    clip_bbox: (f64, f64, f64, f64),
    matrix:    &Matrix,
) -> Vec<(f64, f64)> {
    if x_step.abs() < 1e-9 || y_step.abs() < 1e-9 { return Vec::new(); }

    // Transform clip_bbox corners to pattern space via matrix inverse
    let clip_min_x; let clip_min_y; let clip_max_x; let clip_max_y;
    if let Some(inv) = matrix.inverted() {
        let corners = [
            inv.transform_point(clip_bbox.0, clip_bbox.1),
            inv.transform_point(clip_bbox.2, clip_bbox.1),
            inv.transform_point(clip_bbox.2, clip_bbox.3),
            inv.transform_point(clip_bbox.0, clip_bbox.3),
        ];
        clip_min_x = corners.iter().map(|c| c.0).fold(f64::MAX, f64::min);
        clip_max_x = corners.iter().map(|c| c.0).fold(f64::MIN, f64::max);
        clip_min_y = corners.iter().map(|c| c.1).fold(f64::MAX, f64::min);
        clip_max_y = corners.iter().map(|c| c.1).fold(f64::MIN, f64::max);
    } else {
        clip_min_x = clip_bbox.0; clip_max_x = clip_bbox.2;
        clip_min_y = clip_bbox.1; clip_max_y = clip_bbox.3;
    }

    let i_start = ((clip_min_x - bbox.0) / x_step).floor() as i64 - 1;
    let i_end   = ((clip_max_x - bbox.0) / x_step).ceil()  as i64 + 1;
    let j_start = ((clip_min_y - bbox.1) / y_step).floor() as i64 - 1;
    let j_end   = ((clip_max_y - bbox.1) / y_step).ceil()  as i64 + 1;

    let mut origins = Vec::new();
    'outer: for j in j_start..=j_end {
        for i in i_start..=i_end {
            let tx = i as f64 * x_step;
            let ty = j as f64 * y_step;
            // Tile bbox in pattern space
            let tx0 = bbox.0 + tx; let tx1 = bbox.2 + tx;
            let ty0 = bbox.1 + ty; let ty1 = bbox.3 + ty;
            if tx0 < clip_max_x && tx1 > clip_min_x && ty0 < clip_max_y && ty1 > clip_min_y {
                origins.push((tx, ty));
                if origins.len() >= 10_000 { break 'outer; }
            }
        }
    }
    origins
}
