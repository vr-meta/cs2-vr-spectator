//! Just enough JSON to answer with, written by hand.
//!
//! Sixty lines against a dependency tree inside a process that writes console commands
//! into a running game. Only the writing side exists: nothing here parses JSON, because
//! nothing needs to - POST /command takes one plain console line as its whole body.

/// A string as a JSON string literal, quotes included.
pub fn quote(s: &str) -> String {
    let mut out = String::with_capacity(s.len() + 2);
    out.push('"');
    for c in s.chars() {
        match c {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            // Everything below a space has to be escaped or the document is invalid. The
            // log is game output and has carried stray control bytes before.
            c if (c as u32) < 0x20 => out.push_str(&format!("\\u{:04x}", c as u32)),
            c => out.push(c),
        }
    }
    out.push('"');
    out
}

/// A number, or `null` where JSON cannot say what a float can. A NaN frame rate written
/// literally makes the whole document unparseable at the far end, which presents as a
/// dashboard that went blank rather than as a bad number.
pub fn number(v: f64) -> String {
    if v.is_finite() {
        let rounded = format!("{v:.2}");
        rounded
            .trim_end_matches('0')
            .trim_end_matches('.')
            .to_string()
    } else {
        "null".to_string()
    }
}

/// An object, built field by field in the order they are added.
pub struct Object {
    out: String,
}

impl Object {
    pub fn new() -> Object {
        Object {
            out: "{".to_string(),
        }
    }

    fn key(&mut self, name: &str) {
        if 1 < self.out.len() {
            self.out.push(',');
        }
        self.out.push_str(&quote(name));
        self.out.push(':');
    }

    /// A value that is already JSON: a nested object, an array, `true`, `null`.
    pub fn raw(&mut self, name: &str, value: &str) -> &mut Object {
        self.key(name);
        self.out.push_str(value);
        self
    }

    pub fn str(&mut self, name: &str, value: &str) -> &mut Object {
        self.raw(name, &quote(value))
    }

    pub fn opt_str(&mut self, name: &str, value: Option<&str>) -> &mut Object {
        match value {
            Some(v) => self.str(name, v),
            None => self.raw(name, "null"),
        }
    }

    pub fn num(&mut self, name: &str, value: f64) -> &mut Object {
        self.raw(name, &number(value))
    }

    pub fn opt_num(&mut self, name: &str, value: Option<f64>) -> &mut Object {
        match value {
            Some(v) => self.num(name, v),
            None => self.raw(name, "null"),
        }
    }

    pub fn bool(&mut self, name: &str, value: bool) -> &mut Object {
        self.raw(name, if value { "true" } else { "false" })
    }

    /// `2528x2780` as a two-element array, or null. The web page wants both numbers and
    /// the operator reads it as one size, so it is written the way it is read.
    pub fn opt_size(&mut self, name: &str, value: Option<(u32, u32)>) -> &mut Object {
        match value {
            Some((w, h)) => self.raw(name, &format!("[{w},{h}]")),
            None => self.raw(name, "null"),
        }
    }

    pub fn finish(&self) -> String {
        let mut out = self.out.clone();
        out.push('}');
        out
    }
}

impl Default for Object {
    fn default() -> Object {
        Object::new()
    }
}

/// An array of already-encoded values.
pub fn array(items: &[String]) -> String {
    format!("[{}]", items.join(","))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn quotes_what_would_otherwise_break_the_document() {
        assert_eq!("\"plain\"", quote("plain"));
        assert_eq!("\"say \\\"no\\\"\"", quote("say \"no\""));
        // The pipe name, which is the single most likely string to appear in the log and
        // is nothing but backslashes.
        assert_eq!("\"\\\\\\\\.\\\\pipe\\\\cs2vr\"", quote("\\\\.\\pipe\\cs2vr"));
        assert_eq!("\"a\\nb\"", quote("a\nb"));
        assert_eq!("\"\\u0007\"", quote("\u{7}"));
    }

    #[test]
    fn keeps_non_ascii_as_it_is() {
        // The log is UTF-8 and a demo file name can be anything; escaping it would be
        // valid JSON but unreadable in the console pane.
        assert_eq!("\"карта\"", quote("карта"));
    }

    #[test]
    fn writes_numbers_a_page_can_read_back() {
        assert_eq!("72", number(72.0));
        assert_eq!("71.8", number(71.8));
        assert_eq!("0", number(0.0));
        assert_eq!("null", number(f64::NAN));
        assert_eq!("null", number(f64::INFINITY));
    }

    #[test]
    fn builds_an_object_in_order() {
        let mut o = Object::new();
        o.str("mode", "playing")
            .opt_str("map", Some("de_inferno"))
            .opt_str("session_state", None)
            .bool("cs2_running", true)
            .opt_num("fps", Some(71.8))
            .opt_size("per_eye", Some((2528, 2780)))
            .opt_size("back_buffer", None)
            .raw("lines", &array(&[quote("one"), quote("two")]));
        assert_eq!(
            "{\"mode\":\"playing\",\"map\":\"de_inferno\",\"session_state\":null,\
             \"cs2_running\":true,\"fps\":71.8,\"per_eye\":[2528,2780],\
             \"back_buffer\":null,\"lines\":[\"one\",\"two\"]}",
            o.finish()
        );
    }

    #[test]
    fn an_empty_object_is_still_an_object() {
        assert_eq!("{}", Object::new().finish());
        assert_eq!("[]", array(&[]));
    }
}
