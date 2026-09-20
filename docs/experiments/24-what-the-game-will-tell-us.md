# 24 — What the game will tell us about itself

**Date:** 2026-09-20 · **CS2 build:** 2000908, GSI `provider.version` 14181 · **Two desk
runs, no headset**

The operator asked for our own HUD, and listed what should be on it: the player list, the
bomb, health, the weapon. Before writing a single line of a renderer, one question had to be
answered, because a "no" would have killed the whole plan: **does that data exist as data?**

Everything the HUD shows today is pixels cut out of CS2's own HUD, and cutting it up is
directly what breaks Tab (#13) and made team select unclickable. Health and ammo are two
rectangles that were *guessed*, not measured, and the run skill has warned operators about it
for weeks. So the alternative worth measuring is CS2's Game State Integration: a documented
JSON feed the game posts to a local HTTP endpoint.

**Answer: yes, and by a wider margin than expected — in a demo. In a live game it is
partial, and the gap is exactly the player list.**

## How it was measured

Two runs, neither of which needed the headset — which is the first useful finding on its own,
because it means this whole question cost the operator nothing.

- **Run 1, WATCH.** `pro_mirage.dem`, a real 10-player match.
  `launch-cs2-experiment.ps1 -SelfBuilt -Demo pro_mirage -ExecCfg exp24_gsi`
- **Run 2, PLAY.** Eight bots on de_dust2. `-ExecCfg exp24_gsi_play`

Both at 2560x1600 **on purpose**: `setting.fullscreen 1` is still in `cs2_video.txt`, so
`-windowed` is ignored and the game takes the display. Asking for the resolution the desktop
is already in makes that a no-op instead of rearranging every window on the machine
(experiment 22). CS2 duly rewrote `setting.defaultresheight` from 2780 to 1600 on exit, as it
always does; harmless only because every launcher states the size explicitly.

Receiving side: `scratchpad/gsi-listen.ps1`, a raw `TcpListener` on 127.0.0.1:57448. A raw
socket rather than `HttpListener` because that wants a urlacl reservation or elevation, and
CS2 sends an ordinary POST with `Content-Length`. It answers `200 OK` *before* writing
anything to disk, because CS2 treats a slow consumer as a timeout.

## Does GSI work with `-insecure` and a DLL injected

**Yes.** First payload 4 seconds after launch, at the main menu, with the hook in the
process. This was not obvious: `-insecure` disables VAC and it would have been entirely
plausible for Valve to gate the telemetry with it.

Rate and size, measured over 47 s of the demo: **195 payloads, ~4.1/s, ~11.5 KB each** —
about 47 KB/s on loopback, with `"throttle" "0.1"` in the config asking for up to 10/s. The
size is all `allplayers`; the live-game payloads are ~700 B.

## What arrives, and where

Requested every key there is, so the answer would come from the payload rather than from
which keys we thought to ask for. Counts are payloads carrying that key.

| Key | demo (281) | game (51) | What it is |
| --- | --- | --- | --- |
| `provider` | 281 | 51 | build, our own steamid, timestamp |
| `map` | 280 | 50 | map, mode, phase, **both team scores**, team names |
| `round` | 280 | 48 | phase, `win_team`, and **`round.bomb`** |
| `player` | 281 | 48 | see below — a different shape in each mode |
| `previously` | 280 | 43 | what changed since the last payload |
| `phase_countdowns` | 280 | **0** | the round timer, in seconds |
| `bomb` | 280 | **0** | state, **world position**, carrier steamid |
| `allplayers` | 280 | **0** | **the player list** |
| `grenades` | 280 | **0** | live utility (empty in this segment — nothing was thrown) |

So the split is the old CS:GO one and it still holds in CS2: **five keys are
observer-only**, and a demo counts as observing.

### The bomb is in both modes — but as two different things

Worth stating because the table above reads as if the bomb were unavailable in a game, and
it is not:

- **Game:** `round.bomb` — measured values `planted`, `exploded`. (`defused` by documentation;
  the bots did not manage it.) A state, no position. **This is what a player's HUD shows
  anyway.**
- **Demo:** the top-level `bomb` block — `state` (`carried` measured), `position` as three
  world coordinates, and `player`, the carrier's steamid.

The demo form is strictly better and enables something CS2's own HUD cannot do: a bomb
marker floating at its real place in the world, seen from a head that can look around.

### `player`, in a game: everything the operator asked for

Measured, verbatim keys: `steamid`, `name`, `observer_slot`, `team`, `activity`
(`playing` / `menu` — **that alone is a cleaner "is a modal open" signal than the Windows
cursor we currently edge-detect**), `state` (`health`, `armor`, `helmet`, `flashed`, `smoked`,
`burning`, `money`, `round_kills`, `round_killhs`, `equip_value`), `weapons` (per slot:
`name`, `type`, `paintkit`, `ammo_clip`, `ammo_clip_max`, `ammo_reserve`, and `state`
`active`/`holstered` — so *which gun is in your hands* is explicit), `match_stats`
(`kills`, `assists`, `deaths`, `mvps`, `score`).

`flashed`, `smoked` and `burning` are numbers, not flags. In a headset those are worth more
than on a monitor: being blinded is currently something the operator can only infer from the
picture.

### `player`, in a demo: only who we are watching

`spectarget`, `position`, `forward`. No health — because the observer has none. **Our own
panel in WATCH mode therefore has to come from `allplayers[spectarget]`**, not from `player`.
Anyone writing the consumer will otherwise look for health in the obvious place and find
nothing.

### `allplayers`, per player, in a demo

`name`, `observer_slot`, `team`, the same full `state`, `match_stats` and `weapons` blocks as
above, plus `position` and `forward` as world vectors. Real team names came through too —
`MOUZ` and `Natus Vincere` from the pro demo.

That is more than CS2's own spectator HUD puts on screen, and the positions mean a name tag
can be drawn *on the player*, in the world, rather than in a list.

## What this changes

**For a demo — the finished half of the project — the whole HUD can be built from this and
nothing else.** No offsets, no cutting rectangles out of CS2's frame, so #13 (Tab) and the
team-select mess simply stop being possible: they are failure modes of cutting up somebody
else's HUD, and there would be no cutting.

**For a live game there are two real gaps**, and it is better to say so now than to discover
them halfway through a renderer:

1. **No player list.** `allplayers` is observer-only, confirmed by measurement and not just
   inherited from CS:GO documentation. The operator explicitly asked to keep the list, so in
   PLAY it needs a different source — CS2's own scoreboard, or struct reads, or accepting
   that a bot game has no list worth showing.
2. **No round timer.** `phase_countdowns` is observer-only too. This one looks recoverable
   without touching the game at all: `round.phase` transitions arrive, and `mp_roundtime` and
   `mp_freezetime` are ours to read — counting down from a phase change is arithmetic. That
   makes it pure logic, so it belongs in `MirvVrMath.h` with a test rather than anywhere near
   the renderer.

## Chat

Asked about, and the answer is a flat no: **there is no chat in GSI at all** — zero matches
for chat, say or message across 3.2 MB of payloads, and no such key in the schema. It is a
state feed, not an event log. `console.log` from a full bot session was also checked: 3208
lines, no chat among them either.

Raised with the operator and **dropped from the HUD**, which is what makes the rest of this
tidy: with chat gone, every remaining item on their list comes from one source.

## Two things that need writing down before this is built on

- **A GSI config cannot live where all our other configs live.** CS2 only scans
  `csgo\cfg\` for `gamestate_integration_*.cfg`, not subdirectories — so it cannot go in
  `cfg\cs2vr\`, and worse, `deploy-cfg.ps1` deletes loose copies of names it ships, so
  putting it in `scripts\cs2\` would make the deploy script remove the very file it
  installed. It was copied by hand for this probe. The README promises we only write to
  `cfg\cs2vr\` and `cs2vr_demos\`; shipping this means widening that promise deliberately,
  with the file named and a clean uninstall.
- **The port must not be 57444.** That is the shipped SteelSeries endpoint. This probe used
  57448. Two configs may both be present and both are honoured, so ours has to be additive
  and never overwrite `gamestate_integration_steelseries.cfg`.

## Artefacts

`scratchpad/gsi-demo.jsonl` (3.2 MB, 281 payloads, WATCH) and `scratchpad/gsi.jsonl`
(51 payloads, PLAY). Not committed — they carry the operator's steamid and a pro match's
player data, and the schema above is the part worth keeping.
