//! Where console.log is, given something we already know.
//!
//! The launcher finds CS2 through the Steam registry, libraryfolders.vdf and
//! appmanifest_730.acf, and has to - it starts a game that is not running yet. This server
//! attaches to a session that already exists, so the running cs2.exe's own image path is
//! the answer and cannot disagree with the session actually being watched. The string work
//! is here, free of Windows, so it is tested on any machine.

/// `...\game\bin\win64\cs2.exe` -> `...\game\csgo`, which holds console.log and steam.inf.
///
/// Walks up from the executable rather than pattern-matching the whole path, so a library
/// on another drive, a renamed install folder or a path with spaces all behave.
pub fn csgo_from_exe(exe: &str) -> Option<String> {
    let parts = split(exe);
    // cs2.exe, win64, bin - and what is left is ...\game.
    if parts.len() < 4 {
        return None;
    }
    let last = parts[parts.len() - 1].to_ascii_lowercase();
    if "cs2.exe" != last {
        return None;
    }
    let game = &parts[..parts.len() - 3];
    Some(join(game, &["csgo"]))
}

/// The `--cs2 <folder>` override. Takes the install root (the folder holding `game\`), the
/// `game` folder or `game\csgo` itself, because all three are things a person reasonably
/// means by "where CS2 is" and telling them off is not worth a line of code.
pub fn csgo_from_folder(folder: &str) -> String {
    let parts = split(folder.trim_end_matches(['\\', '/']));
    let last = parts.last().map(|s| s.to_ascii_lowercase());
    match last.as_deref() {
        Some("csgo") => join(&parts, &[]),
        Some("game") => join(&parts, &["csgo"]),
        _ => join(&parts, &["game", "csgo"]),
    }
}

/// The only console a worn launch has.
pub fn console_log_in(csgo: &str) -> String {
    join(&split(csgo), &["console.log"])
}

/// The build the game on disk actually is, as opposed to the one the hook was made for.
pub fn steam_inf_in(csgo: &str) -> String {
    join(&split(csgo), &["steam.inf"])
}

/// Both separators, because a path typed by a person on Windows is whichever one they
/// reached for.
fn split(path: &str) -> Vec<&str> {
    path.split(['\\', '/']).filter(|s| !s.is_empty()).collect()
}

fn join(base: &[&str], tail: &[&str]) -> String {
    let mut parts: Vec<&str> = base.to_vec();
    parts.extend_from_slice(tail);
    parts.join("\\")
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn walks_up_from_the_running_game() {
        let csgo = csgo_from_exe(
            "D:\\SteamLibrary\\steamapps\\common\\Counter-Strike Global Offensive\\game\\bin\\win64\\cs2.exe",
        )
        .unwrap();
        assert_eq!(
            "D:\\SteamLibrary\\steamapps\\common\\Counter-Strike Global Offensive\\game\\csgo\\console.log",
            console_log_in(&csgo)
        );
    }

    #[test]
    fn the_case_of_the_executable_is_not_ours_to_choose() {
        // Toolhelp32 and QueryFullProcessImageNameW do not agree on it.
        assert!(csgo_from_exe("C:\\g\\game\\bin\\win64\\CS2.EXE").is_some());
    }

    #[test]
    fn the_same_folder_holds_the_build_number() {
        let csgo = csgo_from_exe("C:\\g\\game\\bin\\win64\\cs2.exe").unwrap();
        assert_eq!("C:\\g\\game\\csgo", csgo);
        assert_eq!("C:\\g\\game\\csgo\\steam.inf", steam_inf_in(&csgo));
    }

    #[test]
    fn refuses_something_that_is_not_the_game() {
        assert_eq!(None, csgo_from_exe("C:\\Windows\\explorer.exe"));
        assert_eq!(None, csgo_from_exe("cs2.exe"));
    }

    #[test]
    fn takes_the_override_as_whichever_folder_was_meant() {
        let want = "C:\\cs2\\game\\csgo";
        assert_eq!(want, csgo_from_folder("C:\\cs2"));
        assert_eq!(want, csgo_from_folder("C:\\cs2\\"));
        assert_eq!(want, csgo_from_folder("C:/cs2"));
        assert_eq!(want, csgo_from_folder("C:\\cs2\\game"));
        assert_eq!(want, csgo_from_folder("C:\\cs2\\game\\csgo"));
    }
}
