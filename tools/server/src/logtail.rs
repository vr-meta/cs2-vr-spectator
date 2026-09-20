//! Following console.log, which is the only thing the hook can answer through.
//!
//! The byte handling is separate from the file handling so it can be tested without one:
//! `feed` takes whatever arrived and is pure, `poll` is the twelve lines that read it.

use std::collections::VecDeque;
use std::fs::File;
use std::io::{Read, Seek, SeekFrom};
use std::path::Path;

use crate::afxvr::{self, State};

/// One kept line and the cursor a client uses to ask for what came after it.
#[derive(Debug, Clone)]
pub struct Line {
    pub cursor: u64,
    pub text: String,
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
}

impl Follower {
    pub fn new() -> Follower {
        Follower {
            offset: 0,
            partial: String::new(),
            ring: VecDeque::new(),
            next_cursor: 1,
            state: State::default(),
            restarts: 0,
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
        self.restarts += 1;
        self.push("--- console.log was recreated: the game restarted ---".to_string());
    }

    fn push(&mut self, text: String) {
        if RING == self.ring.len() {
            self.ring.pop_front();
        }
        self.ring.push_back(Line {
            cursor: self.next_cursor,
            text,
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
        if !afxvr::is_afxvr(&line) {
            return;
        }
        if let Some(event) = afxvr::parse_line(&line) {
            self.state.apply(&event, now_ms);
        }
        self.push(line);
    }

    /// Everything after `cursor`, and where to ask from next time. `missed` counts lines
    /// that fell out of the ring before the caller came back for them, so a client that
    /// dropped out knows it has a hole rather than quietly believing it saw everything.
    pub fn since(&self, cursor: u64) -> (Vec<&Line>, u64, u64) {
        let oldest = self.ring.front().map(|l| l.cursor).unwrap_or(self.next_cursor);
        let missed = if 0 < cursor && cursor + 1 < oldest {
            oldest - cursor - 1
        } else {
            0
        };
        let lines: Vec<&Line> = self.ring.iter().filter(|l| l.cursor > cursor).collect();
        (lines, self.next_cursor - 1, missed)
    }

    /// The cursor as it stands, for a caller that wants only what happens from now on.
    pub fn cursor(&self) -> u64 {
        self.next_cursor - 1
    }

    /// Read whatever the file has gained. Returns whether anything did.
    pub fn poll(&mut self, path: &Path, now_ms: u64) -> std::io::Result<bool> {
        // The game holds console.log open for writing the whole time it runs. Rust's
        // File::open already asks for the sharing that allows (send-command.ps1 had to
        // say FileShare.ReadWrite by hand to get the same thing).
        let mut file = File::open(path)?;
        let length = file.metadata()?.len();

        if length < self.offset {
            self.restart();
        }
        if length == self.offset {
            return Ok(false);
        }

        file.seek(SeekFrom::Start(self.offset))?;
        let mut buffer = Vec::new();
        file.read_to_end(&mut buffer)?;
        self.offset += buffer.len() as u64;
        self.feed(&buffer, now_ms);
        Ok(true)
    }
}

impl Default for Follower {
    fn default() -> Follower {
        Follower::new()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::afxvr::Mode;

    #[test]
    fn keeps_only_our_own_lines() {
        let mut f = Follower::new();
        f.feed(
            b"Host_Changelevel: de_inferno\nAFXVR: recentred.\nsome game noise\n",
            0,
        );
        let (lines, cursor, missed) = f.since(0);
        assert_eq!(1, lines.len());
        assert_eq!("AFXVR: recentred.", lines[0].text);
        assert_eq!(1, cursor);
        assert_eq!(0, missed);
    }

    #[test]
    fn half_a_line_waits_for_the_rest_of_itself() {
        // The game is writing while we read, so a read ending mid-line is the normal case
        // rather than the exception. Parsing the half would report a wrong mode, not a
        // late one.
        let mut f = Follower::new();
        f.feed(b"AFXVR: mode PLAY (map \"de_in", 0);
        assert_eq!(None, f.state.mode);
        assert!(f.since(0).0.is_empty());

        f.feed(b"ferno\", demo 0, cursor 0, button 0) - world in both eyes\n", 0);
        assert_eq!(Some(Mode::Play), f.state.mode);
        assert_eq!(Some("de_inferno".to_string()), f.state.map);
    }

    #[test]
    fn a_cursor_asks_for_what_came_after_it() {
        let mut f = Follower::new();
        f.feed(b"AFXVR: one\nAFXVR: two\n", 0);
        let (lines, cursor, _) = f.since(0);
        assert_eq!(2, lines.len());
        f.feed(b"AFXVR: three\n", 0);
        let (lines, _, missed) = f.since(cursor);
        assert_eq!(1, lines.len());
        assert_eq!("AFXVR: three", lines[0].text);
        assert_eq!(0, missed);
    }

    #[test]
    fn a_client_that_fell_behind_is_told_so() {
        let mut f = Follower::new();
        for i in 0..RING + 50 {
            f.feed(format!("AFXVR: line {i}\n").as_bytes(), 0);
        }
        let (lines, _, missed) = f.since(1);
        assert_eq!(RING, lines.len());
        assert_eq!(49, missed);
    }

    #[test]
    fn a_restart_clears_what_is_no_longer_true() {
        let mut f = Follower::new();
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
        let (lines, cursor, _) = f.since(before);
        assert!(cursor > before);
        assert_eq!(1, lines.len());
        assert!(lines[0].text.contains("restarted"));
    }

    #[test]
    fn a_line_that_never_ends_is_not_held_for_ever() {
        let mut f = Follower::new();
        let long = format!("AFXVR: {}", "x".repeat(MAX_PARTIAL + 10));
        f.feed(long.as_bytes(), 0);
        assert_eq!(1, f.since(0).0.len());
    }

    #[test]
    fn bytes_that_are_not_text_do_not_stop_the_follower() {
        let mut f = Follower::new();
        f.feed(b"AFXVR: \xff\xfe broken\nAFXVR: recentred.\n", 0);
        assert_eq!(2, f.since(0).0.len());
    }
}
