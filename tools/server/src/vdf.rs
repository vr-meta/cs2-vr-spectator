//! Valve's text formats, for the case where the game is NOT running.
//!
//! While a session is up, the running cs2.exe's own image path is the answer and this is
//! not used - it cannot disagree with the session being watched, which a library lookup
//! can. But a server started before the game has to find console.log somehow, so the walk
//! through libraryfolders.vdf and appmanifest_730.acf is ported here from the launcher's
//! LauncherLogic.h, with the same cases its tests cover.

/// Every value stored under `key`, in order of appearance.
///
/// libraryfolders.vdf and appmanifest_*.acf are the same format: quoted keys and quoted
/// values, nested in braces, backslashes escaped. The nesting is ignored on purpose - the
/// two questions asked here ("which libraries?", "which install folder?") do not need it,
/// and a parser that understands less has less to get wrong when Valve adds a field.
pub fn values(text: &str, key: &str) -> Vec<String> {
    let mut tokens: Vec<(String, bool)> = Vec::new(); // the token, and whether it opened a line
    let mut line_has_token = false;

    let chars: Vec<char> = text.chars().collect();
    let mut i = 0;
    while i < chars.len() {
        let c = chars[i];
        if '\n' == c {
            line_has_token = false;
            i += 1;
            continue;
        }
        if '/' == c && i + 1 < chars.len() && '/' == chars[i + 1] {
            while i < chars.len() && '\n' != chars[i] {
                i += 1;
            }
            line_has_token = false;
            continue;
        }
        if '"' != c {
            i += 1;
            continue;
        }

        let mut token = String::new();
        i += 1;
        while i < chars.len() && '"' != chars[i] {
            if '\\' == chars[i] && i + 1 < chars.len() {
                match chars[i + 1] {
                    '\\' => {
                        token.push('\\');
                        i += 2;
                        continue;
                    }
                    '"' => {
                        token.push('"');
                        i += 2;
                        continue;
                    }
                    'n' => {
                        token.push('\n');
                        i += 2;
                        continue;
                    }
                    't' => {
                        token.push('\t');
                        i += 2;
                        continue;
                    }
                    _ => {}
                }
            }
            token.push(chars[i]);
            i += 1;
        }
        i += 1; // the closing quote, or the end of the file

        tokens.push((token, !line_has_token));
        line_has_token = true;
    }

    // A value is the token after a key ON THE SAME LINE. A key that opens a block has
    // nothing after it on its line, and the next line's first token is a key, not a value.
    let mut found = Vec::new();
    for n in 0..tokens.len().saturating_sub(1) {
        if !tokens[n].1 || tokens[n + 1].1 {
            continue;
        }
        if tokens[n].0.eq_ignore_ascii_case(key) {
            found.push(tokens[n + 1].0.clone());
        }
    }
    found
}

/// The value of `key` in steam.inf, which is plain `Key=Value` lines.
pub fn inf_value(text: &str, key: &str) -> Option<String> {
    for line in text.lines() {
        let line = line.trim_end();
        if let Some(rest) = line.strip_prefix(key) {
            // A key that is only the beginning of another key is not that key: `Client`
            // must not answer with ClientVersion's value.
            if let Some(value) = rest.strip_prefix('=') {
                return Some(value.to_string());
            }
        }
    }
    None
}

#[cfg(test)]
mod tests {
    use super::*;

    const LIBRARY_FOLDERS: &str = "\"libraryfolders\"\n\
{\n\
\t\"0\"\n\
\t{\n\
\t\t\"path\"\t\t\"C:\\\\Program Files (x86)\\\\Steam\"\n\
\t\t\"label\"\t\t\"\"\n\
\t\t\"apps\"\n\
\t\t{\n\
\t\t\t\"730\"\t\t\"38654705664\"\n\
\t\t}\n\
\t}\n\
\t\"1\"\n\
\t{\n\
\t\t\"path\"\t\t\"D:\\\\SteamLibrary\"\n\
\t\t\"label\"\t\t\"\"\n\
\t}\n\
}\n";

    const APP_MANIFEST: &str = "\"AppState\"\n\
{\n\
\t\"appid\"\t\t\"730\"\n\
\t\"installdir\"\t\t\"Counter-Strike Global Offensive\"\n\
\t\"UserConfig\"\n\
\t{\n\
\t\t\"language\"\t\t\"english\"\n\
\t}\n\
}\n";

    #[test]
    fn gives_up_the_library_paths_unescaped() {
        assert_eq!(
            vec![
                "C:\\Program Files (x86)\\Steam".to_string(),
                "D:\\SteamLibrary".to_string()
            ],
            values(LIBRARY_FOLDERS, "path")
        );
    }

    #[test]
    fn the_install_folder_is_not_the_games_name() {
        // Which is the point of reading it: CS2 still lives in a folder called Global
        // Offensive.
        assert_eq!(
            vec!["Counter-Strike Global Offensive".to_string()],
            values(APP_MANIFEST, "installdir")
        );
    }

    #[test]
    fn a_key_that_opens_a_block_has_no_value() {
        assert!(values(LIBRARY_FOLDERS, "apps").is_empty());
        assert!(values(LIBRARY_FOLDERS, "libraryfolders").is_empty());
        assert!(values(APP_MANIFEST, "UserConfig").is_empty());
    }

    #[test]
    fn an_empty_value_is_still_a_value() {
        assert_eq!(2, values(LIBRARY_FOLDERS, "label").len());
        assert_eq!("", values(LIBRARY_FOLDERS, "label")[0]);
    }

    #[test]
    fn keys_are_matched_whatever_their_case() {
        // Valve has not been consistent about it.
        assert_eq!(2, values(LIBRARY_FOLDERS, "PATH").len());
    }

    #[test]
    fn survives_comments_quotes_and_a_file_that_stops_mid_string() {
        assert_eq!(
            vec!["yes".to_string()],
            values("// \"path\" \"nope\"\n\"path\" \"yes\"\n", "path")
        );
        assert_eq!(
            vec!["say \"hi\"".to_string()],
            values("\"name\" \"say \\\"hi\\\"\"\n", "name")
        );
        assert!(values("\"path\" \"C:\\\\unterminated", "path").len() <= 1);
        assert!(values("", "path").is_empty());
    }

    #[test]
    fn steam_inf_says_which_build_the_game_is() {
        let inf = "ClientVersion=2000908\r\nServerVersion=2000908\r\nPatchVersion=1.41.8.1\r\nappID=730\r\n";
        assert_eq!(Some("2000908".to_string()), inf_value(inf, "ClientVersion"));
        assert_eq!(Some("1.41.8.1".to_string()), inf_value(inf, "PatchVersion"));
        assert_eq!(Some("730".to_string()), inf_value(inf, "appID"));
        // A key that is only the beginning of another key is not that key.
        assert_eq!(None, inf_value(inf, "Client"));
        assert_eq!(None, inf_value(inf, "Missing"));
        assert_eq!(None, inf_value("", "ClientVersion"));
        // A last line with no newline after it still counts.
        assert_eq!(
            Some("42".to_string()),
            inf_value("A=1\nClientVersion=42", "ClientVersion")
        );
    }
}
