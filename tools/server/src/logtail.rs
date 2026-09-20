//! Following console.log, which is the only thing the hook can answer through.
//!
//! The byte handling is separate from the file handling so it can be tested without one:
//! `feed` takes whatever arrived and is pure, `poll` is the twelve lines that read it.

use std::collections::VecDeque;
use std::fs::File;
use std::io::{Read, Seek, SeekFrom};
use std::path::Path;

use crate::afxvr::{self, State};
use crate::noise;

/// One kept line and the cursor a client uses to ask for what came after it.
#[derive(Debug, Clone)]
pub struct Line {
    pub cursor: u64,
    pub text: String,
    /// Whether it is the hook talking. Every line is kept, because **a command's answer is
    /// usually not prefixed**: `mirv_vr_version` echoes `AFXVR: pipe: mirv_vr_version` and
    /// then prints its banner as plain `cs2-vr-spectator 0.0.0-dev` with indented
    /// continuation lines, and every help text does the same. Keeping only the prefixed
    /// lines returned the echo of a command and threw away its reply.
    pub afxvr: bool,
}

/// About twenty minutes of a talkative session. Enough that a client polling once a second
/// never misses anything, and small enough to hold while the game is using the memory.
const RING: usize = 2000;

/// A line longer than this is not a line. The game has written a megabyte without a newline
/// before now (a shader dump), and holding it as "the partial line" would grow without
/// bound while never being shown to anybody.
const MAX_PARTIAL: usize = 64 * 1024;

pub struct Follower {
    /// How far into the file we have read.
    offset: u64,
    /// The tail of the last read, which usually stops mid-line: the game is writing while
    /// we read, and half a line parsed as a whole one is a wrong fact rather than a late
    /// one.
    partial: String,
    ring: VecDeque<Line>,
    next_cursor: u64,
    pub state: State,
    pub restarts: u64,
    /// Whether known engine chatter is collapsed. Off with --raw, for the session where a
    /// puzzle turns out to live in a line the denylist covers.
    filter: bool,
    /// The noise seen since the last line that was kept, as (pattern, count). Held rather
    /// than reported line by line, so that two patterns interleaving do not produce more
    /// markers than they replace.
    pending: Vec<(usize, u64)>,
    pub suppressed: u64,
}

impl Follower {
    pub fn new(filter: bool) -> Follower {
        Follower {
            offset: 0,
            partial: String::new(),
            ring: VecDeque::new(),
            next_cursor: 1,
            state: State::default(),
            restarts: 0,
            filter,
            pending: Vec::new(),
            suppressed: 0,
        }
    }

    /// The file got shorter than what we had already read, which on Windows means it was
    /// recreated: CS2 truncates console.log at every launch with -condebug. Everything
    /// known is about a game that has gone.
    ///
    /// The cursor deliberately does NOT go back to zero - a client that kept its place
    /// across the restart must not be handed old lines as if they were new.
    fn restart(&mut self) {
        self.offset = 0;
        self.partial.clear();
        self.ring.clear();
        self.state = State::default();
        // Counted against the session that went, not carried into the next one.
        self.pending.clear();
        self.suppressed = 0;
        self.restarts += 1;
        self.push(
            "--- console.log was recreated: the game restarted ---".to_string(),
            true,
        );
    }

    fn push(&mut self, text: String, afxvr: bool) {
        if RING == self.ring.len() {
            self.ring.pop_front();
        }
        self.ring.push_back(Line {
            cursor: self.next_cursor,
            text,
            afxvr,
        });
        self.next_cursor += 1;
    }

    /// Bytes as they came off the file. Not necessarily whole lines, not necessarily valid
    /// UTF-8: console.log is game output and carries whatever the game put in it.
    pub fn feed(&mut self, bytes: &[u8], now_ms: u64) {
        self.partial.push_str(&String::from_utf8_lossy(bytes));

        while let Some(end) = self.partial.find('\n') {
            let line: String = self.partial.drain(..=end).collect();
            let line = line.trim_end_matches(['\n', '\r']).to_string();
            self.take(line, now_ms);
        }

        if MAX_PARTIAL < self.partial.len() {
            let stuck = std::mem::take(&mut self.partial);
            self.take(stuck, now_ms);
        }
    }

    fn take(&mut self, line: String, now_ms: u64) {
        let ours = afxvr::is_afxvr(&line);

        if !ours && self.filter {
            if let Some(pattern) = noise::matches(&line) {
                self.suppressed += 1;
                match self.pending.iter_mut().find(|(p, _)| *p == pattern) {
                    Some((_, count)) => *count += 1,
                    None => self.pending.push((pattern, 1)),
                }
                return;
            }
        }

        if ours {
            if let Some(event) = afxvr::parse_line(&line) {
                self.state.apply(&event, now_ms);
            }
        } else if line.trim().is_empty() {
            // Blank lines are the only thing worth dropping outright; they would push real
            // output out of the ring and say nothing.
            return;
        }

        self.flush_pending();
        self.push(line, ours);
    }

    /// What was suppressed, said out loud before the next line that was kept.
    ///
    /// Never a silent drop. A `CopyResource` that silently did nothing cost this project a
    /// black screen in a headset and an afternoon, and the reason it was expensive is that
    /// nothing anywhere said it had happened. A filter that quietly eats lines is the same
    /// failure wearing a friendlier face - and if a suppressed line ever turns out to
    /// matter, the count is what tells the next person where to look.
    fn flush_pending(&mut self) {
        if self.pending.is_empty() {
            return;
        }
        for (pattern, count) in std::mem::take(&mut self.pending) {
            self.push(noise::suppressed_note(pattern, count), false);
        }
    }

    /// Everything after `cursor`, and where to ask from next time. With `ours_only`, just
    /// the hook's own lines - which is what a status pane wants; a command's answer wants
    /// everything, because most of an answer is not prefixed.
    ///
    /// `missed` counts lines that fell out of the ring before the caller came back for
    /// them, so a client that dropped out knows it has a hole rather than quietly
    /// believing it saw everything.
    pub fn since(&self, cursor: u64, ours_only: bool) -> (Vec<&Line>, u64, u64) {
        let oldest = self
            .ring
            .front()
            .map(|l| l.cursor)
            .unwrap_or(self.next_cursor);
        let missed = if 0 < cursor && cursor + 1 < oldest {
            oldest - cursor - 1
        } else {
            0
        };
        let lines: Vec<&Line> = self
            .ring
            .iter()
            .filter(|l| l.cursor > cursor && (!ours_only || l.afxvr))
            .collect();
        (lines, self.next_cursor - 1, missed)
    }

    /// The cursor as it stands, for a caller that wants only what happens from now on.
    pub fn cursor(&self) -> u64 {
        self.next_cursor - 1
    }

    pub fn offset(&self) -> u64 {
        self.offset
    }

    /// The game was relaunched; drop what was true about the one that went.
    pub fn relaunched(&mut self) {
        self.restart();
    }

    /// Take a chunk that was read somewhere else.
    ///
    /// The reading deliberately does not happen in here. It used to: `poll` opened the
    /// file, read all of it and parsed it while the server's lock was held, and joining a
    /// session whose console.log was already megabytes long then blocked every request for
    /// as long as the fold took. A caller that asked for at most 1200 ms of waiting got
    /// 16933, which is not a slow answer but a wrong one.
    ///
    /// `from` is where the caller started reading. If we have moved since, the chunk is
    /// dropped and the next pass reads it again - cheaper than being wrong about where we
    /// are in the file.
    pub fn absorb(&mut self, from: u64, bytes: &[u8], now_ms: u64) {
        if from != self.offset {
            return;
        }
        self.offset += bytes.len() as u64;
        self.feed(bytes, now_ms);
    }
}

/// At most this much of the file per pass, so that catching up on a long log is many short
/// turns under the lock instead of one long one.
pub const CHUNK: usize = 512 * 1024;

/// What one look at the file found. Read with no lock held.
pub enum Chunk {
    Nothing,
    /// Shorter than where we had read to: -condebug recreates console.log at each launch.
    Relaunched,
    Bytes { from: u64, bytes: Vec<u8> },
}

pub fn read_chunk(path: &Path, offset: u64) -> std::io::Result<Chunk> {
    // The game holds console.log open for writing the whole time it runs. Rust's
    // File::open already asks for the sharing that allows (send-command.ps1 had to say
    // FileShare.ReadWrite by hand to get the same thing).
    let mut file = File::open(path)?;
    let length = file.metadata()?.len();

    if length < offset {
        return Ok(Chunk::Relaunched);
    }
    if length == offset {
        return Ok(Chunk::Nothing);
    }

    file.seek(SeekFrom::Start(offset))?;
    let wanted = (length - offset).min(CHUNK as u64) as usize;
    let mut bytes = vec![0u8; wanted];
    let read = file.read(&mut bytes)?;
    bytes.truncate(read);
    Ok(Chunk::Bytes {
        from: offset,
        bytes,
    })
}

impl Default for Follower {
    fn default() -> Follower {
        Follower::new(true)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::afxvr::Mode;

    #[test]
    fn follows_a_file_shaped_like_the_real_one() {
        // Byte for byte what the first worn session wrote: a UTF-8 BOM, then game lines
        // and stamped hook lines, and a multi-line message whose continuation starts at
        // column 0. Matching at column 0 alone kept the continuation and lost every line
        // that mattered, which read from outside as a server whose cursor was stuck.
        let mut f = Follower::new(true);
        f.feed(
            "\u{feff}09/20 12:14:24 [RenderSystem] Loaded\n\
             09/20 12:14:30 AFXVR: console pipe open at \\\\.\\pipe\\cs2vr, for this user only.\n\
             AFXVR: named pipe. mirv_vr_pipe 0 closes it.\n\
             09/20 12:14:32 AFXVR: mode WATCH (map \"de_mirage\", demo 1, cursor 0, button 0) - world in both eyes\n\
             09/20 12:14:34 AFXVR: 71.8 frames/s submitted at 2528x1600 per eye (13.93 ms)\n"
                .as_bytes(),
            0,
        );
        assert_eq!(Some(Mode::Watch), f.state.mode);
        assert_eq!(Some("de_mirage".to_string()), f.state.map);
        assert!(f.state.demo);
        assert_eq!(Some((2528, 1600)), f.state.per_eye(0));
        assert_eq!(Some(true), f.state.pipe_open);
        // Four hook lines when asked for ours, five when asked for everything - and the
        // kept ones keep their timestamps, because the operator reading the console pane
        // wants them.
        let (lines, _, _) = f.since(0, true);
        assert_eq!(4, lines.len());
        assert!(lines[0].text.starts_with("09/20 12:14:30 AFXVR:"));
        assert_eq!(5, f.since(0, false).0.len());
    }

    #[test]
    fn a_commands_answer_is_kept_even_though_it_is_not_prefixed() {
        // This is what `mirv_vr_version` actually prints: the echo carries the prefix and
        // the answer does not. Keeping only prefixed lines returned the echo of a command
        // and threw its reply away, which is most of the point of POST /command.
        let mut f = Follower::new(true);
        f.feed(
            "09/20 12:19:31 AFXVR: pipe: mirv_vr_version\n\
             09/20 12:19:31 cs2-vr-spectator 0.0.0-dev\n\
             \x20 built for CS2   2000908\n\
             \x20 this game is    2000908  - match\n"
                .as_bytes(),
            0,
        );
        assert_eq!(1, f.since(0, true).0.len());
        let (all, _, _) = f.since(0, false);
        assert_eq!(4, all.len());
        assert!(all[1].text.contains("cs2-vr-spectator 0.0.0-dev"));
        assert!(!all[1].afxvr);
    }

    #[test]
    fn collapses_the_line_that_is_ninety_nine_percent_of_the_file() {
        // 172143 of 173754 lines in a real session were this one. The point of collapsing
        // rather than dropping is the note: the count is what tells the next person to
        // look, if a suppressed line ever turns out to have mattered.
        let mut f = Follower::new(true);
        let noise = "09/20 12:35:40 [Client] Ignoring CSVCMsg_UserCommands_t: Missing command to delta from\n";
        for _ in 0..172 {
            f.feed(noise.as_bytes(), 0);
        }
        // Nothing is reported while the run is still going - the note needs its final count.
        assert!(f.since(0, false).0.is_empty());
        assert_eq!(172, f.suppressed);

        f.feed(b"09/20 12:35:41 AFXVR: pipe: mirv_vr_crop\n", 0);
        let (lines, _, _) = f.since(0, false);
        assert_eq!(2, lines.len());
        assert_eq!(
            "[172 lines suppressed: [Client] Ignoring CSVCMsg_UserCommands_t]",
            lines[0].text
        );
        assert!(lines[1].text.ends_with("AFXVR: pipe: mirv_vr_crop"));
        // The note belongs to the full view, not to the hook's own pane - there was never
        // a gap there to explain.
        assert_eq!(1, f.since(0, true).0.len());
    }

    #[test]
    fn two_patterns_interleaving_do_not_make_more_notes_than_they_replace() {
        let mut f = Follower::new(true);
        for _ in 0..3 {
            f.feed(b"09/20 12:35:40 [Client] Ignoring CSVCMsg_UserCommands_t: x\n", 0);
            f.feed(b"09/20 12:35:40 [Shooting] cl: ReadFrameInput - Presented data has no mod info\n", 0);
        }
        f.feed(b"09/20 12:35:41 AFXVR: recentred.\n", 0);
        let (lines, _, _) = f.since(0, false);
        assert_eq!(3, lines.len());
        assert!(lines[0].text.starts_with("[3 lines suppressed:"));
        assert!(lines[1].text.starts_with("[3 lines suppressed:"));
        assert_eq!(6, f.suppressed);
    }

    #[test]
    fn raw_keeps_every_byte_the_game_wrote() {
        // --raw, for the session where the puzzle turns out to live in a line the denylist
        // covers. A filter nobody can turn off is a filter that eventually hides the answer.
        let mut f = Follower::new(false);
        f.feed(b"09/20 12:35:40 [Client] Ignoring CSVCMsg_UserCommands_t: x\n", 0);
        assert_eq!(1, f.since(0, false).0.len());
        assert_eq!(0, f.suppressed);
    }

    #[test]
    fn keeps_only_our_own_lines() {
        let mut f = Follower::new(true);
        f.feed(
            b"Host_Changelevel: de_inferno\nAFXVR: recentred.\nsome game noise\n",
            0,
        );
        let (lines, cursor, missed) = f.since(0, true);
        assert_eq!(1, lines.len());
        assert_eq!("AFXVR: recentred.", lines[0].text);
        // The cursor counts every line kept, ours or not, so that asking for everything
        // and asking for ours share one sequence.
        assert_eq!(3, cursor);
        assert_eq!(0, missed);
    }

    #[test]
    fn half_a_line_waits_for_the_rest_of_itself() {
        // The game is writing while we read, so a read ending mid-line is the normal case
        // rather than the exception. Parsing the half would report a wrong mode, not a
        // late one.
        let mut f = Follower::new(true);
        f.feed(b"AFXVR: mode PLAY (map \"de_in", 0);
        assert_eq!(None, f.state.mode);
        assert!(f.since(0, true).0.is_empty());

        f.feed(b"ferno\", demo 0, cursor 0, button 0) - world in both eyes\n", 0);
        assert_eq!(Some(Mode::Play), f.state.mode);
        assert_eq!(Some("de_inferno".to_string()), f.state.map);
    }

    #[test]
    fn a_cursor_asks_for_what_came_after_it() {
        let mut f = Follower::new(true);
        f.feed(b"AFXVR: one\nAFXVR: two\n", 0);
        let (lines, cursor, _) = f.since(0, true);
        assert_eq!(2, lines.len());
        f.feed(b"AFXVR: three\n", 0);
        let (lines, _, missed) = f.since(cursor, true);
        assert_eq!(1, lines.len());
        assert_eq!("AFXVR: three", lines[0].text);
        assert_eq!(0, missed);
    }

    #[test]
    fn a_client_that_fell_behind_is_told_so() {
        let mut f = Follower::new(true);
        for i in 0..RING + 50 {
            f.feed(format!("AFXVR: line {i}\n").as_bytes(), 0);
        }
        let (lines, _, missed) = f.since(1, true);
        assert_eq!(RING, lines.len());
        assert_eq!(49, missed);
    }

    #[test]
    fn a_restart_clears_what_is_no_longer_true() {
        let mut f = Follower::new(true);
        f.feed(
            b"AFXVR: mode PLAY (map \"de_inferno\", demo 0, cursor 0, button 0) - world in both eyes\n",
            0,
        );
        let before = f.cursor();
        assert_eq!(Some(Mode::Play), f.state.mode);

        f.restart();
        assert_eq!(None, f.state.mode);
        assert_eq!(1, f.restarts);
        // And the cursor still only goes forwards, so a client that held its place is not
        // handed the dead session's lines a second time.
        let (lines, cursor, _) = f.since(before, true);
        assert!(cursor > before);
        assert_eq!(1, lines.len());
        assert!(lines[0].text.contains("restarted"));
    }

    #[test]
    fn a_line_that_never_ends_is_not_held_for_ever() {
        let mut f = Follower::new(true);
        let long = format!("AFXVR: {}", "x".repeat(MAX_PARTIAL + 10));
        f.feed(long.as_bytes(), 0);
        assert_eq!(1, f.since(0, true).0.len());
    }

    #[test]
    fn bytes_that_are_not_text_do_not_stop_the_follower() {
        let mut f = Follower::new(true);
        f.feed(b"AFXVR: \xff\xfe broken\nAFXVR: recentred.\n", 0);
        assert_eq!(2, f.since(0, true).0.len());
    }
}
