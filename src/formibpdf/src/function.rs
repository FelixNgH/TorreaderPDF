// function.rs — PDF Function evaluation (ISO 32000-2 §7.10)
// Used by shadings (tint transforms), uncolored patterns, transfer functions.

use crate::pdf_object::PdfObject;
use crate::xref::XRefTable;
use std::collections::HashMap;

#[derive(Debug, Clone)]
pub enum PdfFunction {
    Sampled {
        domain:    Vec<(f32, f32)>,
        range:     Vec<(f32, f32)>,
        size:      Vec<u32>,
        order:     u8,
        encode:    Vec<(f32, f32)>,
        decode:    Vec<(f32, f32)>,
        samples:   Vec<f32>,
        n_inputs:  u32,
        n_outputs: u32,
    },
    Exponential {
        c0: Vec<f32>,
        c1: Vec<f32>,
        n:  f32,
    },
    Stitching {
        functions: Vec<PdfFunction>,
        bounds:    Vec<f32>,
        encode:    Vec<(f32, f32)>,
        domain:    (f32, f32),
    },
    PostScript {
        code: Vec<u8>,
    },
}

impl PdfFunction {
    pub fn parse(obj: &PdfObject, xref: &XRefTable, data: &[u8]) -> Option<Self> {
        let resolved = xref.resolve(data, obj);
        let (dict, stream_data): (&HashMap<String, PdfObject>, Option<&[u8]>) = match &resolved {
            PdfObject::Dict(d)             => (d, None),
            PdfObject::Stream { dict, data: sd } => (dict, Some(sd.as_slice())),
            _ => return None,
        };

        let ft = dict.get("FunctionType").and_then(|o| o.as_i64()).unwrap_or(2);

        match ft {
            0 => {
                let domain = parse_pairs(dict, "Domain").unwrap_or_else(|| vec![(0.0, 1.0)]);
                let range  = parse_pairs(dict, "Range").unwrap_or_default();
                let size: Vec<u32> = dict.get("Size")
                    .and_then(|o| o.as_array())
                    .map(|a| a.iter().filter_map(|x| x.as_i64()).map(|v| v as u32).collect())
                    .unwrap_or_default();
                let order = dict.get("Order").and_then(|o| o.as_i64()).unwrap_or(1) as u8;
                let encode = parse_pairs(dict, "Encode").unwrap_or_else(|| {
                    size.iter().map(|&s| (0.0, (s.saturating_sub(1)) as f32)).collect()
                });
                let decode = parse_pairs(dict, "Decode").unwrap_or_else(|| range.clone());
                let bps = dict.get("BitsPerSample").and_then(|o| o.as_i64()).unwrap_or(8) as u32;
                let n_inputs  = domain.len() as u32;
                let n_outputs = if range.is_empty() { 1 } else { range.len() as u32 };

                let samples = if let Some(sd) = stream_data {
                    decode_samples(sd, bps, n_outputs as usize,
                                   size.iter().map(|&s| s as usize).product::<usize>().max(1))
                } else { Vec::new() };

                Some(PdfFunction::Sampled { domain, range, size, order, encode, decode, samples, n_inputs, n_outputs })
            }
            2 => {
                let c0 = arr_to_f32(dict.get("C0"));
                let c1 = arr_to_f32(dict.get("C1"));
                let c0 = if c0.is_empty() { vec![0.0] } else { c0 };
                let c1 = if c1.is_empty() { vec![1.0] } else { c1 };
                let n  = dict.get("N").and_then(|o| o.as_f64()).unwrap_or(1.0) as f32;
                Some(PdfFunction::Exponential { c0, c1, n })
            }
            3 => {
                let sub_objs: Vec<PdfObject> = dict.get("Functions")
                    .and_then(|o| o.as_array())
                    .map(|a| a.iter().map(|o| xref.resolve(data, o)).collect())
                    .unwrap_or_default();
                let functions: Vec<PdfFunction> = sub_objs.iter()
                    .filter_map(|o| Self::parse(o, xref, data))
                    .collect();
                let bounds = arr_to_f32(dict.get("Bounds"));
                let encode = parse_pairs(dict, "Encode").unwrap_or_default();
                let domain = dict.get("Domain")
                    .and_then(|o| o.as_array())
                    .and_then(|a| {
                        let d0 = a.get(0)?.as_f64()? as f32;
                        let d1 = a.get(1)?.as_f64()? as f32;
                        Some((d0, d1))
                    }).unwrap_or((0.0, 1.0));
                Some(PdfFunction::Stitching { functions, bounds, encode, domain })
            }
            4 => {
                let code = stream_data.map(|s| s.to_vec()).unwrap_or_default();
                Some(PdfFunction::PostScript { code })
            }
            _ => None,
        }
    }

    pub fn evaluate(&self, inputs: &[f32]) -> Vec<f32> {
        match self {
            PdfFunction::Sampled { domain, size, encode: _, decode, samples, n_inputs, n_outputs, .. } => {
                let ni = *n_inputs as usize;
                let no = *n_outputs as usize;
                if ni == 0 || no == 0 { return vec![0.0; no]; }
                if inputs.len() < ni   { return vec![0.0; no]; }
                if domain.len() < ni   { return vec![0.0; no]; }

                let mut lo    = vec![0usize; ni];
                let mut hi    = vec![0usize; ni];
                let mut delta = vec![0f32; ni];
                for i in 0..ni {
                    let (dmin, dmax) = domain[i];
                    let clamped = inputs[i].max(dmin).min(dmax);
                    let u = if dmax > dmin { (clamped - dmin) / (dmax - dmin) } else { 0.0 };
                    let dim = size.get(i).copied().unwrap_or(1) as usize;
                    let pos = u * (dim.saturating_sub(1)) as f32;
                    lo[i]    = pos.floor() as usize;
                    hi[i]    = (lo[i] + 1).min(dim.saturating_sub(1));
                    delta[i] = pos - lo[i] as f32;
                }

                let mut stride = vec![1usize; ni];
                for i in (0..ni.saturating_sub(1)).rev() {
                    stride[i] = stride[i + 1] * size.get(i + 1).copied().unwrap_or(1) as usize;
                }

                let total: usize = size.iter().map(|&s| s as usize).product::<usize>().max(1);
                if samples.len() < total * no { return vec![0.0; no]; }

                let mut result = vec![0f32; no];
                for mask in 0..(1usize << ni) {
                    let mut idx    = 0usize;
                    let mut weight = 1.0f32;
                    for d in 0..ni {
                        let use_hi = (mask >> d) & 1 != 0;
                        idx    += (if use_hi { hi[d] } else { lo[d] }) * stride[d];
                        weight *= if use_hi { delta[d] } else { 1.0 - delta[d] };
                    }
                    let base = idx * no;
                    for o in 0..no {
                        result[o] += weight * samples.get(base + o).copied().unwrap_or(0.0);
                    }
                }

                result.iter().enumerate().map(|(o, &s)| {
                    let (dmin, dmax) = decode.get(o).copied().unwrap_or((0.0, 1.0));
                    dmin + s * (dmax - dmin)
                }).collect()
            }

            PdfFunction::Exponential { c0, c1, n } => {
                let t = inputs.first().copied().unwrap_or(0.0);
                c0.iter().zip(c1.iter())
                    .map(|(&a, &b)| a + t.powf(*n) * (b - a))
                    .collect()
            }

            PdfFunction::Stitching { functions, bounds, encode, domain } => {
                if inputs.is_empty() || functions.is_empty() { return Vec::new(); }
                let t = inputs[0].max(domain.0).min(domain.1);
                let mut piece = 0usize;
                while piece < bounds.len() && t >= bounds[piece] { piece += 1; }
                piece = piece.min(functions.len().saturating_sub(1));
                let (d0, d1) = if piece < bounds.len() {
                    let lo = if piece == 0 { domain.0 } else { bounds[piece - 1] };
                    let hi = bounds[piece];
                    (lo, hi)
                } else {
                    let lo = bounds.last().copied().unwrap_or(domain.0);
                    (lo, domain.1)
                };
                let enc = encode.get(piece).copied().unwrap_or((0.0, 1.0));
                let t2 = if (d1 - d0).abs() > 1e-9 { enc.0 + (t - d0) / (d1 - d0) * (enc.1 - enc.0) } else { enc.0 };
                functions[piece].evaluate(&[t2])
            }

            PdfFunction::PostScript { code } => {
                let mut stack: Vec<f32> = inputs.to_vec();
                let text = String::from_utf8_lossy(code);
                // Strip { } braces — simplified, no nested control flow
                let clean = text.replace('{', " ").replace('}', " ");
                for tok in clean.split_whitespace() {
                    if let Ok(v) = tok.parse::<f32>() { stack.push(v); continue; }
                    match tok {
                        "true"  => stack.push(1.0),
                        "false" => stack.push(0.0),
                        "abs"   => { if let Some(a)   = stack.pop()               { stack.push(a.abs()); } }
                        "neg"   => { if let Some(a)   = stack.pop()               { stack.push(-a); } }
                        "sqrt"  => { if let Some(a)   = stack.pop()               { stack.push(a.sqrt()); } }
                        "cos"   => { if let Some(a)   = stack.pop()               { stack.push(a.to_radians().cos()); } }
                        "sin"   => { if let Some(a)   = stack.pop()               { stack.push(a.to_radians().sin()); } }
                        "atan"  => { if let (Some(b), Some(a)) = (stack.pop(), stack.pop()) { stack.push(a.atan2(b).to_degrees()); } }
                        "exp"   => { if let Some(a)   = stack.pop()               { stack.push(a.exp()); } }
                        "ln"    => { if let Some(a)   = stack.pop()               { stack.push(a.max(1e-30).ln()); } }
                        "log"   => { if let Some(a)   = stack.pop()               { stack.push(a.max(1e-30).log10()); } }
                        "ceiling" => { if let Some(a) = stack.pop()               { stack.push(a.ceil()); } }
                        "floor" => { if let Some(a)   = stack.pop()               { stack.push(a.floor()); } }
                        "round" => { if let Some(a)   = stack.pop()               { stack.push(a.round()); } }
                        "truncate" => { if let Some(a) = stack.pop()              { stack.push(a.trunc()); } }
                        "add"   => { if let (Some(b), Some(a)) = (stack.pop(), stack.pop()) { stack.push(a + b); } }
                        "sub"   => { if let (Some(b), Some(a)) = (stack.pop(), stack.pop()) { stack.push(a - b); } }
                        "mul"   => { if let (Some(b), Some(a)) = (stack.pop(), stack.pop()) { stack.push(a * b); } }
                        "div"   => { if let (Some(b), Some(a)) = (stack.pop(), stack.pop()) { stack.push(if b.abs() > 1e-30 { a / b } else { 0.0 }); } }
                        "idiv"  => { if let (Some(b), Some(a)) = (stack.pop(), stack.pop()) { stack.push(if b.abs() > 0.5 { (a as i32 / b as i32) as f32 } else { 0.0 }); } }
                        "mod"   => { if let (Some(b), Some(a)) = (stack.pop(), stack.pop()) { stack.push(a % b); } }
                        "gt"    => { if let (Some(b), Some(a)) = (stack.pop(), stack.pop()) { stack.push(if a > b { 1.0 } else { 0.0 }); } }
                        "ge"    => { if let (Some(b), Some(a)) = (stack.pop(), stack.pop()) { stack.push(if a >= b { 1.0 } else { 0.0 }); } }
                        "lt"    => { if let (Some(b), Some(a)) = (stack.pop(), stack.pop()) { stack.push(if a < b { 1.0 } else { 0.0 }); } }
                        "le"    => { if let (Some(b), Some(a)) = (stack.pop(), stack.pop()) { stack.push(if a <= b { 1.0 } else { 0.0 }); } }
                        "eq"    => { if let (Some(b), Some(a)) = (stack.pop(), stack.pop()) { stack.push(if (a - b).abs() < 1e-6 { 1.0 } else { 0.0 }); } }
                        "ne"    => { if let (Some(b), Some(a)) = (stack.pop(), stack.pop()) { stack.push(if (a - b).abs() >= 1e-6 { 1.0 } else { 0.0 }); } }
                        "not"   => { if let Some(a)   = stack.pop()               { stack.push(if a == 0.0 { 1.0 } else { 0.0 }); } }
                        "and"   => { if let (Some(b), Some(a)) = (stack.pop(), stack.pop()) { stack.push(if a != 0.0 && b != 0.0 { 1.0 } else { 0.0 }); } }
                        "or"    => { if let (Some(b), Some(a)) = (stack.pop(), stack.pop()) { stack.push(if a != 0.0 || b != 0.0 { 1.0 } else { 0.0 }); } }
                        "xor"   => { if let (Some(b), Some(a)) = (stack.pop(), stack.pop()) { stack.push(if (a != 0.0) ^ (b != 0.0) { 1.0 } else { 0.0 }); } }
                        "bitshift" => { if let (Some(b), Some(a)) = (stack.pop(), stack.pop()) { let i = a as i32; let s = b as i32; stack.push((if s >= 0 { i.wrapping_shl(s as u32) } else { i.wrapping_shr((-s) as u32) }) as f32); } }
                        "pop"   => { let _ = stack.pop(); }
                        "dup"   => { if let Some(&v) = stack.last() { stack.push(v); } }
                        "exch"  => { let n = stack.len(); if n >= 2 { stack.swap(n-1, n-2); } }
                        "copy"  => { if let Some(n) = stack.pop() { let n = n as usize; let s = stack.len(); if n <= s { let v: Vec<f32> = stack[s-n..].to_vec(); stack.extend_from_slice(&v); } } }
                        "index" => { if let Some(n) = stack.pop() { let idx = n as usize; let s = stack.len(); if idx < s { stack.push(stack[s-1-idx]); } } }
                        "roll"  => { if stack.len() >= 2 { let j = stack.pop().unwrap() as i32; let n = stack.pop().unwrap() as usize; let s = stack.len(); if n > 0 && n <= s { let m = ((j % n as i32 + n as i32) as usize) % n; stack[s-n..].rotate_left(m); } } }
                        "if"    => { if let (Some(proc_v), Some(cond)) = (stack.pop(), stack.pop()) { let _ = (proc_v, cond); } } // simplified
                        "ifelse" => { if stack.len() >= 3 { let _else = stack.pop(); let _if = stack.pop(); let _cond = stack.pop(); } } // simplified
                        _ => {}
                    }
                }
                stack
            }
        }
    }
}

// ── Helpers ───────────────────────────────────────────────────────────────────

fn parse_pairs(dict: &HashMap<String, PdfObject>, key: &str) -> Option<Vec<(f32, f32)>> {
    let arr = dict.get(key)?.as_array()?;
    let vals: Vec<f32> = arr.iter().filter_map(|o| o.as_f64()).map(|v| v as f32).collect();
    if vals.len() < 2 { return None; }
    Some(vals.chunks(2).filter_map(|c| if c.len() == 2 { Some((c[0], c[1])) } else { None }).collect())
}

fn arr_to_f32(obj: Option<&PdfObject>) -> Vec<f32> {
    match obj {
        Some(PdfObject::Array(a)) => a.iter().filter_map(|o| o.as_f64()).map(|v| v as f32).collect(),
        Some(o) => o.as_f64().map(|v| vec![v as f32]).unwrap_or_default(),
        None => Vec::new(),
    }
}

fn decode_samples(data: &[u8], bps: u32, n_outputs: usize, n_samples: usize) -> Vec<f32> {
    let total = n_samples * n_outputs;
    let mut out = Vec::with_capacity(total);
    let max = ((1u64 << bps) - 1) as f32;
    match bps {
        8 => {
            for &b in data.iter().take(total) {
                out.push(b as f32 / max);
            }
        }
        16 => {
            let mut i = 0;
            while i + 1 < data.len() && out.len() < total {
                let v = u16::from_be_bytes([data[i], data[i+1]]);
                out.push(v as f32 / max);
                i += 2;
            }
        }
        _ => {
            // Bit-level read (MSB first)
            let mut bits_read = 0u64;
            let mut buf = 0u32;
            let mut buf_bits = 0u32;
            for &byte in data {
                buf = (buf << 8) | byte as u32;
                buf_bits += 8;
                while buf_bits >= bps && out.len() < total {
                    buf_bits -= bps;
                    let sample = (buf >> buf_bits) & ((1 << bps) - 1);
                    out.push(sample as f32 / max);
                    bits_read += bps as u64;
                    let _ = bits_read;
                }
            }
        }
    }
    while out.len() < total { out.push(0.0); }
    out
}
