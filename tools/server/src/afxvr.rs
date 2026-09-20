//! What a worn session says about itself, read back out of console.log.
//!
//! The hook has no reply channel - its pipe is PIPE_ACCESS_INBOUND (MirvVrXrPipe.cpp:105)
//! - so these lines are the only answer anything ever gets. Parsing them is therefore the
//! one part of this program that must not be wrong, and it is pure: no Windows, no files,
//! no clock. The clock is passed in. CI runs these tests on a Linux runner.

/// What the headset is showing. Decided in the hook by DecideMode and announced on change.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Mode {
    Idle,
    Menu,
    Watch,
    Play,
}

impl Mode {
    /// The hook prints MENU, WATCH and PLAY in capitals but `idle` in lower case
    /// (MirvVrXr.cpp:3779). A parser that keyed on the capitals would silently never see
    /// the one mode that means "the session is up and showing nothing".
    fn parse(word: &str) -> Option<Mode> {
        if word.eq_ignore_ascii_case("idle") {
            Some(Mode::Idle)
        } else if word.eq_ignore_ascii_case("menu") {
            Some(Mode::Menu)
        } else if word.eq_ignore_ascii_case("watch") {
            Some(Mode::Watch)
        } else if word.eq_ignore_ascii_case("play") {
            Some(Mode::Play)
        } else {
            None
        }
    }

    pub fn as_str(self) -> &'static str {
        match self {
            Mode::Idle => "idle",
            Mode::Menu => "menu",
            Mode::Watch => "watching",
            Mode::Play => "playing",
        }
    }
}

#[derive(Debug, Clone, PartialEq)]
pub enum Event {
    Mode {
        mode: Mode,
        /// None when no map is loaded. The engine hands back the literal `<empty>` rather
        /// than an empty string, and reading that as a map name is how VR mode once
        /// believed it had a world with nothing loaded at all (MirvVrXr.cpp:4553).
        map: Option<String>,
        demo: bool,
        cursor: bool,
        world_in_eyes: bool,
        sheet_over: bool,
    },
    /// Menu mode copies no eyes, so it reports the back buffer rather than a per-eye size.
    MenuFps { fps: f32, width: u32, height: u32 },
    EyeFps { fps: f32, width: u32, height: u32 },
    BackBuffer { width: u32, height: u32 },
    Swapchains { width: u32, height: u32 },
    SessionState(String),
    Version { hook: String, cs2_build: String },
    PipeOpen(bool),
    /// The hook echoes every line it was handed before running it (MirvVrXrPipe.cpp:88).
    /// That echo is what lets POST /command wait for its own command instead of sleeping
    /// and hoping. Several unrelated warnings share the `pipe: ` prefix, so a caller
    /// correlates by comparing this text with what it actually wrote rather than by
    /// treating the next such line as its own.
    PipeEcho(String),
}

const AFXVR: &str = "AFXVR: ";

/// What CS2 puts in front of a line before the hook's own text starts.
///
/// `-condebug` stamps `MM/DD HH:MM:SS ` on the FIRST line of each message and leaves the
/// continuation lines of a multi-line `advancedfx::Message` at column 0. So the stamp is
/// stripped when it is there and never required: 206 of the 210 AFXVR lines in the first
/// real session carried one, and demanding it would have thrown away the other four -
/// while matching at column 0 alone, as this did at first, kept ONLY those four and missed
/// every line that mattered.
fn strip_stamp(line: &str) -> &str {
    let b = line.as_bytes();
    let stamped = 15 <= b.len()
        && b[0].is_ascii_digit()
        && b[1].is_ascii_digit()
        && b'/' == b[2]
        && b[3].is_ascii_digit()
        && b[4].is_ascii_digit()
        && b' ' == b[5]
        && b[6].is_ascii_digit()
        && b[7].is_ascii_digit()
        && b':' == b[8]
        && b[9].is_ascii_digit()
        && b[10].is_ascii_digit()
        && b':' == b[11]
        && b[12].is_ascii_digit()
        && b[13].is_ascii_digit()
        && b' ' == b[14];
    if stamped {
        &line[15..]
    } else {
        line
    }
}

/// The line with everything that is not the hook's own words taken off the front: the
/// stamp, and the UTF-8 byte order mark that console.log opens with and that therefore
/// sits in front of the very first line of a fold from byte zero.
pub fn strip_noise(line: &str) -> &str {
    strip_stamp(line.trim_start_matches('\u{feff}'))
}

/// True for the lines this program keeps. Everything else in console.log belongs to the
/// game and is none of our business.
pub fn is_afxvr(line: &str) -> bool {
    strip_noise(line).starts_with("AFXVR:")
}

fn after<'a>(s: &'a str, prefix: &str) -> Option<&'a str> {
    s.find(prefix).map(|i| &s[i + prefix.len()..])
}

fn until<'a>(s: &'a str, end: &str) -> Option<&'a str> {
    s.find(end).map(|i| &s[..i])
}

/// `2528x2780`, as every size in the log is written.
fn parse_size(s: &str) -> Option<(u32, u32)> {
    let token = s.split(|c: char| c.is_whitespace() || ',' == c).next()?;
    let (w, h) = token.split_once('x')?;
    Some((w.trim().parse().ok()?, h.trim().parse().ok()?))
}

/// The integer after `name ` in a `demo 0, cursor 1, button 0` list.
fn parse_flag(s: &str, name: &str) -> bool {
    match after(s, name) {
        Some(rest) => {
            let digits: String = rest.chars().take_while(|c| c.is_ascii_digit()).collect();
            !digits.is_empty() && "0" != digits
        }
        None => false,
    }
}

/// The session states the hook can print (MirvVrXr.cpp:3188). Matched as a closed set
/// because `session begun.`, `session created; ...` and `session already created.` share
/// the same prefix and are not states.
const SESSION_STATES: [&str; 9] = [
    "IDLE",
    "READY",
    "SYNCHRONIZED",
    "VISIBLE",
    "FOCUSED",
    "STOPPING",
    "LOSS_PENDING",
    "EXITING",
    "UNKNOWN",
];

/// One console line in, at most one fact out.
pub fn parse_line(line: &str) -> Option<Event> {
    let line = line.trim_end_matches(['\r', '\n']);
    // Anchored, not searched. A game line that happens to quote `AFXVR: ` in the middle of
    // itself is the game talking about us, not us talking.
    let rest = strip_noise(line).strip_prefix(AFXVR)?;

    if rest.starts_with("mode ") {
        let r = after(rest, "mode ")?;
        let name = until(r, " (map \"")?;
        let mode = Mode::parse(name)?;
        let fields = after(r, " (map \"")?;
        let map_raw = until(fields, "\"")?;
        let tail = after(r, ") - ").unwrap_or("");
        return Some(Event::Mode {
            mode,
            map: if map_raw.is_empty() || "<empty>" == map_raw {
                None
            } else {
                Some(map_raw.to_string())
            },
            demo: parse_flag(fields, "demo "),
            cursor: parse_flag(fields, "cursor "),
            world_in_eyes: tail.starts_with("world"),
            sheet_over: tail.contains("with the window over it"),
        });
    }

    if rest.starts_with("menu screen, ") {
        let r = after(rest, "menu screen, ")?;
        let fps: f32 = until(r, " frames/s")?.trim().parse().ok()?;
        let (width, height) = parse_size(after(r, " at ")?)?;
        return Some(Event::MenuFps { fps, width, height });
    }

    if rest.contains(" frames/s submitted at ") {
        let fps: f32 = until(rest, " frames/s submitted at ")?.trim().parse().ok()?;
        let (width, height) = parse_size(after(rest, " frames/s submitted at ")?)?;
        return Some(Event::EyeFps { fps, width, height });
    }

    if rest.starts_with("back buffer is now ") {
        let (width, height) = parse_size(after(rest, "back buffer is now ")?)?;
        return Some(Event::BackBuffer { width, height });
    }

    if rest.starts_with("swapchains ") {
        let (width, height) = parse_size(after(rest, "swapchains ")?)?;
        return Some(Event::Swapchains { width, height });
    }

    if rest.starts_with("session ") {
        let word = after(rest, "session ")?.split_whitespace().next()?;
        if SESSION_STATES.contains(&word) {
            return Some(Event::SessionState(word.to_string()));
        }
        return None;
    }

    if rest.starts_with("cs2-vr-spectator ") {
        let r = after(rest, "cs2-vr-spectator ")?;
        let hook = until(r, ", built for CS2 ")?.to_string();
        let cs2_build = until(after(r, ", built for CS2 ")?, ".")?.to_string();
        return Some(Event::Version { hook, cs2_build });
    }

    if rest.starts_with("console pipe open") {
        return Some(Event::PipeOpen(true));
    }
    if rest.starts_with("console pipe closed") {
        return Some(Event::PipeOpen(false));
    }

    if rest.starts_with("pipe: ") {
        return Some(Event::PipeEcho(after(rest, "pipe: ")?.to_string()));
    }

    None
}

/// A frame-rate line older than this says nothing about now. Both of them come every two
/// seconds while frame logging is on, so silence means the session stopped submitting -
/// and a dashboard reporting a confident 72 fps for a session that died is worse than one
/// reporting nothing.
const FPS_STALE_MS: u64 = 5_000;

/// Everything the log has said, folded down to what is true now.
///
/// The mode line is edge-triggered: the hook prints it only when the mode CHANGES
/// (MirvVrXr.cpp:3775), and no command dumps the current one. So this is folded from byte
/// zero of the file rather than from the tail - a server that only tailed would know
/// nothing at all about a session that was already running when it started.
#[derive(Debug, Default, Clone)]
pub struct State {
    pub mode: Option<Mode>,
    pub map: Option<String>,
    pub demo: bool,
    pub cursor: bool,
    pub world_in_eyes: bool,
    pub sheet_over: bool,
    pub session_state: Option<String>,
    pub hook_version: Option<String>,
    pub cs2_build_tested: Option<String>,
    pub pipe_open: Option<bool>,
    pub back_buffer: Option<(u32, u32)>,
    per_eye: Option<(u32, u32)>,
    fps: Option<f32>,
    fps_seen_ms: Option<u64>,
    /// Whether a frame-rate line was EVER seen. vr.cfg sends `mirv_vr_xr fps 1`, so in any
    /// session started the supported way it was; never having seen one means the session
    /// was started some other way, which is a different thing to say than "stale".
    fps_ever: bool,
}

impl State {
    pub fn apply(&mut self, event: &Event, now_ms: u64) {
        match event {
            Event::Mode {
                mode,
                map,
                demo,
                cursor,
                world_in_eyes,
                sheet_over,
            } => {
                self.mode = Some(*mode);
                self.map = map.clone();
                self.demo = *demo;
                self.cursor = *cursor;
                self.world_in_eyes = *world_in_eyes;
                self.sheet_over = *sheet_over;
            }
            Event::MenuFps { fps, width, height } => {
                self.fps = Some(*fps);
                self.fps_seen_ms = Some(now_ms);
                self.fps_ever = true;
                // Menu mode submits one quad built from the back buffer; there is no
                // per-eye image, and leaving the last one standing would describe a
                // session that is no longer rendering that way.
                self.per_eye = None;
                self.back_buffer = Some((*width, *height));
            }
            Event::EyeFps { fps, width, height } => {
                self.fps = Some(*fps);
                self.fps_seen_ms = Some(now_ms);
                self.fps_ever = true;
                self.per_eye = Some((*width, *height));
            }
            Event::BackBuffer { width, height } => self.back_buffer = Some((*width, *height)),
            Event::Swapchains { width, height } => self.per_eye = Some((*width, *height)),
            Event::SessionState(s) => self.session_state = Some(s.clone()),
            Event::Version { hook, cs2_build } => {
                self.hook_version = Some(hook.clone());
                self.cs2_build_tested = Some(cs2_build.clone());
            }
            Event::PipeOpen(open) => self.pipe_open = Some(*open),
            Event::PipeEcho(_) => {}
        }
    }

    fn fps_fresh(&self, now_ms: u64) -> bool {
        match self.fps_seen_ms {
            Some(seen) => now_ms.saturating_sub(seen) < FPS_STALE_MS,
            None => false,
        }
    }

    pub fn fps(&self, now_ms: u64) -> Option<f32> {
        if self.fps_fresh(now_ms) {
            self.fps
        } else {
            None
        }
    }

    pub fn per_eye(&self, now_ms: u64) -> Option<(u32, u32)> {
        if self.fps_fresh(now_ms) {
            self.per_eye
        } else {
            None
        }
    }

    /// Why there is no frame rate, in words a person or an agent can act on.
    pub fn fps_note(&self, now_ms: u64) -> Option<String> {
        if self.fps_fresh(now_ms) {
            return None;
        }
        if !self.fps_ever {
            return Some(
                "no frame-rate line yet - arm it with `mirv_vr_xr fps 1` (vr.cfg does this \
                 on a supported launch)"
                    .to_string(),
            );
        }
        let ago = self
            .fps_seen_ms
            .map(|seen| now_ms.saturating_sub(seen) / 1000)
            .unwrap_or(0);
        Some(format!(
            "nothing submitted for {ago}s - the session stopped, or frame logging was turned off"
        ))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    // Every fixture below is a line from a real console.log rather than one invented to
    // match the parser. Two are here because they nearly cost a bug each: the lower-case
    // `idle`, and `<empty>` as a map name.
    #[test]
    fn reads_a_mode_line() {
        let ev = parse_line(
            "AFXVR: mode PLAY (map \"de_inferno\", demo 0, cursor 0, button 0) - world in both eyes",
        )
        .unwrap();
        assert_eq!(
            ev,
            Event::Mode {
                mode: Mode::Play,
                map: Some("de_inferno".to_string()),
                demo: false,
                cursor: false,
                world_in_eyes: true,
                sheet_over: false,
            }
        );
    }

    #[test]
    fn reads_lower_case_idle() {
        match parse_line(
            "AFXVR: mode idle (map \"de_inferno\", demo 0, cursor 0, button 0) - a screen",
        ) {
            Some(Event::Mode { mode, .. }) => assert_eq!(Mode::Idle, mode),
            other => panic!("{other:?}"),
        }
    }

    #[test]
    fn empty_is_no_map_rather_than_a_map_called_empty() {
        match parse_line(
            "AFXVR: mode MENU (map \"<empty>\", demo 0, cursor 1, button 0) - a screen",
        ) {
            Some(Event::Mode {
                mode, map, cursor, ..
            }) => {
                assert_eq!(Mode::Menu, mode);
                assert_eq!(None, map);
                assert!(cursor);
            }
            other => panic!("{other:?}"),
        }
    }

    #[test]
    fn reads_the_window_over_the_world() {
        match parse_line(
            "AFXVR: mode PLAY (map \"de_inferno\", demo 0, cursor 1, button 0) - world in both eyes, with the window over it",
        ) {
            Some(Event::Mode { world_in_eyes, sheet_over, .. }) => {
                assert!(world_in_eyes);
                assert!(sheet_over);
            }
            other => panic!("{other:?}"),
        }
    }

    #[test]
    fn reads_both_frame_rate_lines() {
        assert_eq!(
            Some(Event::MenuFps {
                fps: 72.0,
                width: 2560,
                height: 1600
            }),
            parse_line("AFXVR: menu screen, 72.0 frames/s at 2560x1600")
        );
        assert_eq!(
            Some(Event::EyeFps {
                fps: 25.3,
                width: 2528,
                height: 2780
            }),
            parse_line("AFXVR: 25.3 frames/s submitted at 2528x2780 per eye (39.51 ms)")
        );
    }

    #[test]
    fn reads_a_frame_rate_line_with_the_session_state_appended() {
        // The state is appended only when it is NOT focused, which is exactly when someone
        // is reading the line to find out why the frame rate collapsed.
        assert_eq!(
            Some(Event::MenuFps {
                fps: 3.0,
                width: 2560,
                height: 1600
            }),
            parse_line("AFXVR: menu screen, 3.0 frames/s at 2560x1600  session VISIBLE")
        );
    }

    #[test]
    fn reads_sizes_and_version() {
        assert_eq!(
            Some(Event::BackBuffer {
                width: 2528,
                height: 2780
            }),
            parse_line(
                "AFXVR: back buffer is now 2528x2780, was 2560x1600 - rebuilding the swapchains."
            )
        );
        assert_eq!(
            Some(Event::Swapchains {
                width: 2528,
                height: 2780
            }),
            parse_line("AFXVR: swapchains 2528x2780, back buffer format 27 -> swapchain 29, 3 images each (two eyes and a panel).")
        );
        assert_eq!(
            Some(Event::Version {
                hook: "0.0.0-dev".to_string(),
                cs2_build: "2000908".to_string()
            }),
            parse_line("AFXVR: cs2-vr-spectator 0.0.0-dev, built for CS2 2000908. https://github.com/vr-meta/cs2-vr-spectator")
        );
    }

    #[test]
    fn a_session_state_is_a_state_and_not_a_sentence() {
        assert_eq!(
            Some(Event::SessionState("FOCUSED".to_string())),
            parse_line("AFXVR: session FOCUSED - visible and receiving input")
        );
        // These share the prefix and are not states. Accepting them would have had the
        // dashboard reporting the session as "begun." for the rest of the run.
        assert_eq!(None, parse_line("AFXVR: session begun."));
        assert_eq!(None, parse_line("AFXVR: session already created."));
        assert_eq!(
            None,
            parse_line("AFXVR: session created; waiting for the runtime to make it ready.")
        );
    }

    #[test]
    fn reads_the_echo_that_correlates_a_command() {
        assert_eq!(
            Some(Event::PipeEcho("mirv_vr_fov 90".to_string())),
            parse_line("AFXVR: pipe: mirv_vr_fov 90")
        );
    }

    // What a line off a real disk looks like. The first fixtures this parser was built
    // against had been run through a sed that stripped the stamp, and nobody knew: the
    // server then matched four continuation lines out of 210 and looked, from the outside,
    // like a cursor that was stuck.
    #[test]
    fn reads_a_line_with_the_timestamp_condebug_puts_on_it() {
        match parse_line(
            "09/20 12:14:32 AFXVR: mode MENU (map \"<empty>\", demo 0, cursor 1, button 0) - a screen",
        ) {
            Some(Event::Mode { mode, map, .. }) => {
                assert_eq!(Mode::Menu, mode);
                assert_eq!(None, map);
            }
            other => panic!("{other:?}"),
        }
        assert!(is_afxvr(
            "09/20 12:14:30 AFXVR: console pipe open at \\\\.\\pipe\\cs2vr, for this user only."
        ));
        assert_eq!(
            Some(Event::PipeEcho("mirv_vr_version".to_string())),
            parse_line("09/20 12:15:45 AFXVR: pipe: mirv_vr_version")
        );
    }

    #[test]
    fn a_continuation_line_has_no_timestamp_and_is_still_ours() {
        // condebug stamps only the first line of a multi-line message, so requiring a
        // stamp would lose these.
        assert!(is_afxvr(
            "AFXVR: named pipe. mirv_vr_pipe 0 closes it."
        ));
    }

    #[test]
    fn the_byte_order_mark_at_the_head_of_the_file_is_not_part_of_the_line() {
        // console.log opens with EF BB BF, which lands in front of the very first line of
        // a fold from byte zero.
        assert!(is_afxvr("\u{feff}09/20 12:14:30 AFXVR: recentred."));
        assert_eq!(
            Some(Event::SessionState("FOCUSED".to_string())),
            parse_line("\u{feff}09/20 12:14:33 AFXVR: session FOCUSED - visible and receiving input")
        );
    }

    #[test]
    fn the_game_quoting_us_is_not_us() {
        assert_eq!(None, parse_line("Some game line mentioning AFXVR: mode PLAY (map \"x\", demo 0, cursor 0, button 0) - a screen"));
        // And something stamp-shaped but not a stamp stays where it is.
        assert_eq!(None, parse_line("12/34 56:78:90AFXVR: recentred."));
    }

    #[test]
    fn ignores_the_game_and_the_unremarkable() {
        assert_eq!(None, parse_line("Host_Changelevel: de_inferno"));
        assert_eq!(None, parse_line(""));
        assert_eq!(None, parse_line("AFXVR: recentred."));
        assert!(!is_afxvr("Host_Changelevel: de_inferno"));
        assert!(is_afxvr("AFXVR: recentred."));
    }

    #[test]
    fn folds_a_session_down_to_what_is_true_now() {
        // Stamped, BOM and all, the way the file actually reads.
        let lines = [
            "\u{feff}09/20 12:14:31 AFXVR: cs2-vr-spectator 0.0.0-dev, built for CS2 2000908. https://example.invalid",
            "09/20 12:14:30 AFXVR: console pipe open at \\\\.\\pipe\\cs2vr, for this user only.",
            "09/20 12:14:32 AFXVR: mode MENU (map \"<empty>\", demo 0, cursor 1, button 0) - a screen",
            "09/20 12:14:34 AFXVR: menu screen, 72.0 frames/s at 2560x1600",
            "09/20 12:14:34 AFXVR: session FOCUSED - visible and receiving input",
            "09/20 12:15:02 AFXVR: mode PLAY (map \"de_inferno\", demo 0, cursor 0, button 0) - world in both eyes",
            // The per-eye size is whatever the clamped client rect is - 2528x1600 on the
            // machine this came from, not the 2780 rows the window asked for.
            "09/20 12:15:04 AFXVR: 71.8 frames/s submitted at 2528x1600 per eye (13.93 ms)",
        ];
        let mut state = State::default();
        for line in lines {
            if let Some(ev) = parse_line(line) {
                state.apply(&ev, 1_000);
            }
        }
        assert_eq!(Some(Mode::Play), state.mode);
        assert_eq!(Some("de_inferno".to_string()), state.map);
        assert_eq!(Some("FOCUSED".to_string()), state.session_state);
        assert_eq!(Some("2000908".to_string()), state.cs2_build_tested);
        assert_eq!(Some(true), state.pipe_open);
        assert_eq!(Some(71.8), state.fps(1_000));
        assert_eq!(Some((2528, 1600)), state.per_eye(1_000));
    }

    #[test]
    fn the_eye_size_changes_within_one_session_and_is_not_cached() {
        // The menu submits one quad built from the back buffer and a loaded map submits
        // two eyes, and the two sizes are legitimately different in the same session - the
        // back buffer changes on map load. Nothing may hold the first size it saw, and a
        // size change is not a relaunch: only a console.log shorter than our cursor is.
        let mut state = State::default();

        for (line, at) in [
            ("09/20 12:14:34 AFXVR: menu screen, 72.0 frames/s at 2560x1600", 1_000u64),
            ("09/20 12:15:01 AFXVR: back buffer is now 2528x2780, was 2560x1600 - rebuilding the swapchains.", 2_000),
            ("09/20 12:15:02 AFXVR: swapchains 2528x2780, back buffer format 27 -> swapchain 29, 3 images each (two eyes and a panel).", 2_000),
            ("09/20 12:15:04 AFXVR: 71.8 frames/s submitted at 2528x2780 per eye (13.93 ms)", 2_000),
        ] {
            state.apply(&parse_line(line).unwrap(), at);
        }
        assert_eq!(Some((2528, 2780)), state.per_eye(2_000));
        assert_eq!(Some((2528, 2780)), state.back_buffer);

        // And back out to the menu, where there are no eyes to report at all.
        state.apply(
            &parse_line("09/20 12:20:00 AFXVR: menu screen, 72.0 frames/s at 2560x1600").unwrap(),
            3_000,
        );
        assert_eq!(None, state.per_eye(3_000));
        assert_eq!(Some((2560, 1600)), state.back_buffer);
    }

    #[test]
    fn an_old_frame_rate_is_not_a_frame_rate() {
        let mut state = State::default();
        state.apply(
            &Event::EyeFps {
                fps: 72.0,
                width: 2528,
                height: 2780,
            },
            10_000,
        );
        assert_eq!(Some(72.0), state.fps(12_000));
        assert_eq!(None, state.fps(20_000));
        assert_eq!(None, state.per_eye(20_000));
        assert!(state.fps_note(20_000).unwrap().contains("10s"));
    }

    #[test]
    fn never_having_seen_one_reads_differently_from_having_lost_it() {
        let state = State::default();
        assert!(state.fps_note(0).unwrap().contains("mirv_vr_xr fps 1"));
    }
}
