// text_order.rs — spatial text clustering for reading order (v2.0)

use crate::interpreter::TextItem;

/// Return page text in visual reading order (top-to-bottom, left-to-right,
/// with basic multi-column detection).
pub fn extract_ordered(items: &[TextItem]) -> String {
    if items.is_empty() { return String::new(); }

    let mean_font: f64 = {
        let sum: f64 = items.iter().map(|i| i.font_size).sum();
        sum / items.len() as f64
    };

    // Group into lines by y proximity
    struct Line<'a> { y: f64, items: Vec<&'a TextItem> }
    let mut lines: Vec<Line> = Vec::new();
    for item in items {
        let tol = (item.font_size * 0.5).max(1.0);
        if let Some(ln) = lines.iter_mut().find(|l| (item.y - l.y).abs() < tol) {
            ln.items.push(item);
        } else {
            lines.push(Line { y: item.y, items: vec![item] });
        }
    }

    // Sort top→bottom (PDF y increases upward, so higher y = earlier)
    lines.sort_by(|a, b| b.y.partial_cmp(&a.y).unwrap_or(std::cmp::Ordering::Equal));

    let col_gap = mean_font.max(4.0);
    let mut result = String::new();
    let mut prev_y = f64::MAX;

    for line in &mut lines {
        line.items.sort_by(|a, b| a.x.partial_cmp(&b.x).unwrap_or(std::cmp::Ordering::Equal));

        // Detect column breaks within the line
        let mut columns: Vec<String> = Vec::new();
        let mut cur = String::new();
        for (i, item) in line.items.iter().enumerate() {
            if i > 0 {
                let prev_end = line.items[i-1].x
                    + line.items[i-1].font_size * line.items[i-1].text.len() as f64 * 0.6;
                if item.x - prev_end > col_gap * 2.0 {
                    columns.push(cur.trim_end().to_string());
                    cur = String::new();
                } else if item.x - prev_end > 0.5 {
                    cur.push(' ');
                }
            }
            cur.push_str(&item.text);
        }
        if !cur.is_empty() { columns.push(cur.trim_end().to_string()); }

        let line_text = columns.join("    ");
        if line_text.trim().is_empty() { continue; }

        if prev_y != f64::MAX {
            let gap = prev_y - line.y;
            if gap > mean_font * 1.5 {
                result.push_str("\n\n");
            } else {
                result.push('\n');
            }
        }
        result.push_str(&line_text);
        prev_y = line.y;
    }

    result
}
