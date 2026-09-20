# cs2vr-server

A worn session has no console. The window is 2528x2780 clamped onto a 2560x1600 display,
Panorama lays the console out for the full height, and its input line ends up below the
bottom of the screen. So a running session is adjusted by writing one line at a time into
`\\.\pipe\cs2vr` and observed by reading `game/csgo/console.log` — six lines of PowerShell
to send a command, then `Select-String` over the log to find out what happened.

This is that loop as an HTTP API, so that the two people who need it — the operator at a
browser and an agent driving the game — go through the same door.

It **attaches** to a session that is already running. It never starts the game: that is
process creation, a remote thread and a DLL injection, and it belongs to `cs2vr.exe`.

```
cargo run  --manifest-path tools/server/Cargo.toml -- --port 8731
cargo test --manifest-path tools/server/Cargo.toml
```

## Endpoints

| | |
|---|---|
| `GET /` | the page: state, map picker, console, and the dials that get tuned while worn |
| `GET /state` | mode, map, frame rate, per-eye size, session state, hook and CS2 build |
| `GET /log?since=N` | the `AFXVR:` lines after cursor N, and the next cursor (`&all=1` for the whole console) |
| `POST /command` | one console line as the body; answers with what the log gained |

```sh
curl -H "X-Cs2Vr: 1" http://127.0.0.1:8731/state
curl -H "X-Cs2Vr: 1" -X POST --data "mirv_vr_panel spread 1.2" http://127.0.0.1:8731/command
```

`POST /command` does not sleep and hope. The hook echoes every line it is handed before it
runs it (`AFXVR: pipe: <line>`), so the answer is waited for: the response carries the
lines that appeared afterwards, and `echoed: false` means the line was written but the
session never acknowledged it. `?wait=<ms>` bounds the wait (default 1200, maximum 5000).

## The two rules it is built around

**Loopback is not enough.** Every request here runs an arbitrary console command inside a
running game, and there is deliberately no allowlist — `quit` is how a session is ended
politely. But any page the operator happens to have open can POST to `127.0.0.1`, and with
a plain-text body that is a "simple request" no preflight would stop. So the `Host` must be
the loopback address the socket was bound to, and anything that acts must carry
`X-Cs2Vr: 1`, a header no simple request may carry. Reads are left open: a foreign page can
send a GET but cannot read the answer.

**The pipe is one-way and one client at a time** (`PIPE_ACCESS_INBOUND`,
`nMaxInstances = 1`). So every command connects, writes and disconnects. Holding it open
would work perfectly here and would quietly take `scripts/send-command.ps1` — and anything
else the operator reaches for mid-session — off the air.

## Shape

Everything that decides anything is pure and tested on any machine, the way `MirvVrMath.h`
and `LauncherLogic.h` are; CI runs `cargo test` on the Linux runner.

| | |
|---|---|
| `afxvr.rs` | one console line to one fact, and the fold down to what is true now |
| `logtail.rs` | following a file the game is writing, and noticing when it was recreated |
| `http.rs` | requests, responses, and who is allowed to act |
| `json.rs`, `vdf.rs`, `paths.rs` | writing JSON, Valve's text formats, where the game is |
| `pipe.rs`, `cs2.rs` | the only two Windows-only parts: the named pipe, and finding cs2.exe |

There are no dependencies, on purpose. This process writes console commands into a game
through a pipe ACL'd to one user; every crate would live inside that boundary, and what it
actually needs is a socket, a file and sixty lines of JSON.

## What the first worn session proved, and what it broke

Verified against a live `start-vr.ps1` session on 2026-09-20: the process lookup, the
relaunch detection against a real truncation, the pipe write end to end, the echo
correlation, and `/state` reporting `watching` on `de_mirage` at 2528x1600 per eye with the
session `FOCUSED`.

It also found four faults that no test without a game could have found, all now fixed and
all with a test of their own:

- **Real lines carry `MM/DD HH:MM:SS ` in front of them** and the parser anchored at column
  0. Of 210 hook lines it matched the four continuation lines of multi-line messages — the
  only ones with no stamp — and missed every line that mattered. It looked from outside
  like a cursor that was stuck. The stamp is now stripped when present and never required.
- **console.log opens with a UTF-8 BOM**, which sits in front of the first line of a fold
  from byte zero.
- **A command's answer is usually not prefixed.** `mirv_vr_version` echoes
  `AFXVR: pipe: mirv_vr_version` and then prints a plain banner with indented continuation
  lines; keeping only prefixed lines returned the echo of a question and threw away its
  answer. Every line is now kept, with `/log` filtering to the hook's own by default and
  `?all=1` for the whole console.
- **The follower held the server's lock across the whole read and parse.** Joining a
  session whose log was already 6 MB blocked requests: a `POST /command` that asked for at
  most 1200 ms took 16933. Reading now happens with no lock held, in 512 KB turns.

Still not exercised: the **MENU -> PLAY** walk (the first session auto-played a demo, so it
went MENU -> WATCH), and the **page under live traffic** — the sliders and the map picker
have only been driven by hand with `curl`.

## Notes from the log it reads

- `-condebug` writes `MM/DD HH:MM:SS ` in front of the **first** line of each message and
  leaves the continuation lines of a multi-line one at column 0, and the file opens with a
  UTF-8 BOM. A parser must strip both and require neither.
- A command's **answer is usually not `AFXVR:`-prefixed** — only the hook's echo of it is.
- The mode line is printed **only when the mode changes**, and nothing dumps the current
  one. So the file is folded from byte zero at startup, not tailed.
- The frame-rate lines need `mirv_vr_xr fps 1`, which `vr.cfg` sends on every supported
  launch. They come every two seconds; after five without one the frame rate is reported as
  unknown rather than as the last number seen.
- `-condebug` recreates `console.log` at every launch. A file shorter than where we had
  read to means a new game, so the state is dropped — while the cursor keeps going
  forwards, so a client that held its place is not handed the dead session's lines again.
- `MENU`, `WATCH` and `PLAY` are capitals; `idle` is not. `<empty>` is the engine's way of
  saying no map is loaded, not a map called `<empty>`.
