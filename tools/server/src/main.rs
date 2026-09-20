//! A control server for a session nobody can reach.
//!
//! A worn launch has no console: the window is 2528x2780 clamped onto a 2560x1600 display
//! and Panorama puts the console's input line below the bottom of the screen. So a session
//! is adjusted by writing single lines into \\.\pipe\cs2vr and observed by reading
//! game/csgo/console.log. Both of those are already there; this turns the pair into one
//! HTTP API, so that a person on a web page and an agent driving the game do the same
//! thing through the same door instead of through six lines of PowerShell each time.
//!
//! What it does NOT do is start the game. Launching means process creation, a remote
//! thread and DLL injection, and that is cs2vr.exe's job and nobody else's.

mod afxvr;
mod cs2;
mod http;
mod json;
mod logtail;
mod paths;
mod pipe;
mod vdf;

use std::io::{Read, Write};
use std::net::{TcpListener, TcpStream};
use std::path::PathBuf;
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

const DEFAULT_PORT: u16 = 8731;
/// Often enough that the console pane feels live, seldom enough that following a 159 KB
/// file costs nothing measurable next to a game rendering three passes a frame.
const POLL: Duration = Duration::from_millis(150);

const USAGE: &str = "\
cs2vr-server - drive and watch a running cs2-vr-spectator session over HTTP.

  --port <n>      listen on 127.0.0.1:<n>. Default 8731.
  --cs2 <folder>  where CS2 is, if the game is not running and Steam cannot be asked.
  --log <file>    console.log itself, if it is somewhere unusual.
  --help

It attaches to a session that is already running and never starts one.
";

struct Args {
    port: u16,
    csgo: Option<String>,
    log: Option<String>,
}

fn parse_args() -> Result<Args, String> {
    let mut args = Args {
        port: DEFAULT_PORT,
        csgo: None,
        log: None,
    };
    let mut argv = std::env::args().skip(1);
    while let Some(arg) = argv.next() {
        let mut value = || {
            argv.next()
                .ok_or_else(|| format!("{arg} needs a value after it"))
        };
        match arg.as_str() {
            "--port" => {
                let text = value()?;
                args.port = text
                    .parse()
                    .map_err(|_| format!("--port {text} is not a port number"))?;
            }
            "--cs2" => args.csgo = Some(paths::csgo_from_folder(&value()?)),
            "--log" => args.log = Some(value()?),
            "--help" | "-h" => {
                print!("{USAGE}");
                std::process::exit(0);
            }
            other => return Err(format!("unknown argument {other}. --help lists them.")),
        }
    }
    Ok(args)
}

/// What the two threads share: one follower, and what is known about the game around it.
struct Shared {
    follower: logtail::Follower,
    log_path: Option<PathBuf>,
    /// Why there is no log to follow, in words rather than as an absence.
    trouble: Option<String>,
    session: Option<cs2::Session>,
    cs2_build_installed: Option<String>,
}

fn main() {
    let args = match parse_args() {
        Ok(args) => args,
        Err(message) => {
            eprintln!("cs2vr-server: {message}");
            std::process::exit(2);
        }
    };

    let shared = Arc::new(Mutex::new(Shared {
        follower: logtail::Follower::new(),
        log_path: None,
        trouble: None,
        session: None,
        cs2_build_installed: None,
    }));

    // Bound to loopback, and that is not a detail of the deployment. Every request that
    // gets through here runs an arbitrary console command inside a running game; the
    // hook's pipe is ACL'd to this one user precisely so that only this user can do that,
    // and a socket on 0.0.0.0 would hand the same power to anyone on the network. The
    // rest of the door is in http::guard - loopback alone does not keep a web page out.
    let listener = match TcpListener::bind(("127.0.0.1", args.port)) {
        Ok(listener) => listener,
        Err(error) => {
            // Never quietly pick another port: an agent needs a known address, and a
            // dashboard that moved is worse than one that did not start.
            eprintln!(
                "cs2vr-server: cannot listen on 127.0.0.1:{} ({error}). Another copy is \
                 probably already running; stop it, or pass --port.",
                args.port
            );
            std::process::exit(1);
        }
    };

    let start = Instant::now();
    {
        let shared = Arc::clone(&shared);
        let fixed_log = args.log.clone();
        let fixed_csgo = args.csgo.clone();
        std::thread::spawn(move || follow(shared, fixed_log, fixed_csgo, start));
    }

    println!("cs2vr-server on http://127.0.0.1:{}", args.port);
    println!("  GET  /state        what the session is doing");
    println!("  GET  /log?since=N  the AFXVR lines after N");
    println!("  POST /command      one console line, with `X-Cs2Vr: 1`");
    println!("It never starts the game. Use cs2vr.exe for that.");

    for stream in listener.incoming() {
        match stream {
            Ok(stream) => {
                let shared = Arc::clone(&shared);
                // A thread each, because POST /command waits for the game to answer and
                // must not hold up the page's polling while it does.
                std::thread::spawn(move || {
                    let _ = serve(stream, shared, args.port, start);
                });
            }
            Err(error) => eprintln!("cs2vr-server: {error}"),
        }
    }
}

fn now_ms(start: Instant) -> u64 {
    start.elapsed().as_millis() as u64
}

/// The thread that reads console.log and keeps up with which game is running.
fn follow(
    shared: Arc<Mutex<Shared>>,
    fixed_log: Option<String>,
    fixed_csgo: Option<String>,
    start: Instant,
) {
    let mut last_look = Instant::now() - Duration::from_secs(60);
    loop {
        // Looking for the game is a process-list walk, so it happens on its own slower
        // clock than reading the bytes a file gained.
        if Duration::from_secs(2) < last_look.elapsed() {
            last_look = Instant::now();
            locate(&shared, fixed_log.as_deref(), fixed_csgo.as_deref());
        }

        let mut more_to_read = false;
        let (path, offset) = {
            let guard = shared.lock().unwrap();
            (guard.log_path.clone(), guard.follower.offset())
        };

        if let Some(path) = path {
            // Read with no lock held. Holding it across the read and the parse is what
            // made a request that asked for 1200 ms take 16933 while a long log was being
            // folded for the first time.
            let chunk = logtail::read_chunk(&path, offset);
            let now = now_ms(start);
            let mut guard = shared.lock().unwrap();
            match chunk {
                Ok(logtail::Chunk::Nothing) => guard.trouble = None,
                Ok(logtail::Chunk::Relaunched) => {
                    guard.follower.relaunched();
                    guard.trouble = None;
                }
                Ok(logtail::Chunk::Bytes { from, bytes }) => {
                    more_to_read = logtail::CHUNK == bytes.len();
                    guard.follower.absorb(from, &bytes, now);
                    guard.trouble = None;
                }
                Err(error) => {
                    guard.trouble = Some(format!("{} cannot be read: {error}", path.display()));
                }
            }
        }

        // Catching up on a backlog should not be paced at the idle rate; only wait when
        // the file has actually been drained.
        if !more_to_read {
            std::thread::sleep(POLL);
        }
    }
}

/// Which console.log to follow. The running game wins over anything looked up, because it
/// is the session actually being watched.
fn locate(shared: &Arc<Mutex<Shared>>, fixed_log: Option<&str>, fixed_csgo: Option<&str>) {
    let session = cs2::running();

    let csgo = match (fixed_csgo, &session) {
        (Some(csgo), _) => Some(csgo.to_string()),
        (None, Some(session)) => paths::csgo_from_exe(&session.exe),
        (None, None) => cs2::csgo_through_steam(),
    };

    let log = match fixed_log {
        Some(log) => Some(PathBuf::from(log)),
        None => csgo
            .as_ref()
            .map(|csgo| PathBuf::from(paths::console_log_in(csgo))),
    };

    let mut guard = shared.lock().unwrap();
    guard.session = session;

    if guard.cs2_build_installed.is_none() {
        if let Some(csgo) = &csgo {
            guard.cs2_build_installed = cs2::installed_build(csgo);
        }
    }

    match &log {
        Some(path) if !path.exists() => {
            guard.trouble = Some(format!(
                "{} does not exist yet. CS2 writes it with -condebug, which cs2vr.exe passes.",
                path.display()
            ));
        }
        None => {
            guard.trouble = Some(
                "CS2 is not running and Steam could not say where it is. Pass --cs2 <folder> \
                 or --log <file>."
                    .to_string(),
            );
        }
        _ => {}
    }

    // Changing files means changing sessions; everything folded from the old one is about
    // a game that is not this one.
    if guard.log_path != log {
        guard.log_path = log;
        guard.follower = logtail::Follower::new();
    }
}

fn serve(
    mut stream: TcpStream,
    shared: Arc<Mutex<Shared>>,
    port: u16,
    start: Instant,
) -> std::io::Result<()> {
    stream.set_read_timeout(Some(Duration::from_secs(10)))?;

    let mut raw: Vec<u8> = Vec::new();
    let mut buffer = [0u8; 4096];
    let head_end = loop {
        match find(&raw, b"\r\n\r\n") {
            Some(at) => break at,
            None => {
                if 32 * 1024 < raw.len() {
                    stream.write_all(&http::error(400, "the request headers are too long"))?;
                    return Ok(());
                }
                let read = stream.read(&mut buffer)?;
                if 0 == read {
                    return Ok(());
                }
                raw.extend_from_slice(&buffer[..read]);
            }
        }
    };

    let head_text = String::from_utf8_lossy(&raw[..head_end]).into_owned();
    let head = match http::parse_head(&head_text) {
        Some(head) => head,
        None => {
            stream.write_all(&http::error(400, "not an HTTP request"))?;
            return Ok(());
        }
    };

    if let Err(denial) = http::guard(&head, port) {
        stream.write_all(&http::error(403, &denial.message()))?;
        return Ok(());
    }

    let wanted = head.content_length();
    if 64 * 1024 < wanted {
        stream.write_all(&http::error(400, "the body is too long for one console line"))?;
        return Ok(());
    }
    let mut body: Vec<u8> = raw[head_end + 4..].to_vec();
    while body.len() < wanted {
        let read = stream.read(&mut buffer)?;
        if 0 == read {
            break;
        }
        body.extend_from_slice(&buffer[..read]);
    }
    let body = String::from_utf8_lossy(&body[..body.len().min(wanted)]).into_owned();

    let response = route(&head, &body, &shared, start);
    stream.write_all(&response)?;
    stream.flush()
}

fn find(haystack: &[u8], needle: &[u8]) -> Option<usize> {
    haystack
        .windows(needle.len())
        .position(|window| window == needle)
}

fn route(head: &http::Head, body: &str, shared: &Arc<Mutex<Shared>>, start: Instant) -> Vec<u8> {
    match (head.method.as_str(), head.path.as_str()) {
        ("GET", "/") => http::response(
            200,
            "text/html; charset=utf-8",
            include_str!("web/index.html"),
        ),
        ("GET", "/state") => json_response(&state_json(shared, start)),
        ("GET", "/log") => {
            let since = http::query_param(&head.query, "since")
                .and_then(|v| v.parse().ok())
                .unwrap_or(0);
            // The hook's own lines by default, because that is what a status pane is for.
            // `?all=1` is the whole console, which is what you want when reading a
            // command's answer by hand.
            let ours_only = !matches!(
                http::query_param(&head.query, "all").as_deref(),
                Some("1") | Some("true") | Some("")
            );
            json_response(&log_json(shared, since, ours_only))
        }
        ("POST", "/command") => command(body, shared, head),
        ("GET", _) => http::error(404, "no such thing here. Try /, /state or /log."),
        _ => http::error(405, "GET or POST."),
    }
}

fn json_response(body: &str) -> Vec<u8> {
    http::response(200, "application/json; charset=utf-8", body)
}

fn state_json(shared: &Arc<Mutex<Shared>>, start: Instant) -> String {
    let now = now_ms(start);
    let guard = shared.lock().unwrap();
    let state = &guard.follower.state;

    let mut o = json::Object::new();
    o.bool("cs2_running", guard.session.is_some())
        .opt_num("pid", guard.session.as_ref().map(|s| s.pid as f64))
        .opt_str("exe", guard.session.as_ref().map(|s| s.exe.as_str()))
        .opt_str(
            "log",
            guard.log_path.as_ref().and_then(|p| p.to_str()),
        )
        .opt_str("trouble", guard.trouble.as_deref())
        .opt_str("mode", state.mode.map(|m| m.as_str()))
        .opt_str("map", state.map.as_deref())
        .bool("demo", state.demo)
        .bool("cursor_showing", state.cursor)
        .bool("world_in_eyes", state.world_in_eyes)
        .bool("sheet_over", state.sheet_over)
        .opt_num("fps", state.fps(now).map(|f| f as f64))
        .opt_str("fps_note", state.fps_note(now).as_deref())
        .opt_size("per_eye", state.per_eye(now))
        .opt_size("back_buffer", state.back_buffer)
        .opt_str("session_state", state.session_state.as_deref())
        .raw(
            "pipe_open",
            match state.pipe_open {
                Some(true) => "true",
                Some(false) => "false",
                None => "null",
            },
        )
        .opt_str("hook_version", state.hook_version.as_deref())
        .opt_str("cs2_build_tested", state.cs2_build_tested.as_deref())
        .opt_str("cs2_build_installed", guard.cs2_build_installed.as_deref())
        .num("cursor", guard.follower.cursor() as f64)
        .num("restarts", guard.follower.restarts as f64);
    o.finish()
}

fn log_json(shared: &Arc<Mutex<Shared>>, since: u64, ours_only: bool) -> String {
    let guard = shared.lock().unwrap();
    let (lines, cursor, missed) = guard.follower.since(since, ours_only);
    let encoded: Vec<String> = lines
        .iter()
        .map(|line| {
            let mut o = json::Object::new();
            o.num("cursor", line.cursor as f64).str("text", &line.text);
            o.finish()
        })
        .collect();

    let mut o = json::Object::new();
    o.raw("lines", &json::array(&encoded))
        .num("cursor", cursor as f64)
        .num("missed", missed as f64);
    o.finish()
}

/// How long to keep collecting once the echo has been seen.
///
/// This was a quiet gap - stop when nothing new has arrived for a while - until a real
/// session showed why that cannot work: a demo playing prints frame-time lines twice a
/// second and `[Demo] Demo Skipping` lines besides, so the log is never quiet and the
/// quiet gap only ever expired at the deadline. A fixed window after the echo is
/// predictable whether the game is talking or silent. Longer than the follower's own poll,
/// or the answer would be cut off by our own reading rather than by the game's finishing.
const AFTER_ECHO: Duration = Duration::from_millis(450);
const DEFAULT_WAIT: u64 = 1200;
const MAX_WAIT: u64 = 5000;

fn command(body: &str, shared: &Arc<Mutex<Shared>>, head: &http::Head) -> Vec<u8> {
    let began = Instant::now();
    let line = body.trim_matches(|c: char| c.is_whitespace());
    if let Err(message) = pipe::check_line(line) {
        return http::error(400, &message);
    }

    let wait = http::query_param(&head.query, "wait")
        .and_then(|v| v.parse::<u64>().ok())
        .unwrap_or(DEFAULT_WAIT)
        .min(MAX_WAIT);

    let before = shared.lock().unwrap().follower.cursor();

    if let Err(message) = pipe::send(line) {
        return http::error(503, &message);
    }

    // The hook echoes the line to console.log before it runs it (MirvVrXrPipe.cpp:88), so
    // the answer can be waited for instead of slept through. Only an echo whose text is
    // the line we wrote counts: several unrelated warnings share the `pipe: ` prefix.
    let deadline = Instant::now() + Duration::from_millis(wait);
    let mut echoed_at: Option<Instant> = None;
    let mut echo_cursor: Option<u64> = None;
    let mut seen = before;

    while Instant::now() < deadline {
        std::thread::sleep(Duration::from_millis(25));
        {
            let guard = shared.lock().unwrap();
            let (lines, cursor, _) = guard.follower.since(seen, true);
            for l in &lines {
                if let Some(afxvr::Event::PipeEcho(text)) = afxvr::parse_line(&l.text) {
                    if text.trim() == line && echo_cursor.is_none() {
                        echo_cursor = Some(l.cursor);
                        echoed_at = Some(Instant::now());
                    }
                }
            }
            seen = cursor;
        }
        if let Some(at) = echoed_at {
            if AFTER_ECHO < at.elapsed() {
                break;
            }
        }
    }

    let echoed = echoed_at.is_some();
    let guard = shared.lock().unwrap();
    // From the echo, not from when the line was written. The engine runs a queued command
    // when it gets to it, and during a demo skip that was seventeen seconds later - so
    // starting at the write buried a four-line answer under a hundred and seventy lines of
    // the game talking to itself.
    let from = echo_cursor.map(|c| c - 1).unwrap_or(before);
    // Everything, not just the prefixed lines: `mirv_vr_version` answers with a plain
    // `cs2-vr-spectator 0.0.0-dev` and three indented lines, and a reply that dropped all
    // four while keeping the echo of the question is not a reply.
    let (lines, cursor, missed) = guard.follower.since(from, false);
    let encoded: Vec<String> = lines
        .iter()
        .map(|line| {
            let mut o = json::Object::new();
            o.num("cursor", line.cursor as f64).str("text", &line.text);
            o.finish()
        })
        .collect();

    let mut o = json::Object::new();
    o.str("sent", line)
        // False means the line was written but the hook never said it had it: the session
        // may have gone, or frame logging and the console log may disagree. Not an error,
        // and not a success either.
        .bool("echoed", echoed)
        .raw("lines", &json::array(&encoded))
        .num("cursor", cursor as f64)
        .num("missed", missed as f64)
        .num("waited_ms", began.elapsed().as_millis() as f64);
    json_response(&o.finish())
}
