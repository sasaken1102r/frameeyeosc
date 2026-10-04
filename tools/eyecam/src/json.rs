//! A minimal JSON reader for eyecam's own files (calib.json): objects, arrays, numbers, strings, booleans, null.

#[derive(Clone, Debug, PartialEq)]
pub enum Json {
    Null,
    Bool(bool),
    Num(f64),
    Str(String),
    Arr(Vec<Json>),
    Obj(Vec<(String, Json)>),
}

impl Json {
    pub fn get(&self, key: &str) -> Option<&Json> {
        match self {
            Json::Obj(items) => items.iter().find(|(k, _)| k == key).map(|(_, v)| v),
            _ => None,
        }
    }

    pub fn num(&self) -> Option<f64> {
        match self {
            Json::Num(v) => Some(*v),
            _ => None,
        }
    }

    pub fn str(&self) -> Option<&str> {
        match self {
            Json::Str(s) => Some(s),
            _ => None,
        }
    }

    pub fn arr(&self) -> Option<&[Json]> {
        match self {
            Json::Arr(v) => Some(v),
            _ => None,
        }
    }
}

struct P<'a> {
    s: &'a [u8],
    i: usize,
}

impl P<'_> {
    fn ws(&mut self) {
        while self.i < self.s.len() && self.s[self.i].is_ascii_whitespace() {
            self.i += 1;
        }
    }

    fn eat(&mut self, c: u8) -> Result<(), String> {
        self.ws();
        if self.s.get(self.i) == Some(&c) {
            self.i += 1;
            Ok(())
        } else {
            Err(format!("expected '{}' at {}", c as char, self.i))
        }
    }

    fn value(&mut self) -> Result<Json, String> {
        self.ws();
        match self.s.get(self.i) {
            Some(b'{') => {
                self.i += 1;
                let mut items = Vec::new();
                self.ws();
                if self.s.get(self.i) == Some(&b'}') {
                    self.i += 1;
                    return Ok(Json::Obj(items));
                }
                loop {
                    let Json::Str(k) = self.value()? else { return Err("object key must be a string".into()) };
                    self.eat(b':')?;
                    items.push((k, self.value()?));
                    self.ws();
                    match self.s.get(self.i) {
                        Some(b',') => self.i += 1,
                        Some(b'}') => {
                            self.i += 1;
                            return Ok(Json::Obj(items));
                        }
                        _ => return Err(format!("bad object at {}", self.i)),
                    }
                }
            }
            Some(b'[') => {
                self.i += 1;
                let mut items = Vec::new();
                self.ws();
                if self.s.get(self.i) == Some(&b']') {
                    self.i += 1;
                    return Ok(Json::Arr(items));
                }
                loop {
                    items.push(self.value()?);
                    self.ws();
                    match self.s.get(self.i) {
                        Some(b',') => self.i += 1,
                        Some(b']') => {
                            self.i += 1;
                            return Ok(Json::Arr(items));
                        }
                        _ => return Err(format!("bad array at {}", self.i)),
                    }
                }
            }
            Some(b'"') => {
                self.i += 1;
                let mut out = Vec::new();
                while let Some(&c) = self.s.get(self.i) {
                    self.i += 1;
                    match c {
                        b'"' => return String::from_utf8(out).map(Json::Str).map_err(|e| e.to_string()),
                        b'\\' => {
                            let e = *self.s.get(self.i).ok_or("bad escape")?;
                            self.i += 1;
                            match e {
                                b'n' => out.push(b'\n'),
                                b't' => out.push(b'\t'),
                                b'u' => {
                                    let hex = std::str::from_utf8(self.s.get(self.i..self.i + 4).ok_or("bad \\u")?)
                                        .map_err(|e| e.to_string())?;
                                    let cp = u32::from_str_radix(hex, 16).map_err(|e| e.to_string())?;
                                    self.i += 4;
                                    let ch = char::from_u32(cp).unwrap_or('?');
                                    out.extend_from_slice(ch.to_string().as_bytes());
                                }
                                other => out.push(other),
                            }
                        }
                        _ => out.push(c),
                    }
                }
                Err("unterminated string".into())
            }
            Some(b't') if self.s[self.i..].starts_with(b"true") => {
                self.i += 4;
                Ok(Json::Bool(true))
            }
            Some(b'f') if self.s[self.i..].starts_with(b"false") => {
                self.i += 5;
                Ok(Json::Bool(false))
            }
            Some(b'n') if self.s[self.i..].starts_with(b"null") => {
                self.i += 4;
                Ok(Json::Null)
            }
            Some(_) => {
                let start = self.i;
                while self.i < self.s.len() && matches!(self.s[self.i], b'-' | b'+' | b'.' | b'e' | b'E' | b'0'..=b'9') {
                    self.i += 1;
                }
                let t = std::str::from_utf8(&self.s[start..self.i]).map_err(|e| e.to_string())?;
                t.parse().map(Json::Num).map_err(|_| format!("bad value at {start}"))
            }
            None => Err("unexpected end".into()),
        }
    }
}

pub fn parse(text: &str) -> Result<Json, String> {
    let mut p = P { s: text.as_bytes(), i: 0 };
    let v = p.value()?;
    p.ws();
    if p.i != p.s.len() {
        return Err(format!("trailing data at {}", p.i));
    }
    Ok(v)
}

/// A JSON string literal.
pub fn string(s: &str) -> String {
    let mut out = String::with_capacity(s.len() + 2);
    out.push('"');
    for c in s.chars() {
        match c {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            c if (c as u32) < 0x20 => out.push_str(&format!("\\u{:04x}", c as u32)),
            c => out.push(c),
        }
    }
    out.push('"');
    out
}

/// A JSON number (null for non-finite values).
pub fn num(v: f64) -> String {
    if v.is_finite() { format!("{v}") } else { "null".into() }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_nested_values() {
        let j = parse(r#"{"a": 1.5, "b": [1, -2e3], "c": {"d": "x\"y", "e": true, "f": null}}"#).unwrap();
        assert_eq!(j.get("a").and_then(Json::num), Some(1.5));
        assert_eq!(j.get("b").and_then(Json::arr).map(|a| a.len()), Some(2));
        assert_eq!(j.get("c").and_then(|c| c.get("d")).and_then(Json::str), Some("x\"y"));
        assert!(parse("{\"a\": }").is_err());
        assert_eq!(num(f64::NAN), "null");
    }
}
