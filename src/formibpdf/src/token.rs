#![allow(dead_code)]

#[derive(Debug, Clone, PartialEq)]
pub enum Token {
    Integer(i64),
    Real(f64),
    Name(String),
    StringLit(Vec<u8>),
    ArrayBegin,
    ArrayEnd,
    Operator(String),
    /// Raw binary payload of an inline image, scanned between `ID` and `EI`.
    InlineImageData(Vec<u8>),
}

pub struct Tokenizer<'a> {
    data: &'a [u8],
    pos: usize,
    /// Lookahead slot: holds at most one pre-scanned token (used for
    /// InlineImageData that is produced as a side-effect of the `ID` operator).
    lookahead: Option<Token>,
}

impl<'a> Tokenizer<'a> {
    pub fn new(data: &'a [u8]) -> Self {
        Self { data, pos: 0, lookahead: None }
    }

    pub fn next_token(&mut self) -> Option<Token> {
        // Drain lookahead first (set by ID handling).
        if let Some(t) = self.lookahead.take() {
            return Some(t);
        }

        self.skip_ws();
        if self.pos >= self.data.len() { return None; }

        match self.data[self.pos] {
            b'[' => { self.pos += 1; Some(Token::ArrayBegin) }
            b']' => { self.pos += 1; Some(Token::ArrayEnd) }
            b'(' => { self.pos += 1; Some(self.read_string_literal()) }
            b'<' => {
                self.pos += 1;
                if self.pos < self.data.len() && self.data[self.pos] == b'<' {
                    self.pos += 1; // skip <<, continue
                    return self.next_token();
                }
                Some(self.read_hex_string())
            }
            b'>' => {
                self.pos += 1;
                if self.pos < self.data.len() && self.data[self.pos] == b'>' {
                    self.pos += 1;
                }
                self.next_token()
            }
            b'/' => { self.pos += 1; Some(self.read_name()) }
            b'+' | b'-' | b'.' | b'0'..=b'9' => Some(self.read_number()),
            b if b.is_ascii_alphabetic() || b == b'*' => {
                let tok = self.read_keyword();
                // After emitting the ID operator, immediately scan inline image
                // data and stash it in the lookahead slot.  The caller will see:
                //   Operator("ID")  then  InlineImageData(bytes)
                // The EI marker is consumed by the scanner, so the next real
                // token after InlineImageData will be whatever follows EI.
                if tok == Token::Operator("ID".to_string()) {
                    let data = self.scan_inline_image_data();
                    self.lookahead = Some(Token::InlineImageData(data));
                }
                Some(tok)
            }
            _ => { self.pos += 1; self.next_token() }
        }
    }

    // ── Inline image data scanner ─────────────────────────────────────────────

    /// Scan forward from the current position until the `EI` marker preceded
    /// by whitespace (or at the start of the stream).  Returns all bytes
    /// between `ID` (exclusive) and `EI` (exclusive).  Positions `self.pos`
    /// just after the consumed `EI` token.
    fn scan_inline_image_data(&mut self) -> Vec<u8> {
        let mut buf: Vec<u8> = Vec::new();
        let data = self.data;
        let mut pos = self.pos;

        loop {
            if pos >= data.len() { break; }

            // Look for whitespace-preceded "EI" followed by whitespace or end.
            // The preceding byte check uses the byte just before pos in buf
            // (or in the original stream if buf is still empty).
            if pos + 1 < data.len() && data[pos] == b'E' && data[pos + 1] == b'I' {
                let prev_ws = if pos == self.pos {
                    // Nothing read yet — start of data after ID counts as boundary.
                    true
                } else {
                    is_pdf_whitespace(*buf.last().unwrap_or(&b' '))
                };
                let after_ws = data.get(pos + 2)
                    .map(|&b| is_pdf_whitespace(b) || b == b'Q' || b == b'q')
                    .unwrap_or(true); // end-of-stream counts as whitespace
                if prev_ws && after_ws {
                    // Consume "EI"
                    self.pos = pos + 2;
                    return buf;
                }
            }

            buf.push(data[pos]);
            pos += 1;
        }

        self.pos = pos;
        buf
    }

    // ── Private helpers ───────────────────────────────────────────────────────

    fn skip_ws(&mut self) {
        while self.pos < self.data.len() {
            let b = self.data[self.pos];
            if b.is_ascii_whitespace() { self.pos += 1; continue; }
            if b == b'%' {
                self.pos += 1;
                while self.pos < self.data.len() {
                    let c = self.data[self.pos];
                    self.pos += 1;
                    if c == b'\n' || c == b'\r' { break; }
                }
                continue;
            }
            break;
        }
    }

    fn read_number(&mut self) -> Token {
        let start = self.pos;
        if self.pos < self.data.len() && matches!(self.data[self.pos], b'+' | b'-') {
            self.pos += 1;
        }
        while self.pos < self.data.len() {
            match self.data[self.pos] {
                b'0'..=b'9' | b'.' | b'e' | b'E' => self.pos += 1,
                _ => break,
            }
        }
        let s = std::str::from_utf8(&self.data[start..self.pos]).unwrap_or("0");
        if s.contains('.') || s.contains('e') || s.contains('E') {
            Token::Real(s.parse().unwrap_or(0.0))
        } else {
            Token::Integer(s.parse().unwrap_or(0))
        }
    }

    fn read_name(&mut self) -> Token {
        let start = self.pos;
        while self.pos < self.data.len() {
            let b = self.data[self.pos];
            if b.is_ascii_whitespace() || Self::is_delim(b) { break; }
            self.pos += 1;
        }
        Token::Name(String::from_utf8(self.data[start..self.pos].to_vec()).unwrap_or_default())
    }

    fn read_string_literal(&mut self) -> Token {
        let mut depth = 1i32;
        let mut buf = Vec::new();
        while self.pos < self.data.len() && depth > 0 {
            match self.data[self.pos] {
                b'\\' => {
                    self.pos += 1;
                    if self.pos < self.data.len() {
                        let esc = self.data[self.pos];
                        buf.push(match esc {
                            b'n' => b'\n', b'r' => b'\r', b't' => b'\t',
                            b'b' => 0x08, b'f' => 0x0C,
                            _ => esc,
                        });
                        self.pos += 1;
                    }
                }
                b'(' => { depth += 1; buf.push(b'('); self.pos += 1; }
                b')' => {
                    depth -= 1;
                    self.pos += 1;
                    if depth > 0 { buf.push(b')'); }
                }
                b => { buf.push(b); self.pos += 1; }
            }
        }
        Token::StringLit(buf)
    }

    fn read_hex_string(&mut self) -> Token {
        let mut hex = Vec::new();
        while self.pos < self.data.len() {
            let b = self.data[self.pos];
            if b == b'>' { self.pos += 1; break; }
            if !b.is_ascii_whitespace() { hex.push(b); }
            self.pos += 1;
        }
        if hex.len() % 2 == 1 { hex.push(b'0'); }
        let out = hex.chunks_exact(2)
            .map(|c| (Self::hex_val(c[0]) << 4) | Self::hex_val(c[1]))
            .collect();
        Token::StringLit(out)
    }

    fn read_keyword(&mut self) -> Token {
        let start = self.pos;
        while self.pos < self.data.len() {
            let b = self.data[self.pos];
            if b.is_ascii_alphabetic() || b == b'*' { self.pos += 1; } else { break; }
        }
        Token::Operator(String::from_utf8(self.data[start..self.pos].to_vec()).unwrap_or_default())
    }

    fn is_delim(b: u8) -> bool {
        matches!(b, b'[' | b']' | b'(' | b')' | b'<' | b'>' | b'{' | b'}' | b'/' | b'%')
    }

    fn hex_val(b: u8) -> u8 {
        match b {
            b'0'..=b'9' => b - b'0',
            b'a'..=b'f' => 10 + b - b'a',
            b'A'..=b'F' => 10 + b - b'A',
            _ => 0,
        }
    }
}

#[inline]
fn is_pdf_whitespace(b: u8) -> bool {
    matches!(b, b' ' | b'\t' | b'\r' | b'\n' | 0x0C | 0x00)
}
