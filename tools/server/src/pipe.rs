//! One line into the hook's console pipe, which is the only way into a worn session.
//!
//! The pipe is PIPE_ACCESS_INBOUND with nMaxInstances = 1 (MirvVrXrPipe.cpp:105-110):
//! one-way, and one client at a time. So this connects, writes, and disconnects for every
//! single command. Holding it open would work perfectly and would take scripts/
//! send-command.ps1 - and anything else the operator reaches for mid-session - off the
//! air, with no message anywhere saying why.

/// kPipeMaxLine in MirvVrXrPipe.cpp:39. The hook discards a longer line with a warning, so
/// refusing it here turns silence into an answer.
pub const MAX_LINE: usize = 512;

/// Is this one console line? Checked before anything is opened, and tested on any machine.
pub fn check_line(line: &str) -> Result<(), String> {
    if line.trim().is_empty() {
        return Err("nothing to send".to_string());
    }
    if line.contains('\n') || line.contains('\r') {
        // One write is one command. A body that may carry a newline is a body that can
        // smuggle a second command past whoever read the first one.
        return Err("one line at a time: the body may not contain a newline".to_string());
    }
    if MAX_LINE < line.len() {
        return Err(format!(
            "{} characters; the hook ignores anything over {MAX_LINE}",
            line.len()
        ));
    }
    Ok(())
}

#[cfg(windows)]
mod windows {
    use std::ffi::c_void;
    use std::os::windows::ffi::OsStrExt;

    type Handle = *mut c_void;

    const GENERIC_WRITE: u32 = 0x4000_0000;
    const OPEN_EXISTING: u32 = 3;
    const ERROR_FILE_NOT_FOUND: u32 = 2;
    const ERROR_PIPE_BUSY: u32 = 231;

    extern "system" {
        fn CreateFileW(
            name: *const u16,
            access: u32,
            share: u32,
            security: *mut c_void,
            disposition: u32,
            flags: u32,
            template: Handle,
        ) -> Handle;
        fn WriteFile(
            file: Handle,
            buffer: *const u8,
            to_write: u32,
            written: *mut u32,
            overlapped: *mut c_void,
        ) -> i32;
        fn CloseHandle(object: Handle) -> i32;
        fn GetLastError() -> u32;
    }

    fn wide(s: &str) -> Vec<u16> {
        std::ffi::OsStr::new(s)
            .encode_wide()
            .chain(std::iter::once(0))
            .collect()
    }

    pub fn send(name: &str, line: &str) -> Result<(), String> {
        let wide_name = wide(name);
        let payload = format!("{line}\n");

        // The hook creates the pipe again after every disconnect, so there is a real
        // window in which a connect fails and nothing is wrong. Retrying briefly is the
        // difference between a command that works and one that fails once an hour for no
        // reason anybody can reproduce.
        let mut last = 0u32;
        for attempt in 0..12 {
            let handle = unsafe {
                CreateFileW(
                    wide_name.as_ptr(),
                    GENERIC_WRITE,
                    0,
                    std::ptr::null_mut(),
                    OPEN_EXISTING,
                    0,
                    std::ptr::null_mut(),
                )
            };

            if !handle.is_null() && handle as isize != -1 {
                let mut written: u32 = 0;
                let ok = unsafe {
                    WriteFile(
                        handle,
                        payload.as_ptr(),
                        payload.len() as u32,
                        &mut written,
                        std::ptr::null_mut(),
                    )
                };
                let error = unsafe { GetLastError() };
                unsafe { CloseHandle(handle) };

                if 0 == ok {
                    return Err(format!("the pipe accepted the connection but not the write (error {error})"));
                }
                return Ok(());
            }

            last = unsafe { GetLastError() };
            if ERROR_FILE_NOT_FOUND != last && ERROR_PIPE_BUSY != last {
                break;
            }
            std::thread::sleep(std::time::Duration::from_millis(if attempt < 4 {
                20
            } else {
                80
            }));
        }

        Err(match last {
            ERROR_FILE_NOT_FOUND => format!(
                "no pipe at {name}. CS2 is not running, or it was started with AFXVR_PIPE=0, \
                 or `mirv_vr_pipe 0` closed it."
            ),
            ERROR_PIPE_BUSY => format!(
                "{name} is busy. The hook allows one client at a time - something else is \
                 holding it open."
            ),
            other => format!("could not open {name} (error {other})"),
        })
    }
}

/// The pipe the hook opens, named in MirvVrXrPipe.cpp.
pub const PIPE_NAME: &str = r"\\.\pipe\cs2vr";

#[cfg(windows)]
pub fn send(line: &str) -> Result<(), String> {
    check_line(line)?;
    windows::send(PIPE_NAME, line)
}

/// Everything except the pipe itself runs anywhere, so the tests do too. Only this last
/// step needs Windows, and saying so is better than not building at all.
#[cfg(not(windows))]
pub fn send(line: &str) -> Result<(), String> {
    check_line(line)?;
    Err(format!(
        "{PIPE_NAME} is a Windows named pipe; this build cannot write to it"
    ))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn one_line_and_only_one() {
        assert_eq!(Ok(()), check_line("mirv_vr_fov 90"));
        assert!(check_line("").is_err());
        assert!(check_line("   ").is_err());
        // The one that matters: a body carrying a second command past whoever read the
        // first.
        assert!(check_line("mirv_vr_fov 90\nquit").is_err());
        assert!(check_line("mirv_vr_fov 90\rquit").is_err());
    }

    #[test]
    fn refuses_what_the_hook_would_silently_drop() {
        let long = "x".repeat(MAX_LINE + 1);
        let message = check_line(&long).unwrap_err();
        assert!(message.contains("513"));
        assert_eq!(Ok(()), check_line(&"x".repeat(MAX_LINE)));
    }

    #[test]
    fn semicolons_are_the_engines_business_and_not_ours() {
        // The console splits on them; this is still one line and one write.
        assert_eq!(Ok(()), check_line("bot_quota 6; map de_inferno"));
        // And `quit` is allowed on purpose - it is the politest way to end a session.
        assert_eq!(Ok(()), check_line("quit"));
    }
}
