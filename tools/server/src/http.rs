//! The HTTP this server needs and nothing else: GET, POST, one connection at a time,
//! Content-Length bodies. Parsing and the access rules are pure, so the part that decides
//! whether a request may run a console command is tested on any machine.

/// A request line and its headers. The body is read separately, once Content-Length is
/// known.
#[derive(Debug, Clone)]
pub struct Head {
    pub method: String,
    pub path: String,
    pub query: String,
    pub headers: Vec<(String, String)>,
}

impl Head {
    /// Header names are case-insensitive and are stored lower-cased on the way in.
    pub fn header(&self, name: &str) -> Option<&str> {
        self.headers
            .iter()
            .find(|(k, _)| k == name)
            .map(|(_, v)| v.as_str())
    }

    pub fn content_length(&self) -> usize {
        self.header("content-length")
            .and_then(|v| v.trim().parse().ok())
            .unwrap_or(0)
    }
}

/// Everything up to the blank line. None if it is not a request at all.
pub fn parse_head(text: &str) -> Option<Head> {
    let mut lines = text.split("\r\n");
    let request_line = lines.next()?;
    let mut parts = request_line.split(' ');
    let method = parts.next()?.to_string();
    let target = parts.next()?;
    // The version is not checked. Anything that gets this far is either our own page or a
    // tool on this machine, and refusing HTTP/1.0 would only ever surprise someone.
    parts.next()?;

    let (path, query) = match target.split_once('?') {
        Some((p, q)) => (p.to_string(), q.to_string()),
        None => (target.to_string(), String::new()),
    };

    let mut headers = Vec::new();
    for line in lines {
        if line.is_empty() {
            break;
        }
        if let Some((k, v)) = line.split_once(':') {
            headers.push((k.trim().to_ascii_lowercase(), v.trim().to_string()));
        }
    }

    Some(Head {
        method,
        path,
        query,
        headers,
    })
}

/// One `name=value` out of a query string, percent-decoded.
pub fn query_param(query: &str, name: &str) -> Option<String> {
    for pair in query.split('&') {
        if let Some((k, v)) = pair.split_once('=') {
            if k == name {
                return Some(percent_decode(v));
            }
        } else if pair == name {
            return Some(String::new());
        }
    }
    None
}

fn percent_decode(s: &str) -> String {
    let bytes = s.as_bytes();
    let mut out: Vec<u8> = Vec::with_capacity(bytes.len());
    let mut i = 0;
    while i < bytes.len() {
        match bytes[i] {
            b'%' if i + 2 < bytes.len() => {
                let hex = std::str::from_utf8(&bytes[i + 1..i + 3]).unwrap_or("");
                match u8::from_str_radix(hex, 16) {
                    Ok(byte) => {
                        out.push(byte);
                        i += 3;
                    }
                    Err(_) => {
                        out.push(bytes[i]);
                        i += 1;
                    }
                }
            }
            b'+' => {
                out.push(b' ');
                i += 1;
            }
            byte => {
                out.push(byte);
                i += 1;
            }
        }
    }
    String::from_utf8_lossy(&out).into_owned()
}

/// Why a request was not allowed to do anything.
#[derive(Debug, Clone, PartialEq)]
pub enum Denial {
    /// Not addressed to this machine by its loopback name.
    Host(String),
    /// Sent by a web page that is not ours.
    Origin(String),
    /// A plain cross-site POST, which a browser can make without asking anyone.
    MissingHeader,
}

impl Denial {
    pub fn message(&self) -> String {
        match self {
            Denial::Host(host) => format!(
                "Host header is {host:?}. This server answers only to its own loopback address."
            ),
            Denial::Origin(origin) => format!(
                "Origin {origin:?} is not this server. A page served from somewhere else may not \
                 drive the game."
            ),
            Denial::MissingHeader => {
                "Requests that act need the header `X-Cs2Vr: 1`. It is there to force a \
                 cross-origin preflight, so a page in the browser cannot post a command \
                 without asking first."
                    .to_string()
            }
        }
    }
}

/// The header that has to be there on anything that acts.
pub const ACT_HEADER: &str = "x-cs2vr";

fn loopback_names(port: u16) -> [String; 3] {
    [
        format!("127.0.0.1:{port}"),
        format!("localhost:{port}"),
        format!("[::1]:{port}"),
    ]
}

/// May this request do what it is asking for?
///
/// The socket is on 127.0.0.1 and the hook's pipe is ACL'd to one user, but neither of
/// those keeps out the attack that actually applies here: any page the operator happens to
/// have open can POST to http://127.0.0.1:8731/command, and with a plain text body that is
/// a "simple request" - no preflight, so the page never has to be told no. This endpoint
/// runs arbitrary console commands inside a running game and deliberately has no allowlist,
/// so two conditions close that door:
///
///   - the Host has to be the loopback address we bound, which stops a name that resolves
///     to 127.0.0.1 from being used to dress a remote page up as a local one;
///   - anything that acts has to carry a header no simple request may carry, which forces
///     a preflight the browser will not complete because we answer no CORS headers at all.
///
/// Reads are left open: a cross-origin page can send a GET but cannot read the answer.
pub fn guard(head: &Head, port: u16) -> Result<(), Denial> {
    let names = loopback_names(port);
    let host = head.header("host").unwrap_or("");
    if !names.iter().any(|n| n == host) {
        return Err(Denial::Host(host.to_string()));
    }

    if let Some(origin) = head.header("origin") {
        let ours = names.iter().any(|n| {
            origin == format!("http://{n}") || origin == format!("https://{n}")
        });
        if !ours {
            return Err(Denial::Origin(origin.to_string()));
        }
    }

    let acts = "GET" != head.method && "HEAD" != head.method;
    if acts && head.header(ACT_HEADER).is_none() {
        return Err(Denial::MissingHeader);
    }

    Ok(())
}

/// A complete response, ready for the socket.
pub fn response(status: u16, content_type: &str, body: &str) -> Vec<u8> {
    let reason = match status {
        200 => "OK",
        400 => "Bad Request",
        403 => "Forbidden",
        404 => "Not Found",
        405 => "Method Not Allowed",
        500 => "Internal Server Error",
        503 => "Service Unavailable",
        _ => "Status",
    };
    let mut out = format!(
        "HTTP/1.1 {status} {reason}\r\n\
         Content-Type: {content_type}\r\n\
         Content-Length: {}\r\n\
         Cache-Control: no-store\r\n\
         X-Content-Type-Options: nosniff\r\n\
         Connection: close\r\n\r\n",
        body.as_bytes().len()
    )
    .into_bytes();
    out.extend_from_slice(body.as_bytes());
    out
}

/// An error as JSON, because both clients of this server read JSON.
pub fn error(status: u16, message: &str) -> Vec<u8> {
    let mut o = crate::json::Object::new();
    o.str("error", message);
    response(status, "application/json; charset=utf-8", &o.finish())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn head(text: &str) -> Head {
        parse_head(text).unwrap()
    }

    #[test]
    fn reads_a_request() {
        let h = head("GET /log?since=42 HTTP/1.1\r\nHost: 127.0.0.1:8731\r\nAccept: */*\r\n\r\n");
        assert_eq!("GET", h.method);
        assert_eq!("/log", h.path);
        assert_eq!("since=42", h.query);
        assert_eq!(Some("127.0.0.1:8731"), h.header("host"));
        assert_eq!(Some("42"), query_param(&h.query, "since").as_deref());
        assert_eq!(None, query_param(&h.query, "wait"));
    }

    #[test]
    fn header_names_are_case_insensitive() {
        // curl, a browser and PowerShell all capitalise differently, and a server that
        // only knew one spelling would refuse one of the three clients this has.
        let h = head("POST /command HTTP/1.1\r\nHOST: 127.0.0.1:8731\r\nContent-Length: 14\r\n\r\n");
        assert_eq!(Some("127.0.0.1:8731"), h.header("host"));
        assert_eq!(14, h.content_length());
    }

    #[test]
    fn decodes_what_a_form_puts_in_a_query() {
        assert_eq!(
            Some("mirv_vr_panel spread 1.2".to_string()),
            query_param("line=mirv_vr_panel+spread+1.2", "line")
        );
        assert_eq!(
            Some("a/b c".to_string()),
            query_param("x=a%2Fb%20c", "x")
        );
    }

    #[test]
    fn a_body_with_no_content_length_is_empty_rather_than_a_guess() {
        let h = head("POST /command HTTP/1.1\r\nHost: 127.0.0.1:8731\r\n\r\n");
        assert_eq!(0, h.content_length());
    }

    #[test]
    fn our_own_page_is_allowed() {
        let h = head("GET /state HTTP/1.1\r\nHost: 127.0.0.1:8731\r\nOrigin: http://127.0.0.1:8731\r\n\r\n");
        assert_eq!(Ok(()), guard(&h, 8731));
    }

    #[test]
    fn an_agent_with_the_header_is_allowed_to_act() {
        let h = head("POST /command HTTP/1.1\r\nHost: localhost:8731\r\nX-Cs2Vr: 1\r\n\r\n");
        assert_eq!(Ok(()), guard(&h, 8731));
    }

    #[test]
    fn a_plain_cross_site_post_is_refused() {
        // This is the whole reason the header exists: a form post from any page the
        // operator has open is a simple request, so nothing else would have stopped it.
        let h = head("POST /command HTTP/1.1\r\nHost: 127.0.0.1:8731\r\n\r\n");
        assert_eq!(Err(Denial::MissingHeader), guard(&h, 8731));
    }

    #[test]
    fn another_page_may_not_drive_the_game() {
        let h = head("POST /command HTTP/1.1\r\nHost: 127.0.0.1:8731\r\nOrigin: https://example.invalid\r\nX-Cs2Vr: 1\r\n\r\n");
        assert_eq!(
            Err(Denial::Origin("https://example.invalid".to_string())),
            guard(&h, 8731)
        );
    }

    #[test]
    fn a_name_that_merely_resolves_here_is_refused() {
        // DNS rebinding: the socket is on 127.0.0.1, but the browser will happily send a
        // request for any hostname that resolves to it.
        let h = head("GET /state HTTP/1.1\r\nHost: rebind.example.invalid:8731\r\n\r\n");
        assert!(matches!(guard(&h, 8731), Err(Denial::Host(_))));
        let missing = head("GET /state HTTP/1.1\r\n\r\n");
        assert!(matches!(guard(&missing, 8731), Err(Denial::Host(_))));
    }

    #[test]
    fn the_port_is_part_of_the_answer() {
        let h = head("GET /state HTTP/1.1\r\nHost: 127.0.0.1:8731\r\n\r\n");
        assert!(matches!(guard(&h, 9000), Err(Denial::Host(_))));
    }

    #[test]
    fn a_response_says_how_long_it_is_in_bytes() {
        let body = "{\"map\":\"карта\"}";
        let bytes = response(200, "application/json", body);
        let text = String::from_utf8_lossy(&bytes);
        assert!(text.starts_with("HTTP/1.1 200 OK\r\n"));
        // Bytes, not characters. Cyrillic in a demo name used to truncate the answer.
        assert!(text.contains(&format!("Content-Length: {}\r\n", body.len())));
        assert!(text.ends_with(body));
    }
}
