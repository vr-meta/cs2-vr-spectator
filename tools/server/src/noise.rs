//! Lines the engine repeats and nobody reads.
//!
//! Measured on a live session on 2026-09-20: of 173754 lines in console.log, **172143 were
//! one pattern** and 506 were the hook's. The file grows at about 18 KB/s while a demo
//! plays, and the first fold of one took seventeen seconds - not because the log was big
//! but because the same line was parsed a hundred and seventy-two thousand times.
//!
//! Source 2's own `con_filter_text_out` does not help: it was tried on that session with
//! `con_filter_enable 1`, both accepted, and the file grew at 17784 bytes/s against 18194
//! before. The filter applies to the on-screen console, not to the `-condebug` file. So it
//! has to live here.
//!
//! **A denylist, never an allowlist.** Keeping only what looks interesting is the mistake
//! that threw away a command's answer, because a reply is not `AFXVR:`-prefixed; the only
//! safe filter is one that drops what is known to be worthless and keeps everything else.

use crate::afxvr;

/// Each entry is matched against the start of the line, after its timestamp. Add to this
/// list only with a measurement: something that is loud AND has no diagnostic value.
///
/// `[RenderPipelineCsgo] RT` is deliberately NOT here - it is how experiment 22 worked out
/// what the engine renders at, and it is 44 lines, not 172 thousand.
pub const NOISE: [&str; 4] = [
    // 99.1% of the file on its own.
    "[Client] Ignoring CSVCMsg_UserCommands_t",
    "[Client] CL:  Forcing ExecuteQueuedOperations due to entity slot re-use",
    "[Shooting] cl: ReadFrameInput - Presented data has no mod info",
    "[Demo] Demo Skipping",
];

/// Which pattern this line is, if it is one of them.
pub fn matches(line: &str) -> Option<usize> {
    let body = afxvr::strip_noise(line);
    // Never the hook's own words, whatever the list says. A denylist entry that grew to
    // cover an AFXVR line would take the state machine's input away silently, which is the
    // one failure this whole design is trying not to have.
    if afxvr::is_afxvr(line) {
        return None;
    }
    NOISE.iter().position(|pattern| body.starts_with(pattern))
}

/// What stands in the log's place, so that a suppressed line is never a silent absence.
pub fn suppressed_note(pattern: usize, count: u64) -> String {
    let what = NOISE.get(pattern).copied().unwrap_or("engine chatter");
    let lines = if 1 == count { "line" } else { "lines" };
    format!("[{count} {lines} suppressed: {what}]")
}

#[cfg(test)]
mod tests {
    use super::*;

    // Every line here was counted in the real session, not invented.
    #[test]
    fn catches_the_one_that_is_the_whole_problem() {
        assert_eq!(
            Some(0),
            matches("09/20 12:35:41 [Client] Ignoring CSVCMsg_UserCommands_t: Missing command to delta from")
        );
        assert_eq!(
            Some(1),
            matches("09/20 12:21:53 [Client] CL:  Forcing ExecuteQueuedOperations due to entity slot re-use (44 weapon_glock)")
        );
        assert_eq!(
            Some(2),
            matches("09/20 12:21:53 [Shooting] cl: ReadFrameInput - Presented data has no mod info")
        );
        assert_eq!(
            Some(3),
            matches("09/20 12:21:53 [Demo] Demo Skipping: skipping to demo tick 91418 (game tick 144249)")
        );
    }

    #[test]
    fn keeps_everything_else() {
        // A command's answer, which carries no prefix at all and must survive.
        assert_eq!(None, matches("09/20 12:21:57 cs2-vr-spectator 0.0.0-dev"));
        assert_eq!(None, matches("  built for CS2   2000908"));
        // The line experiment 22 was built on.
        assert_eq!(
            None,
            matches("09/20 12:21:53 [RenderPipelineCsgo] RT 2528x2780")
        );
        // Ordinary engine output that is not on the list.
        assert_eq!(None, matches("09/20 12:21:53 [Prediction] Added TrueView prediction for player slot 4 (iM)"));
    }

    #[test]
    fn never_the_hooks_own_words() {
        assert_eq!(None, matches("09/20 12:21:57 AFXVR: pipe: mirv_vr_version"));
        assert_eq!(None, matches("AFXVR: named pipe. mirv_vr_pipe 0 closes it."));
    }

    #[test]
    fn says_what_it_took_away() {
        assert_eq!(
            "[172 lines suppressed: [Client] Ignoring CSVCMsg_UserCommands_t]",
            suppressed_note(0, 172)
        );
        assert_eq!(
            "[1 line suppressed: [Demo] Demo Skipping]",
            suppressed_note(3, 1)
        );
    }
}
