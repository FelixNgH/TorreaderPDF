use std::collections::HashMap;

#[derive(Debug, Clone)]
pub enum PdfObject {
    Null,
    Bool(bool),
    Integer(i64),
    Real(f64),
    Name(String),
    Str(Vec<u8>),
    Array(Vec<PdfObject>),
    Dict(HashMap<String, PdfObject>),
    Stream { dict: HashMap<String, PdfObject>, data: Vec<u8> },
    Ref(u32, u16),
}

impl PdfObject {
    pub fn as_i64(&self) -> Option<i64> {
        match self { PdfObject::Integer(v) => Some(*v), _ => None }
    }
    pub fn as_f64(&self) -> Option<f64> {
        match self {
            PdfObject::Real(v)    => Some(*v),
            PdfObject::Integer(v) => Some(*v as f64),
            _ => None,
        }
    }
    pub fn as_name(&self) -> Option<&str> {
        match self { PdfObject::Name(s) => Some(s), _ => None }
    }
    pub fn as_bytes(&self) -> Option<&[u8]> {
        match self { PdfObject::Str(v) => Some(v), _ => None }
    }
    pub fn as_array(&self) -> Option<&[PdfObject]> {
        match self { PdfObject::Array(v) => Some(v), _ => None }
    }
    pub fn as_dict(&self) -> Option<&HashMap<String, PdfObject>> {
        match self { PdfObject::Dict(d) => Some(d), _ => None }
    }
    pub fn dict_get(&self, key: &str) -> Option<&PdfObject> {
        match self {
            PdfObject::Dict(d)        => d.get(key),
            PdfObject::Stream{dict,..}=> dict.get(key),
            _ => None,
        }
    }
    pub fn as_ref_id(&self) -> Option<(u32, u16)> {
        match self { PdfObject::Ref(id,gen) => Some((*id,*gen)), _ => None }
    }
}

// ── Parser ───────────────────────────────────────────────────────────────────

pub fn parse_object(data: &[u8], pos: &mut usize) -> Option<PdfObject> {
    skip_ws(data, pos);
    if *pos >= data.len() { return None; }

    // Comment
    if data[*pos] == b'%' {
        while *pos < data.len() && data[*pos] != b'\n' { *pos += 1; }
        return parse_object(data, pos);
    }

    // null / true / false
    if data[*pos..].starts_with(b"null")  { *pos += 4; return Some(PdfObject::Null); }
    if data[*pos..].starts_with(b"true")  { *pos += 4; return Some(PdfObject::Bool(true)); }
    if data[*pos..].starts_with(b"false") { *pos += 5; return Some(PdfObject::Bool(false)); }

    match data[*pos] {
        b'/' => {
            *pos += 1;
            let start = *pos;
            while *pos < data.len() && !is_delim_or_ws(data[*pos]) { *pos += 1; }
            let raw = &data[start..*pos];
            let s = decode_name(raw);
            Some(PdfObject::Name(s))
        }
        b'(' => {
            *pos += 1;
            let mut depth = 1i32;
            let mut buf = Vec::new();
            while *pos < data.len() && depth > 0 {
                match data[*pos] {
                    b'\\' => {
                        *pos += 1;
                        if *pos < data.len() {
                            buf.push(match data[*pos] {
                                b'n' => b'\n', b'r' => b'\r', b't' => b'\t',
                                b'b' => 0x08, b'f' => 0x0C,
                                c => c,
                            });
                            *pos += 1;
                        }
                    }
                    b'(' => { depth += 1; buf.push(b'('); *pos += 1; }
                    b')' => { depth -= 1; *pos += 1; if depth > 0 { buf.push(b')'); } }
                    c    => { buf.push(c); *pos += 1; }
                }
            }
            Some(PdfObject::Str(buf))
        }
        b'<' if *pos + 1 < data.len() && data[*pos+1] == b'<' => {
            *pos += 2;
            let mut dict = HashMap::new();
            loop {
                skip_ws(data, pos);
                if *pos >= data.len() { break; }
                if data[*pos..].starts_with(b">>") { *pos += 2; break; }
                if data[*pos] != b'/' { *pos += 1; continue; }
                *pos += 1;
                let ks = *pos;
                while *pos < data.len() && !is_delim_or_ws(data[*pos]) { *pos += 1; }
                let key = String::from_utf8_lossy(&data[ks..*pos]).to_string();
                if let Some(val) = parse_object(data, pos) {
                    dict.insert(key, val);
                }
            }
            Some(PdfObject::Dict(dict))
        }
        b'<' => {
            *pos += 1;
            let mut hex = Vec::new();
            while *pos < data.len() && data[*pos] != b'>' {
                if !data[*pos].is_ascii_whitespace() { hex.push(data[*pos]); }
                *pos += 1;
            }
            if *pos < data.len() { *pos += 1; }
            if hex.len() % 2 == 1 { hex.push(b'0'); }
            let out = hex.chunks_exact(2)
                .map(|c| (hv(c[0]) << 4) | hv(c[1])).collect();
            Some(PdfObject::Str(out))
        }
        b'[' => {
            *pos += 1;
            let mut arr = Vec::new();
            loop {
                skip_ws(data, pos);
                if *pos >= data.len() { break; }
                if data[*pos] == b']' { *pos += 1; break; }
                if let Some(v) = parse_object(data, pos) { arr.push(v); }
                else { *pos += 1; }
            }
            Some(PdfObject::Array(arr))
        }
        c if c == b'+' || c == b'-' || c == b'.' || c.is_ascii_digit() => {
            parse_number_or_ref(data, pos)
        }
        _ => { *pos += 1; None }
    }
}

fn parse_number_or_ref(data: &[u8], pos: &mut usize) -> Option<PdfObject> {
    let _save = *pos;
    let n1 = read_number_str(data, pos)?;
    let is_real = n1.contains('.');
    if is_real {
        return Some(PdfObject::Real(n1.parse().ok()?));
    }
    let v1: i64 = n1.parse().ok()?;
    let save2 = *pos;
    skip_ws(data, pos);
    if let Some(n2) = try_read_uint(data, pos) {
        skip_ws(data, pos);
        if *pos < data.len() && data[*pos] == b'R' {
            *pos += 1;
            return Some(PdfObject::Ref(v1 as u32, n2 as u16));
        }
    }
    *pos = save2;
    Some(PdfObject::Integer(v1))
}

fn read_number_str<'a>(data: &'a [u8], pos: &mut usize) -> Option<String> {
    let start = *pos;
    if *pos < data.len() && matches!(data[*pos], b'+' | b'-') { *pos += 1; }
    while *pos < data.len() && (data[*pos].is_ascii_digit() || data[*pos] == b'.') { *pos += 1; }
    if *pos == start { return None; }
    Some(String::from_utf8_lossy(&data[start..*pos]).to_string())
}

fn try_read_uint(data: &[u8], pos: &mut usize) -> Option<u64> {
    let start = *pos;
    while *pos < data.len() && data[*pos].is_ascii_digit() { *pos += 1; }
    if *pos == start { return None; }
    String::from_utf8_lossy(&data[start..*pos]).parse().ok()
}

pub fn skip_ws(data: &[u8], pos: &mut usize) {
    while *pos < data.len() {
        if data[*pos].is_ascii_whitespace() { *pos += 1; continue; }
        if data[*pos] == b'%' {
            while *pos < data.len() && data[*pos] != b'\n' { *pos += 1; }
            continue;
        }
        break;
    }
}

fn is_delim_or_ws(b: u8) -> bool {
    b.is_ascii_whitespace() || matches!(b, b'(' | b')' | b'<' | b'>' | b'[' | b']' | b'{' | b'}' | b'/' | b'%')
}

fn decode_name(raw: &[u8]) -> String {
    let mut out = Vec::new();
    let mut i = 0;
    while i < raw.len() {
        if raw[i] == b'#' && i + 2 < raw.len() {
            out.push((hv(raw[i+1]) << 4) | hv(raw[i+2]));
            i += 3;
        } else {
            out.push(raw[i]);
            i += 1;
        }
    }
    String::from_utf8(out).unwrap_or_default()
}

fn hv(b: u8) -> u8 {
    match b {
        b'0'..=b'9' => b - b'0',
        b'a'..=b'f' => b - b'a' + 10,
        b'A'..=b'F' => b - b'A' + 10,
        _ => 0,
    }
}
