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

### PENDING Debug overlay: consolidate all debug UI into a removable floating panel

**Firmware:** this commit
**Why:** Adding `CMD_DEBUG_FIRE` (0x1B) and `CMD_DEBUG_HIT` (0x1C) so a
bare second Pro Micro (no trigger, no IR LED, no vest) can participate
in IR + mesh testing. Rather than scatter buttons through existing
views, want one floating overlay that holds **every** debug surface
the app currently has (or grows) so it's a single-file strip-out when
the test rig is no longer needed.

**iOS changes:**

- File: `laserTag/BLEManager.swift`
  - Add `sendDebugFire()` → write `[0x1B]` to the emitter command char
  - Add `sendDebugHit(shooterId: UInt8 = 0xFF)` → write `[0x1C, shooterId]`
  - Mirror the existing `CMD_RESPAWN` / `CMD_RELOAD` write paths

- File: `laserTag/DebugOverlay.swift` (new)
  - Floating, draggable, semi-transparent panel pinned to a corner
    (top-right default). Collapses to a small "DBG" pill when not in
    use so it's out of the way.
  - Contents (consolidate from elsewhere — see "Move into overlay"
    below):
    - **Fire** button → `sendDebugFire()`
    - **Sim Hit** button (with shooter-ID stepper, default 0xFF) →
      `sendDebugHit(shooterId:)`
    - **Emitter log** — embed `EmitterLogView`'s body, or factor it
      into a reusable subview if it's currently tab-bound
    - Any other developer-only controls scattered through the app
  - Wrap the entire file in `#if DEBUG` so release builds don't ship it

- File: `laserTag/ContentView.swift`
  - Host `DebugOverlay` once at the root via `.overlay(alignment: …)`
    so it floats above every phase (lobby, pairing, game, training).
    Single hosting point — making the whole overlay removable in one
    file delete + one line removed here.

- **Move into overlay (delete from current homes):**
  - `DashboardView.swift` — remove the "Logs" tab and the
    `case .emitterLog: "Emitter Log"` line if it's only used by that
    tab. Tab bar drops from 5 to 4.
  - `FreePracticeView.swift` — remove the `#if DEBUG` mock-hit button;
    Sim Hit in the overlay replaces it.
  - Audit for any other DEBUG-gated buttons / dev-only sheets and
    fold them into the overlay too.

**Notes / gotchas:**

- Goal is "one commit to remove the test rig." Resist the temptation
  to add debug logic anywhere except `DebugOverlay.swift` +
  `BLEManager.swift` send helpers.
- `CMD_DEBUG_FIRE` has no ack — phone observes the effect via the
  next `RSP_STATE_UPDATE` (mag ammo decrement).
- `CMD_DEBUG_HIT` has no ack either — observed via `RSP_STATE_UPDATE`
  (health decrement) and optionally `RSP_DEATH_NOTIFY` if it kills.
- Firmware gates both commands behind `current_state ==
  EMITTER_GAME_ACTIVE` (Fire) or `EMITTER_GAME_ACTIVE`/`EMITTER_DEAD`
  (Hit, via `on_vest_hit`). The overlay should reflect this — buttons
  disabled outside an active game, with a hint about why.

---

## Applied

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
