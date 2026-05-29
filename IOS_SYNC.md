# iOS App Sync Notes

This file is the explicit channel between firmware changes here and
the iOS app at `/Users/shaurya/Documents/laserTag`. The Claude
"firmware → app sync" routine reads it **first**, processes any
`### PENDING` entries, then falls back to git-log heuristics for
anything not explicitly noted.

Use this file whenever a firmware change needs the app to do something
the routine couldn't reliably figure out from diffs alone:

- New opcodes whose UX implications aren't obvious from the bytes
- New files / views the app needs
- Semantic changes (existing opcode now means something different)
- Multi-file orchestration ("when X happens, do Y, Z, and W")
- Payload-shape changes that need careful migration on the app side

Mechanically-obvious changes (e.g. you added `CMD_FOO = 0x21` and the
serialize path is right next to the existing enum) don't need a note —
the routine will pick those up from the git log on its own.

---

## Workflow

1. **When making a firmware change that affects iOS**, add a `### PENDING`
   subsection under `## Pending` below. Format below.
2. **Commit the firmware change including this note in the same commit.**
   That way the SHA-to-note mapping is unambiguous.
3. **Run the sync routine.** It will:
   - Read each `### PENDING` entry in order
   - Apply the iOS changes
   - Move processed entries from `## Pending` to `## Applied`, annotating
     them with the iOS commit SHA(s) that landed them
   - Commit and push both repos
4. **If the routine hits ambiguity it can't resolve** (e.g. you said "add
   a screen for X" but X is underspecified), it stops and reports
   instead of guessing. Fix the note, re-run.

## Pending-entry format

```
### PENDING <one-line summary>

**Firmware:** <sha or branch>
**Why:** <short rationale — what protocol change drove this>

**iOS changes:**
- File: laserTag/<File>.swift
  - <bullet of exact change>
  - <another bullet>
- File: laserTag/<NewFile>.swift (new)
  - <what it does>
- Wire into: <DashboardView tab / sheet / NavigationStack entry / etc.>

**Notes / gotchas:**
- <anything subtle the routine should know>
```

Keep it terse — file paths and changes, not paragraphs. The routine has
access to both repos so it can look things up.

---

## Pending

_None._

---

## Applied

### Firmware `6bb6a9d` → iOS `0f893e9`: Debug overlay — consolidate test rig + CMD_DEBUG_FIRE/HIT

**Why:** Firmware added `CMD_DEBUG_FIRE` (0x1B) and `CMD_DEBUG_HIT` (0x1C)
so a bare second Pro Micro can drive IR + mesh testing. The PENDING entry
asked to fold **every** developer-only surface into one removable floating
overlay so the test rig is a single commit to strip out.

**iOS changes applied:**
- `Models.swift` — `Command.debugFire` (→ `[0x1B]`) and
  `.debugHit(shooterId:)` (→ `[0x1C, shooterId]`) serialize cases +
  `displayLabel`s. (Cases are *not* `#if DEBUG`-gated so the wire enum stays
  complete; only the send helpers/overlay are gated.)
- `BLEManager.swift` — `#if DEBUG sendDebugFire()` / `sendDebugHit(shooterId:)`
  write helpers mirroring `sendReload`/`sendRespawn`. No ack — effects observed
  via `RSP_STATE_UPDATE` (mag/health decrement) and `RSP_DEATH_NOTIFY`.
- `DebugOverlay.swift` (new, whole file `#if DEBUG`) — draggable,
  semi-transparent panel pinned top-right, collapsing to a "DBG" pill. Holds
  Fire + Sim Hit (shooter-ID stepper, default 0xFF), gated to an active game
  (`bleManager.gameState == .running`) with a why-disabled hint; the
  consolidated skip-nav, the Emitter Test launcher, and an embedded
  `EmitterLogView` (reused as-is inside a height-capped `NavigationStack`).
- `ContentView.swift` — single `#if DEBUG` hosting point via
  `.overlay(alignment: .topTrailing) { DebugOverlay() }`.
- **Moved into the overlay (deleted from former homes):**
  - `DashboardView.swift` — removed the "Logs" tab (5→4). Kept
    `case .emitterLog: "Emitter Log"` in `DeviceInfoView.responseText` — it is
    required for `ResponseType` switch exhaustiveness (RSP_LOG 0x9B still
    arrives and shows under "Last Response"), so it was *not* tab-bound.
  - `FreePracticeView.swift` + `ReactionTimeView.swift` — removed the
    `#if DEBUG` mock-hit buttons + `mockHealth` state.
  - `PairingView.swift` — removed `devSkipBar` (+ `skipLabel`) and the
    `EmitterTestView` dev sheet (+ `showEmitterTest`); both now launch from
    the overlay. `EmitterTestView.swift` itself is unchanged (still `#if DEBUG`,
    now presented from `DebugOverlay`).

**Decisions / notes (autonomous run):**
- Build verified: `xcodebuild ... -scheme laserTag -destination 'iPhone 17
  Pro' build` → **BUILD SUCCEEDED** (Debug config, so all `#if DEBUG` paths
  compiled).
- Audit interpretation: folded skip-nav + Emitter Test into the overlay (the
  note's "audit for any other DEBUG-gated buttons / dev-only sheets") so the
  rig is a true single-strip-out. EmitterTestView stays its own file —
  inlining a 335-line bring-up screen into the overlay would be a needless
  refactor; the overlay owns its only launch point now.
- Emitter log embedded inline via the note's "factor it into a reusable
  subview" option (reused `EmitterLogView` in a `NavigationStack`), not a
  separate compact tail.
- **Gotcha:** the root `.overlay` does not render above `fullScreenCover`s, so
  the overlay (incl. Sim Hit) is hidden while inside Free Practice / Reaction
  Time (both presented as covers from `LobbyView`). The removed in-view
  mock-hit buttons were pure local-UI sims; their real replacement
  (`CMD_DEBUG_HIT` → real `RSP_STATE_UPDATE` → `session.onHit`) is driven from
  the overlay before entering a cover, or via hardware. This matches the
  note's "Sim Hit in the overlay replaces it" intent.
- Strip-out recipe: delete `DebugOverlay.swift`, remove the `#if DEBUG`
  `.overlay` block in `ContentView`, and remove the two `#if DEBUG` send
  helpers in `BLEManager` (+ optionally the two `Command` debug cases).

### Firmware `this commit` → iOS (companion commit, same session): Multiplayer lobby/join/start wiring

**Why:** Full end-to-end wiring of host-broadcast → join → roster-sync →
game-start across both repos. Two new phone→emitter opcodes plus a
mesh name-length format fix. iOS side was implemented in the same
session, so this lands as Applied rather than Pending.

**Firmware changes:**
- `protocol.h` — `CMD_BROADCAST_LOBBY_JOIN = 0x1D`
  (payload: `lobby_code[4 LE] + player_id[1] + username[]`),
  `CMD_BROADCAST_LOBBY_STATE = 0x1E`
  (payload: `lobby_code[4 LE] + N×[pid, teamId, nameLen, name]`)
- `mesh.c` — fixed `mesh_broadcast_lobby_announce` /
  `mesh_broadcast_lobby_join` to write the `name_len` byte BEFORE the
  name (iOS parsers read `nameLen` at `payload[5]`). Added
  `mesh_broadcast_lobby_state` + a `MESH_GAME_START` receive callback so
  a joiner's emitter self-starts the game without a phone round-trip.
- `main.c` — `CMD_BROADCAST_LOBBY_JOIN` / `CMD_BROADCAST_LOBBY_STATE`
  handlers; two-pass `apply_game_config` that adopts the local player's
  host-assigned team from the roster; `start_game_local` helper shared
  by phone-driven start and mesh-driven joiner start.

**iOS changes applied:**
- `Models.swift` — `LobbyCommand.broadcastLobbyJoin(lobbyCode:playerId:username:)`
  (→ `[0x1D]`) and `.broadcastLobbyState(Data)` (→ `[0x1E]`) serialize cases.
- `GameManager.swift` — `onRosterChanged` callback +
  `buildLobbyStatePayload()`; `handleLobbyJoin` pushes roster via
  `onRosterChanged`; `case .gameStart` → `handleGameStart` parses the
  config (`parseGameConfig`) and starts the local game; `joinLobby(_:)`
  seeds the joined roster.
- `ContentView.swift` — wires `onRosterChanged` →
  `sendLobbyCommand(.broadcastLobbyState)`.
- `HostLobbyView.swift` — onAppear sends `setPlayerInfo` +
  `startLobbyBroadcast`; close button sends `stopLobby`.
- `JoinLobbyView.swift` — scan via `sendLobbyCommand(.startLobbyScan)`;
  lobby tap sends `setPlayerInfo` + `broadcastLobbyJoin`; joined
  waiting-room UI; `.onChange(of: gameActive)` → primeEmitterState +
  navigate to `.game`.

**Notes / gotchas:**
- Emitter is the sole mesh-frame originator per player (single
  monotonic seq counter) to avoid dedup collisions.
- Joiner self-starts on `MESH_GAME_START` without re-broadcasting;
  relies on existing TTL relay to propagate. No re-broadcast storm.
- Untested on hardware — only one assembled emitter rig exists.

### Firmware `81eb927` (iOS-only) → iOS `42c52ea`: Training mode mag-size bump

**Why:** With the previous `TrainingSession` config (`magSize = 30`,
`initialTotalAmmo = 9999`), the user ran out of mag ammo after 30
shots and the trigger silently stopped firing (firmware bails out of
`trigger_work_handler` when `mag_ammo == 0`). The training view
never sends `CMD_RELOAD`, so it just dead-ended.

**iOS changes applied:**
- `TrainingSession.swift`
  - Set `magSize = 9999` in the training `EmitterConfig` (matches
    `initialTotalAmmo`). `game_state_start_game` sets
    `starting_mag = min(initial_total_ammo, mag_size)`, so the
    starting mag now holds the whole budget. Other fields unchanged.

### Firmware `1ed47b6`, `ccdbdcd` (iOS-only) → iOS `b792f21`: Training mode — Target Practice + Reaction Time

**Why:** Single-player practice modes using existing BLE primitives
(CMD_CONFIG with friendlyFire=1, CMD_START, RSP_STATE_UPDATE,
CMD_RESPAWN, CMD_GAME_OVER). No new firmware opcodes.

**iOS changes applied:**
- `BLEManager.swift`
  - Added `onHitDetected`/`onShotFired` callbacks: diff consecutive
    RSP_STATE_UPDATE health/magAmmo values, fire on strict decrease.
    Reset `previousHealth`/`previousMagAmmo` on disconnect + game-over.
- `LobbyView.swift`
  - Added third primary action button "Training" presenting
    `TrainingSelectView` as fullScreenCover
- `TrainingSession.swift` (new)
  - Shared @Observable model: config build (friendlyFire=true,
    maxHealth=255, damage=1), start/reset/exit lifecycle, hit/shot/
    streak counters with 750ms streak-break window
- `TrainingSelectView.swift` (new)
  - Mode picker with vest-paired precondition check
- `FreePracticeView.swift` (new)
  - Continuous shooting HUD: score, shots, accuracy, streak, health bar.
    DEBUG mock-hit button for simulator testing
- `ReactionTimeView.swift` (new)
  - State machine (idle→armed→cued→scored), random 1.5–5s delay,
    full-screen red "FIRE" cue with haptic, anti-cheat early-hit
    detection, grade bands (ELITE/FAST/AVG/SLOW), sparkline stats
- `HitAnimationOverlay.swift` (new)
  - 6-layer showcase animation: radial flash (streak-colored),
    geometric hit marker, score popup, concentric ring, camera shake,
    milestone slow-mo vignette. Haptic: .heavy normal / .success milestones

**Design decisions:**
- Training presented as fullScreenCover from LobbyView (not a new
  AppFlow phase) — smaller diff, cleaner back-button behavior
- maxHealth=255/damage=1 approach (255 hits before death) chosen over
  auto-respawn to avoid complexity
- Sound deferred (visual-only shipped) — no audio assets available

### Firmware `9d511b2` (iOS-only) → iOS `72284ef`: Vest-disconnect banner fix

**Why:** User-reported UX bug — banner was not tappable and overlapped
UI elements behind it.

**iOS changes:**
- `VestDisconnectBanner.swift`
  - Added `flow.advance(to: .pairing)` to the tap action so tapping
    the banner navigates to the pairing flow (also dismisses the
    banner since `shouldShow` checks `flow.phase != .pairing`).
    `enterPairingMode()` BLE command retained.
- `ContentView.swift`
  - Changed banner hosting from `ZStack` overlay to
    `.safeAreaInset(edge: .top, spacing: 0)` so the banner pushes
    content down instead of occluding controls.
  - Banner is hosted once at the app root — no per-view placement
    needed; all phases (lobby, game) get the inset consistently.

### Firmware `d683f9f` → iOS `147b4b5`: RSP_LOG (0x9B) + log viewer

**Why:** Emitter on battery, no SWD access — added a BLE log relay so
firmware logs reach the phone over the existing GATT connection.

**iOS changes:**
- `Models.swift`
  - Added `case emitterLog = 0x9B` to `ResponseType`
  - Added `LogSeverity` enum (DBG/INF/WRN/ERR) with `label` and
    `systemImage` helpers
  - Added `EmitterLogEntry` struct (id, timestamp, severity, message)
- `BLEManager.swift`
  - Added observable `emitterLogs: [EmitterLogEntry]` (capped at 500
    entries, oldest dropped)
  - Added `clearEmitterLogs()` for the viewer's Clear button
  - Handled `.emitterLog` in the `didUpdateValueFor` switch — parses
    severity from `data[1]`, decodes UTF-8 from `data[2..]`, appends
    to the buffer
- `EmitterLogView.swift` (new)
  - Severity-colored monospaced rows with HH:mm:ss.SSS timestamps
  - Severity filter (All / INF+ / WRN+ / ERR)
  - Auto-scroll toggle
  - "Copy All" → UIPasteboard with `HH:mm:ss.SSS [SEV] message`
    formatting
  - "Clear" → calls `clearEmitterLogs()`
- `DashboardView.swift`
  - Added a 5th tab (`Logs`, system image `text.alignleft`) between
    Coded PHY and Device, hosting `EmitterLogView` in a
    `NavigationStack`
  - Added `case .emitterLog: "Emitter Log"` to the response-name
    switch (Swift exhaustiveness)

**Notes:** Notification payload max is MTU − 3 bytes. Before iOS
finishes MTU exchange (~few hundred ms after connect) the default
ATT_MTU of 23 limits a single log line to 18 chars of text — the
firmware truncates to fit current MTU automatically. The iOS side
doesn't need to do anything special; it just shows whatever arrives.
