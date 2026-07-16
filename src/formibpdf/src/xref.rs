use std::collections::HashMap;
use crate::pdf_object::{PdfObject, parse_object, skip_ws};

#[derive(Debug, Clone)]
pub struct XRefEntry {
    pub offset:     u64,
    pub generation: u16,
    pub in_use:     bool,
    pub compressed_in: Option<u32>,
}

pub struct XRefTable {
    pub entries: HashMap<u32, XRefEntry>,
    pub trailer: HashMap<String, PdfObject>,
}

impl XRefTable {
    pub fn parse(data: &[u8]) -> Result<Self, String> {
        let offset = find_startxref(data)?;
        let mut table = XRefTable { entries: HashMap::new(), trailer: HashMap::new() };
        table.load_xref_at(data, offset, 0)?;
        Ok(table)
    }

    // Load XRef section starting at `offset`. depth prevents infinite /Prev loops.
    fn load_xref_at(&mut self, data: &[u8], offset: usize, depth: u8) -> Result<(), String> {
        if depth > 8 || offset >= data.len() { return Ok(()); }
        let mut pos = offset;
        skip_ws(data, &mut pos);

        if data[pos..].starts_with(b"xref") {
            // ── Traditional XRef table ────────────────────────────────────
            pos += 4;
            let mut section_prev: Option<i64> = None;
            loop {
                skip_ws(data, &mut pos);
                if pos >= data.len() { break; }
                if data[pos..].starts_with(b"trailer") {
                    pos += 7;
                    skip_ws(data, &mut pos);
                    if let Some(PdfObject::Dict(t)) = parse_object(data, &mut pos) {
                        // Capture THIS section's own /Prev before merging (merged trailer
                        // pins to the first section via or_insert).
                        section_prev = t.get("Prev").and_then(|o| o.as_i64());
                        // Merge trailer (first wins for Root/Info)
                        for (k, v) in t { self.trailer.entry(k).or_insert(v); }
                    }
                    break;
                }
                // Read subsection header: first_obj count
                let line = read_line(data, &mut pos);
                let parts: Vec<&str> = line.split_whitespace().collect();
                if parts.len() < 2 { break; }
                let first: u32 = parts[0].parse().unwrap_or(0);
                let count: u32 = parts[1].parse().unwrap_or(0);
                for i in 0..count {
                    let entry_line = read_line(data, &mut pos);
                    let ep: Vec<&str> = entry_line.split_whitespace().collect();
                    if ep.len() < 3 { continue; }
                    let off:  u64  = ep[0].parse().unwrap_or(0);
                    let gen:  u16  = ep[1].parse().unwrap_or(0);
                    let used: bool = ep[2] == "n";
                    // Only insert if not already present (later XRef sections override earlier)
                    self.entries.entry(first + i)
                        .or_insert(XRefEntry { offset: off, generation: gen, in_use: used, compressed_in: None });
                }
            }
            // Follow THIS section's own /Prev (see note in the XRef-stream branch).
            if let Some(prev) = section_prev {
                if prev > 0 { let _ = self.load_xref_at(data, prev as usize, depth + 1); }
            }
        } else {
            // ── XRef Stream (PDF 1.5+) ────────────────────────────────────
            // Read: "N G obj << dict >> stream ... endstream"
            skip_number(data, &mut pos); skip_ws(data, &mut pos); // obj_id
            skip_number(data, &mut pos); skip_ws(data, &mut pos); // generation
            if data[pos..].starts_with(b"obj") { pos += 3; } skip_ws(data, &mut pos);

            let dict = match parse_object(data, &mut pos) {
                Some(PdfObject::Dict(d)) => d,
                _ => return Err("XRef stream: no dict".to_string()),
            };
            skip_ws(data, &mut pos);
            if !data[pos..].starts_with(b"stream") {
                return Err("XRef stream: no stream keyword".to_string());
            }
            pos += 6;
            if pos < data.len() && data[pos] == b'\r' { pos += 1; }
            if pos < data.len() && data[pos] == b'\n' { pos += 1; }

            let length = match dict.get("Length") {
                Some(PdfObject::Integer(l)) => *l as usize,
                _ => 0,
            };
            let raw = if length > 0 && pos + length <= data.len() {
                data[pos..pos+length].to_vec()
            } else {
                // Try to find endstream
                find_before_endstream(data, pos)
            };

            let stream_data = decompress_if_needed(&dict, &raw);

            // /W field widths, /Index subsections
            let w: Vec<usize> = dict.get("W")
                .and_then(|o| o.as_array())
                .map(|a| a.iter().map(|x| x.as_i64().unwrap_or(0) as usize).collect())
                .unwrap_or_else(|| vec![1,2,1]);
            if w.len() < 3 { return Err("XRef stream: /W too short".to_string()); }
            let (w0,w1,w2) = (w[0], w[1], w[2]);
            let entry_size = w0 + w1 + w2;
            if entry_size == 0 { return Err("XRef stream: zero entry size".to_string()); }

            let index: Vec<u32> = dict.get("Index")
                .and_then(|o| o.as_array())
                .map(|a| a.iter().map(|x| x.as_i64().unwrap_or(0) as u32).collect())
                .unwrap_or_else(|| {
                    let size = dict.get("Size").and_then(|o|o.as_i64()).unwrap_or(0);
                    vec![0, size as u32]
                });

            let mut byte_off = 0;
            let mut idx_pair = 0;
            while idx_pair + 1 < index.len() && byte_off + entry_size <= stream_data.len() {
                let first = index[idx_pair];
                let count = index[idx_pair + 1];
                idx_pair += 2;
                for i in 0..count {
                    if byte_off + entry_size > stream_data.len() { break; }
                    let typ  = read_be_uint(&stream_data[byte_off..], w0);
                    let f2   = read_be_uint(&stream_data[byte_off+w0..], w1);
                    let f3   = read_be_uint(&stream_data[byte_off+w0+w1..], w2);
                    byte_off += entry_size;
                    let obj_id = first + i;
                    let typ = if w0 == 0 { 1 } else { typ }; // default type=1
                    match typ {
                        1 => { // uncompressed: f2=offset, f3=generation
                            self.entries.entry(obj_id).or_insert(XRefEntry {
                                offset: f2, generation: f3 as u16, in_use: true, compressed_in: None });
                        }
                        2 => { // compressed: f2=ObjStm object number, f3=index within stream
                            self.entries.entry(obj_id).or_insert(XRefEntry {
                                offset: f3, generation: 0, in_use: true,
                                compressed_in: Some(f2 as u32) });
                        }
                        _ => {} // free
                    }
                }
            }
            // XRef stream dict IS the trailer. Follow THIS dict's own /Prev (not the
            // merged trailer's, which or_insert pins to the first section → infinite
            // self-loop and the main xref never loads).
            let this_prev = dict.get("Prev").and_then(|o| o.as_i64());
            for (k, v) in dict { self.trailer.entry(k).or_insert(v); }
            if let Some(prev) = this_prev {
                if prev > 0 { let _ = self.load_xref_at(data, prev as usize, depth + 1); }
            }
        }
        Ok(())
    }

    /// Load indirect object by ID. Returns decompressed Stream if applicable.
    pub fn load_object(&self, data: &[u8], obj_id: u32) -> Option<PdfObject> {
        let entry = self.entries.get(&obj_id)?;
        if !entry.in_use { return None; }
        // If this is a compressed object within an ObjStm
        if let Some(stm) = entry.compressed_in {
            let idx = entry.offset as usize;
            return self.load_compressed(data, stm, idx, obj_id, 0);
        }
        let mut pos = entry.offset as usize;
        skip_ws(data, &mut pos);
        skip_number(data, &mut pos); skip_ws(data, &mut pos); // obj_id
        skip_number(data, &mut pos); skip_ws(data, &mut pos); // generation
        if data[pos..].starts_with(b"obj") { pos += 3; } else { return None; }
        skip_ws(data, &mut pos);

        let obj = parse_object(data, &mut pos)?;
        skip_ws(data, &mut pos);
        if !data[pos..].starts_with(b"stream") { return Some(obj); }
        pos += 6;
        if pos < data.len() && data[pos] == b'\r' { pos += 1; }
        if pos < data.len() && data[pos] == b'\n' { pos += 1; }

        let dict = match &obj { PdfObject::Dict(d) => d.clone(), _ => return Some(obj) };

        // Resolve /Length (may be indirect ref)
        let length: usize = match dict.get("Length") {
            Some(PdfObject::Integer(l))  => *l as usize,
            Some(PdfObject::Ref(id, _)) => self.load_object(data, *id)
                .and_then(|o| o.as_i64()).unwrap_or(0) as usize,
            _ => 0,
        };
        let raw = if length > 0 && pos + length <= data.len() {
            data[pos..pos+length].to_vec()
        } else {
            find_before_endstream(data, pos)
        };

        let stream_data = decompress_if_needed(&dict, &raw);
        Some(PdfObject::Stream { dict, data: stream_data })
    }

    /// Resolve Ref → actual object (single level, no deep recursion).
    pub fn resolve(&self, data: &[u8], obj: &PdfObject) -> PdfObject {
        match obj {
            PdfObject::Ref(id, _) => self.load_object(data, *id).unwrap_or(PdfObject::Null),
            other => other.clone(),
        }
    }

    fn load_compressed(&self, data:&[u8], stm_num:u32, index:usize, want_obj:u32, depth:u8) -> Option<PdfObject> {
        if depth > 8 { return None; }
        if stm_num == want_obj { return None; }

        // Load the object stream
        let stm = self.load_object(data, stm_num)?;
        let (dict, sdata) = match stm {
            PdfObject::Stream { dict, data } => (dict, data),
            _ => return None,
        };

        // N and First from the ObjStm dict
        let n = dict.get("N").and_then(|o| o.as_i64()).unwrap_or(0) as usize;
        let first = dict.get("First").and_then(|o| o.as_i64()).unwrap_or(0) as usize;

        // Parse the object number + offset pairs from sdata
        let mut pairs: Vec<(u32, usize)> = Vec::new();
        let mut p = 0usize;
        while p < sdata.len() {
            // skip whitespace
            while p < sdata.len() && sdata[p].is_ascii_whitespace() { p += 1; }
            if p >= sdata.len() { break; }
            // read objnum
            let start = p;
            while p < sdata.len() && sdata[p].is_ascii_digit() { p += 1; }
            if p == start { break; }
            let obj_num = std::str::from_utf8(&sdata[start..p]).ok()?.parse::<u32>().ok()?;
            // skip whitespace
            while p < sdata.len() && sdata[p].is_ascii_whitespace() { p += 1; }
            if p >= sdata.len() { break; }
            // read offset
            let start2 = p;
            while p < sdata.len() && sdata[p].is_ascii_digit() { p += 1; }
            if p == start2 { break; }
            let off = std::str::from_utf8(&sdata[start2..p]).ok()?.parse::<usize>().ok()?;
            pairs.push((obj_num, off));
        }

        // Choose the pair by index, else search by want_obj
        let chosen_offset = if index < pairs.len() {
            Some(pairs[index].1)
        } else {
            // search for want_obj
            pairs.iter().find(|(num, _)| *num == want_obj).map(|(_, o)| *o)
        }?;

        let mut obj_pos = first + chosen_offset;
        parse_object(&sdata, &mut obj_pos).into()
    }
    // End impl
}

// ── Helpers ───────────────────────────────────────────────────────────────────

fn find_startxref(data: &[u8]) -> Result<usize, String> {
    let search = if data.len() > 1024 { &data[data.len()-1024..] } else { data };
    let base = data.len() - search.len();
    // Search BACKWARDS for last "startxref"
    let needle = b"startxref";
    let mut found = None;
    for i in (0..=search.len().saturating_sub(needle.len())).rev() {
        if search[i..].starts_with(needle) { found = Some(base + i); break; }
    }
    let pos = found.ok_or("startxref not found")?;
    let mut p = pos + b"startxref".len();
    skip_ws(data, &mut p);
    let s = read_digits(data, &mut p);
    let off: usize = s.parse().map_err(|_| "bad startxref offset")?;
    Ok(off)
}

fn read_line(data: &[u8], pos: &mut usize) -> String {
    let start = *pos;
    while *pos < data.len() && data[*pos] != b'\n' && data[*pos] != b'\r' { *pos += 1; }
    let s = String::from_utf8_lossy(&data[start..*pos]).trim().to_string();
    if *pos < data.len() && data[*pos] == b'\r' { *pos += 1; }
    if *pos < data.len() && data[*pos] == b'\n' { *pos += 1; }
    s
}

fn skip_number(data: &[u8], pos: &mut usize) {
    while *pos < data.len() && data[*pos].is_ascii_digit() { *pos += 1; }
}

fn read_digits(data: &[u8], pos: &mut usize) -> String {
    let start = *pos;
    while *pos < data.len() && data[*pos].is_ascii_digit() { *pos += 1; }
    String::from_utf8_lossy(&data[start..*pos]).to_string()
}

fn read_be_uint(data: &[u8], width: usize) -> u64 {
    let mut v = 0u64;
    for i in 0..width.min(data.len()) { v = (v << 8) | data[i] as u64; }
    v
}

fn find_before_endstream(data: &[u8], start: usize) -> Vec<u8> {
    let needle = b"endstream";
    for i in start..data.len().saturating_sub(needle.len()) {
        if data[i..].starts_with(needle) {
            let end = if i > 0 && data[i-1] == b'\n' { i-1 } else { i };
            let end = if end > 0 && data[end-1] == b'\r' { end-1 } else { end };
            return data[start..end].to_vec();
        }
    }
    data[start..].to_vec()
}

pub fn decompress_if_needed(dict: &HashMap<String, PdfObject>, raw: &[u8]) -> Vec<u8> {
    use std::io::Read;
    let filter_name = dict.get("Filter").and_then(|f| match f {
        PdfObject::Name(n) => Some(n.as_str()),
        PdfObject::Array(a) => a.first().and_then(|x| x.as_name()),
        _ => None,
    }).unwrap_or("");

    match filter_name {
        "FlateDecode" => {
            let mut out = Vec::new();
            if flate2::read::ZlibDecoder::new(raw).read_to_end(&mut out).is_ok() && !out.is_empty() {
                return apply_predictor(out, dict);
            }
            out.clear();
            if flate2::read::DeflateDecoder::new(raw).read_to_end(&mut out).is_ok() && !out.is_empty() {
                return apply_predictor(out, dict);
            }
            raw.to_vec()
        }
        "LZWDecode" => {
            let decoded = lzw_decode(raw, dict);
            apply_predictor(decoded, dict)
        }
        "ASCII85Decode" => ascii85_decode(raw),
        "ASCIIHexDecode" => {
            // Hex-encoded bytes: "48 65 6c 6c 6f >" → bytes
            let hex: String = raw.iter().map(|&b| b as char).collect();
            hex.split_whitespace()
               .filter(|s| *s != ">")
               .filter_map(|s| u8::from_str_radix(s, 16).ok())
               .collect()
        }
        "RunLengthDecode" => {
            let mut out = Vec::new();
            let mut i = 0;
            while i < raw.len() {
                let b = raw[i] as i16;
                i += 1;
                if b == 128 { break; }
                else if b >= 0 {
                    let n = (b + 1) as usize;
                    if i + n <= raw.len() { out.extend_from_slice(&raw[i..i+n]); i += n; }
                } else {
                    let n = (1 - b) as usize;
                    if i < raw.len() { let byte = raw[i]; i += 1; out.extend(std::iter::repeat(byte).take(n)); }
                }
            }
            out
        }
        _ => raw.to_vec(),
    }
}

fn lzw_decode(raw: &[u8], dict: &HashMap<String, PdfObject>) -> Vec<u8> {
    // Early predict: check EarlyChange parameter (default 1 in PDF)
    let early = dict.get("DecodeParms")
        .and_then(|o| if let PdfObject::Dict(d) = o { d.get("EarlyChange").and_then(|v| v.as_i64()) } else { None })
        .unwrap_or(1);
    let mut table: Vec<Vec<u8>> = (0u16..=255).map(|i| vec![i as u8]).collect();
    table.push(vec![]); // 256 = clear
    table.push(vec![]); // 257 = eoi
    let mut out: Vec<u8> = Vec::new();
    let mut bits = 9u32;
    let mut bit_pos = 0u64;
    let mut prev: Option<Vec<u8>> = None;

    let read_code = |bp: &mut u64, bits: u32| -> Option<u16> {
        let byte_off = (*bp / 8) as usize;
        let bit_off  = (*bp % 8) as u32;
        if byte_off + 3 > raw.len() { return None; }
        let val = ((raw[byte_off] as u32) << 16)
                | ((raw.get(byte_off+1).copied().unwrap_or(0) as u32) << 8)
                |  (raw.get(byte_off+2).copied().unwrap_or(0) as u32);
        *bp += bits as u64;
        Some(((val >> (24 - bit_off - bits)) & ((1 << bits) - 1)) as u16)
    };

    loop {
        let code = match read_code(&mut bit_pos, bits) { Some(c) => c, None => break };
        if code == 256 { // clear
            table.truncate(258);
            bits = 9;
            prev = None;
            continue;
        }
        if code == 257 { break; } // eoi

        let entry: Vec<u8> = if (code as usize) < table.len() {
            table[code as usize].clone()
        } else if let Some(ref p) = prev {
            let mut e = p.clone(); e.push(p[0]); e
        } else { break };

        out.extend_from_slice(&entry);
        if let Some(ref p) = prev {
            let mut new_entry = p.clone();
            new_entry.push(entry[0]);
            table.push(new_entry);
            let next_size = table.len() + if early == 1 { 1 } else { 0 };
            if next_size > (1 << bits) && bits < 12 { bits += 1; }
        }
        prev = Some(entry);
    }
    out
}

fn paeth(a: u8, b: u8, c: u8) -> u8 {
    let a_i = a as i32;
    let b_i = b as i32;
    let c_i = c as i32;
    let p = a_i + b_i - c_i;
    let pa = (p - a_i).abs();
    let pb = (p - b_i).abs();
    let pc = (p - c_i).abs();
    if pa <= pb && pa <= pc { a } else if pb <= pc { b } else { c }
}

fn ascii85_decode(raw: &[u8]) -> Vec<u8> {
    let mut out = Vec::new();
    let mut group = [0u32; 5];
    let mut gi = 0;
    for &b in raw {
        if b == b'~' { break; }
        if b == b'z' && gi == 0 { out.extend_from_slice(&[0u8; 4]); continue; }
        if b.is_ascii_whitespace() { continue; }
        if b < 33 || b > 117 { break; }
        group[gi] = (b - 33) as u32;
        gi += 1;
        if gi == 5 {
            let n = group[0]*85u32.pow(4) + group[1]*85u32.pow(3) + group[2]*85u32.pow(2) + group[3]*85 + group[4];
            out.extend_from_slice(&n.to_be_bytes());
            gi = 0;
        }
    }
    if gi > 0 {
        for i in gi..5 { group[i] = 84; }
        let n = group[0]*85u32.pow(4) + group[1]*85u32.pow(3) + group[2]*85u32.pow(2) + group[3]*85 + group[4];
        let bytes = n.to_be_bytes();
        out.extend_from_slice(&bytes[..gi-1]);
    }
    out
}

// Feature A: apply_predictor
pub fn apply_predictor(data: Vec<u8>, dict: &HashMap<String, PdfObject>) -> Vec<u8> {
    // Read DecodeParms
    let (predictor, columns, colors, bits) = {
        let mut pred = 1i64;
        let mut cols = 1i64;
        let mut cols_colors = 1i64;
        let mut bpc = 8i64;

        if let Some(o) = dict.get("DecodeParms") {
            match o {
                PdfObject::Dict(d) => {
                    pred = d.get("Predictor").and_then(|v| v.as_i64()).unwrap_or(1);
                    cols = d.get("Columns").and_then(|v| v.as_i64()).unwrap_or(1);
                    cols_colors = d.get("Colors").and_then(|v| v.as_i64()).unwrap_or(1);
                    bpc = d.get("BitsPerComponent").and_then(|v| v.as_i64()).unwrap_or(8);
                }
                PdfObject::Array(arr) => {
                    if let Some(PdfObject::Dict(d)) = arr.first() {
                        pred = d.get("Predictor").and_then(|v| v.as_i64()).unwrap_or(1);
                        cols = d.get("Columns").and_then(|v| v.as_i64()).unwrap_or(1);
                        cols_colors = d.get("Colors").and_then(|v| v.as_i64()).unwrap_or(1);
                        bpc = d.get("BitsPerComponent").and_then(|v| v.as_i64()).unwrap_or(8);
                    }
                }
                _ => {}
            }
        }
        (pred as i32, cols as usize, cols_colors as usize, bpc as usize)
    };

    let predictor = predictor as i32;
    let row_len = ((columns * colors * bits + 7) / 8) as usize;
    let bpp = std::cmp::max(1, (colors * bits + 7) / 8) as usize;

    if row_len == 0 { return data; }

    match predictor {
        1 => data,
        2 => {
            // TIFF predictor (8-bit)
            let mut v = data;
            let stride = row_len;
            let mut pos = 0;
            while pos + stride <= v.len() {
                for i in bpp..stride {
                    let idx = pos + i;
                    let left = v[idx - bpp];
                    v[idx] = v[idx].wrapping_add(left);
                }
                pos += stride;
            }
            v
        }
        p if p >= 10 => {
            // PNG predictors
            let mut out: Vec<u8> = Vec::new();
            let stride = 1 + row_len;
            let rows = data.len() / stride;
            let mut prev = vec![0u8; row_len];
            let mut pos = 0;
            for _ in 0..rows {
                let filter = data[pos];
                pos += 1;
                let row = &data[pos .. pos + row_len];
                pos += row_len;
                let mut recon = vec![0u8; row_len];
                match filter {
                    0 => { recon.copy_from_slice(row); }
                    1 => {
                        for i in 0..row_len {
                            let left = if i >= bpp { recon[i - bpp] } else { 0 };
                            recon[i] = row[i].wrapping_add(left);
                        }
                    }
                    2 => {
                        for i in 0..row_len {
                            recon[i] = row[i].wrapping_add(prev[i]);
                        }
                    }
                    3 => {
                        for i in 0..row_len {
                            let left = if i >= bpp { recon[i - bpp] } else { 0 };
                            let up = prev[i];
                            let add = (((left as i32) + (up as i32)) / 2) as u8;
                            recon[i] = row[i].wrapping_add(add);
                        }
                    }
                    4 => {
                        for i in 0..row_len {
                            let a = if i >= bpp { recon[i - bpp] } else { 0 };
                            let b = prev[i];
                            let c = if i >= bpp { prev[i - bpp] } else { 0 };
                            let pr = paeth(a, b, c);
                            recon[i] = row[i].wrapping_add(pr);
                        }
                    }
                    _ => { recon.copy_from_slice(row); }
                }
                out.extend_from_slice(&recon);
                prev = recon;
            }
            out
        }
        _ => data,
    }
}