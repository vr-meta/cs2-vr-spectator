//! Finding the game. Two ways, and the first one is better.
//!
//! While a session is up, the running cs2.exe's own image path says where console.log is
//! and cannot disagree with the session actually being watched - a Steam library lookup
//! can, if there are two installs or the manifest is stale. The library walk is the
//! fallback for a server started before the game, so that the page has something to show
//! and the right file to start following the moment the game appears.

use crate::paths;

#[derive(Debug, Clone)]
pub struct Session {
    pub pid: u32,
    pub exe: String,
}

#[cfg(windows)]
mod windows {
    use std::ffi::c_void;
    use std::os::windows::ffi::OsStrExt;

    type Handle = *mut c_void;

    const TH32CS_SNAPPROCESS: u32 = 2;
    const PROCESS_QUERY_LIMITED_INFORMATION: u32 = 0x1000;
    const HKEY_CURRENT_USER: Handle = 0x8000_0001u32 as usize as Handle;
    const HKEY_LOCAL_MACHINE: Handle = 0x8000_0002u32 as usize as Handle;
    const RRF_RT_REG_SZ: u32 = 0x0000_0002;
    const RRF_RT_REG_EXPAND_SZ: u32 = 0x0000_0004;

    #[repr(C)]
    struct ProcessEntry32W {
        size: u32,
        usage: u32,
        process_id: u32,
        default_heap_id: usize,
        module_id: u32,
        threads: u32,
        parent_process_id: u32,
        pri_class_base: i32,
        flags: u32,
        exe_file: [u16; 260],
    }

    extern "system" {
        fn CreateToolhelp32Snapshot(flags: u32, process_id: u32) -> Handle;
        fn Process32FirstW(snapshot: Handle, entry: *mut ProcessEntry32W) -> i32;
        fn Process32NextW(snapshot: Handle, entry: *mut ProcessEntry32W) -> i32;
        fn OpenProcess(access: u32, inherit: i32, process_id: u32) -> Handle;
        fn QueryFullProcessImageNameW(
            process: Handle,
            flags: u32,
            name: *mut u16,
            size: *mut u32,
        ) -> i32;
        fn CloseHandle(object: Handle) -> i32;
    }

    // The registry lives in advapi32, which nothing else here pulls in, so it has to be
    // asked for by name or the link fails on this one symbol.
    #[link(name = "advapi32")]
    extern "system" {
        fn RegGetValueW(
            key: Handle,
            subkey: *const u16,
            value: *const u16,
            flags: u32,
            kind: *mut u32,
            data: *mut c_void,
            bytes: *mut u32,
        ) -> i32;
    }

    fn wide(s: &str) -> Vec<u16> {
        std::ffi::OsStr::new(s)
            .encode_wide()
            .chain(std::iter::once(0))
            .collect()
    }

    fn from_wide(buffer: &[u16]) -> String {
        let end = buffer.iter().position(|&c| 0 == c).unwrap_or(buffer.len());
        String::from_utf16_lossy(&buffer[..end])
    }

    /// The first process called cs2.exe, with its full image path. The launcher looks for
    /// processes the same way (main.cpp:148).
    pub fn running_cs2() -> Option<super::Session> {
        let snapshot = unsafe { CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0) };
        if snapshot.is_null() || -1 == snapshot as isize {
            return None;
        }

        let mut entry: ProcessEntry32W = unsafe { std::mem::zeroed() };
        entry.size = std::mem::size_of::<ProcessEntry32W>() as u32;

        let mut found = None;
        let mut ok = unsafe { Process32FirstW(snapshot, &mut entry) };
        while 0 != ok {
            if from_wide(&entry.exe_file).eq_ignore_ascii_case("cs2.exe") {
                found = Some(entry.process_id);
                break;
            }
            ok = unsafe { Process32NextW(snapshot, &mut entry) };
        }
        unsafe { CloseHandle(snapshot) };

        let pid = found?;
        let exe = image_path(pid)?;
        Some(super::Session { pid, exe })
    }

    /// LIMITED_INFORMATION, because all that is wanted is a path. Asking for more would
    /// need rights this program has no business holding over the game it is watching.
    fn image_path(pid: u32) -> Option<String> {
        let process = unsafe { OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, 0, pid) };
        if process.is_null() {
            return None;
        }
        let mut buffer = [0u16; 32768];
        let mut size = buffer.len() as u32;
        let ok = unsafe { QueryFullProcessImageNameW(process, 0, buffer.as_mut_ptr(), &mut size) };
        unsafe { CloseHandle(process) };
        if 0 == ok {
            return None;
        }
        Some(from_wide(&buffer[..size as usize]))
    }

    pub fn reg_string(root: Handle, subkey: &str, value: &str) -> Option<String> {
        let subkey = wide(subkey);
        let value = wide(value);
        let mut buffer = [0u16; 1024];
        let mut bytes = (buffer.len() * 2) as u32;
        let result = unsafe {
            RegGetValueW(
                root,
                subkey.as_ptr(),
                value.as_ptr(),
                RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ,
                std::ptr::null_mut(),
                buffer.as_mut_ptr() as *mut c_void,
                &mut bytes,
            )
        };
        if 0 != result {
            return None;
        }
        Some(from_wide(&buffer))
    }

    /// Where Steam is. The launcher reads the same two values in the same order
    /// (main.cpp:181).
    pub fn steam_path() -> Option<String> {
        reg_string(HKEY_CURRENT_USER, "Software\\Valve\\Steam", "SteamPath").or_else(|| {
            reg_string(
                HKEY_LOCAL_MACHINE,
                "SOFTWARE\\WOW6432Node\\Valve\\Steam",
                "InstallPath",
            )
        })
    }
}

#[cfg(not(windows))]
mod windows {
    pub fn running_cs2() -> Option<super::Session> {
        None
    }
    pub fn steam_path() -> Option<String> {
        None
    }
}

/// The game, if it is running right now.
pub fn running() -> Option<Session> {
    windows::running_cs2()
}

/// `...\game\csgo` found through Steam, for when the game is not running.
pub fn csgo_through_steam() -> Option<String> {
    let steam = windows::steam_path()?;
    let steam = steam.replace('/', "\\");

    // Steam has moved this file between the two places over the years and both are still
    // found in the wild.
    let vdf = read(&format!("{steam}\\steamapps\\libraryfolders.vdf"))
        .or_else(|| read(&format!("{steam}\\config\\libraryfolders.vdf")))?;

    let mut libraries = crate::vdf::values(&vdf, "path");
    // The Steam folder is itself a library, and on a single-drive machine it is the only
    // one that is missing from the list often enough to matter.
    libraries.push(steam);

    for library in libraries {
        let manifest = match read(&format!("{library}\\steamapps\\appmanifest_730.acf")) {
            Some(text) => text,
            None => continue,
        };
        let install_dir = match crate::vdf::values(&manifest, "installdir").into_iter().next() {
            Some(dir) => dir,
            None => continue,
        };
        let root = format!("{library}\\steamapps\\common\\{install_dir}");
        let csgo = paths::csgo_from_folder(&root);
        if std::path::Path::new(&format!("{root}\\game\\bin\\win64\\cs2.exe")).exists() {
            return Some(csgo);
        }
    }
    None
}

fn read(path: &str) -> Option<String> {
    std::fs::read(path)
        .ok()
        .map(|bytes| String::from_utf8_lossy(&bytes).into_owned())
}

/// Which build the installed game is. The hook's own tested build comes out of the log, so
/// a mismatch between the two is visible on the page without anyone having to look it up.
pub fn installed_build(csgo: &str) -> Option<String> {
    let text = read(&paths::steam_inf_in(csgo))?;
    crate::vdf::inf_value(&text, "ClientVersion")
}
