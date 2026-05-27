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

### PENDING Target Practice mode — single-player, shoot your own vest

**Firmware:** n/a — iOS-only feature. The firmware primitives that already
exist (`CMD_CONFIG` with `friendly_fire=1`, `CMD_START`, `RSP_STATE_UPDATE`,
`CMD_RESPAWN`) cover everything; no new opcodes.
**Why:** Lets a single user with only their own emitter + vest practice
aiming. They physically shoot their own vest's TSOP, the vest accepts
the hit (because friendly-fire is on), the emitter reduces health, and
the app surfaces the hit with an animation + score.

**iOS changes:**

- `LobbyView.swift`
  - Add a third primary action button **Target Practice**, next to
    Create Game / Join Game. Same visual weight as the others.
  - Tapping it pushes / presents a new `TargetPracticeSetupView` (or
    goes straight into `TargetPracticeView` if no setup is needed —
    routine's call, see below).

- `TargetPracticeView.swift` (new)
  - Single-player game screen. No mesh, no map, no teammates.
  - On appear:
    1. Build a target-practice `EmitterConfig`:
       - `friendlyFire = true` (REQUIRED — without this the vest drops
         own-player hits)
       - `maxHealth = 255` (so the user doesn't die quickly; OR
         lower + auto-`sendRespawn()` on `onDeathNotification`)
       - `damage = 10`
       - `magSize = 30`, `reloadSpeedMs = 500`, `fireRateMs = 100`
       - `initialTotalAmmo = 9999` (effectively unlimited)
       - `fullAuto = false` (semi-auto feels better for practice;
         user can toggle in a small inline option if desired)
    2. `bleManager.sendConfig(config)` → wait for `configAck`
    3. `bleManager.startGame()` → wait for `startAck`. Emitter will
       automatically push `VEST_CMD_FRIENDLY_LIST` (empty) and
       `VEST_CMD_FRIENDLY_FIRE(1)` to the vest as part of its
       `CMD_START` handler, putting the vest in `VEST_GAME_ACTIVE`.
  - HUD elements:
    - Big score counter (hits)
    - Shots fired (track locally by watching `emitterState.magAmmo`
      decrement — each decrement = one shot)
    - Accuracy: `hits / shots` as a percentage
    - Current streak (consecutive hits without intervening shots that
      didn't hit — see "Notes" below on how to detect a hit)
    - Best streak
    - Current health (from `emitterState.health`) — small indicator
      so the user knows when they're about to need a respawn
  - Hit animation: when a hit is detected, trigger an attention-grabbing
    visual — flash overlay, expanding ring at center, hit-marker like
    a competitive shooter, +1 score popup that floats up and fades.
    Use SwiftUI animations + a brief haptic
    (`UIImpactFeedbackGenerator(style: .heavy).impactOccurred()`).
  - Sound: optional — if added, use AVFoundation with a short
    "hit" sound effect bundled in `Assets.xcassets` or a small
    sound file resource. Mute toggle in the top bar.
  - Controls:
    - "Reset" — re-sends the same config + `startGame()` (or
      `sendRespawn()` if game is still running) to reset health/ammo
      and zero the local score.
    - "Exit" — calls `bleManager.endGame()` (CMD_GAME_OVER 0x04) and
      pops back to `LobbyView`.

- `BLEManager.swift`
  - No new opcode handling needed; reuse the existing dispatch.
  - May need to expose a hit-detection signal that
    `TargetPracticeView` can observe. Suggested: new closure
    property `onHitDetected: ((UInt8 newHealth, UInt8 damage) -> Void)?`
    set inside the switch on `.stateUpdate` when `emitterState.health`
    just decreased (compare previous health vs new). Set it from
    `TargetPracticeView.onAppear` and clear in `onDisappear`. Don't
    just diff in the view — the diff happens inside the dispatch
    handler so the source of truth stays in `BLEManager`.

- `AppFlow.swift`
  - If you decide target practice should be a separate top-level
    phase (`.targetPractice`), add the case and the transitions.
    Alternative: keep flow at `.lobby` and treat target practice as a
    full-screen sheet — fewer enum changes, simpler back path. Pick
    the option that keeps the diff small.

- `GameManager.swift`
  - **Do not** wire target practice through `GameManager`. That class
    is for the mesh-driven multiplayer lifecycle (lobby, teams, mesh
    messages). Target practice is single-device, no mesh, no roster.
    Keep the practice state local to `TargetPracticeView` (or a small
    dedicated `@Observable` model owned by the view).

**Notes / gotchas:**

- **Detecting a "hit" cleanly:** the firmware sends
  `RSP_STATE_UPDATE` (0xA0) for many reasons — firing reduces
  `magAmmo`, hits reduce `health`, reload changes both magAmmo and
  reserve. The correct hit signal is **health decreased between
  consecutive 0xA0 updates**. Cache the previous health in
  `BLEManager` and compare. Mag-ammo decrements alone are shots, not
  hits.
- **Shots fired vs hits:** `magAmmo` decreasing is the most reliable
  shot indicator. Track in `BLEManager` the same way (cache
  previous, compute delta on each `.stateUpdate`). The view computes
  accuracy from those running totals.
- **Friendly-fire requirement is non-negotiable.** Without
  `friendly_fire = 1` in the config, the vest will silently drop the
  user's own hits (the vest does this check, not the emitter). If
  this is forgotten the whole feature looks broken with no obvious
  error.
- **Respawn behaviour:** if you go with normal `maxHealth` (e.g.
  100) instead of 255, wire `onDeathNotification` in
  `BLEManager` to auto-call `sendRespawn()` after a short delay
  (1–2 s — long enough for the user to see a "you died, respawning"
  animation). Otherwise the gun will just stop firing once dead
  and the user will think it's broken.
- **The vest must be paired** before target practice can start. The
  setup view (or the entry-into-target-practice action) should
  check `bleManager.vestPaired` and route to the pairing flow if
  not yet paired. Same precondition as starting a normal game.
- **No mesh broadcasts.** Make sure target-practice mode doesn't
  send `CMD_BROADCAST_GAME_CFG` or any lobby/mesh commands. The
  emitter would happily broadcast and confuse nearby devices.
  Use only `CMD_CONFIG` + `CMD_START` + (optional)
  `CMD_RESPAWN` + `CMD_GAME_OVER` + `CMD_RELOAD`.
- **Visual style:** the existing app leans into a monochrome /
  utilitarian aesthetic (see DashboardView). The hit animation
  should match — punchy but not cartoonish. Reference the
  competitive-shooter hit-marker idiom (brief geometric flash,
  not a confetti explosion).
- **If the routine wants to ask anything about visual design,
  scoring rules, or whether to add sound — stop and ask** before
  shipping. The exact UX of "cool hit animation" is subjective and
  worth a quick clarification rather than a wrong-direction
  rebuild.

---

## Applied

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
