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

### PENDING Training mode — Target Practice + Reaction Time (single-player)

**Firmware:** n/a — iOS-only feature. The existing primitives
(`CMD_CONFIG` with `friendlyFire=1`, `CMD_START`, `RSP_STATE_UPDATE`,
`RSP_DEATH_NOTIFY`, `CMD_RESPAWN`, `CMD_GAME_OVER`) cover everything;
no new opcodes.
**Why:** Single-player practice modes that work with only the user's
own emitter + vest. They shoot their own vest, the vest accepts the
hit (friendly-fire on), the emitter reports health drop, and the app
surfaces it. Doubles as a showcase demo for the whole system — the
hit animation in particular needs to be visibly impressive enough that
you'd record video of it.

Two sub-modes share the same setup and hit-detection plumbing:

- **Free Practice** — continuous shooting, tracks score + accuracy +
  streak. Pure aim-training loop.
- **Reaction Time** — wait-cue-shoot-measure loop. Random delay
  (1.5–5 s), then a clear cue, measure time from cue to hit, repeat.
  Track best/avg/worst times across the session.

**iOS changes:**

- `LobbyView.swift`
  - Add a third primary action button **Training**, peer of
    Create Game / Join Game. Same visual weight.
  - Tapping it presents `TrainingSelectView` (or routes
    directly via `AppFlow` — see below).

- `TrainingSelectView.swift` (new)
  - Small mode-picker screen. Two large buttons:
    - **Target Practice** → `FreePracticeView`
    - **Reaction Time** → `ReactionTimeView`
  - Brief one-line description under each. Back button returns to
    lobby.
  - Both modes share the same precondition: `bleManager.vestPaired`
    must be true. If not, route the user back into the pairing
    flow.

- `TrainingSession.swift` (new — shared model)
  - `@Observable` class owned by whichever training view is active.
  - Holds the config-build / start / reset / exit logic so both
    sub-modes use the same setup path:
    - `start()` builds an `EmitterConfig` with:
      - `friendlyFire = true` *(REQUIRED — without this the vest
        silently drops own-player hits and the feature looks
        broken)*
      - `maxHealth = 255`, `damage = 1` *(effectively no death loop
        for practice — see "Respawn behaviour" if you'd rather use
        normal HP + auto-respawn)*
      - `magSize = 30`, `reloadSpeedMs = 500`, `fireRateMs = 100`
      - `initialTotalAmmo = 9999`
      - `fullAuto = false`
    - Then `bleManager.sendConfig(config)` (wait `.configAck`),
      then `bleManager.startGame()` (wait `.startAck`). Emitter
      will automatically push `VEST_CMD_FRIENDLY_LIST` (empty) and
      `VEST_CMD_FRIENDLY_FIRE(1)` to the vest as part of its
      `CMD_START` handler.
    - `reset()` → `bleManager.sendRespawn()` (restores HP/ammo
      without re-running config exchange).
    - `exit()` → `bleManager.endGame()` (CMD_GAME_OVER) and pop
      navigation.
  - Exposes published counters: `hits`, `shots`, `streak`,
    `bestStreak`. Computed `accuracy: Double { hits / shots }`.

- `FreePracticeView.swift` (new)
  - HUD:
    - Big score counter (hits)
    - Shots fired, accuracy %, current streak, best streak
    - Current health (small, peripheral) so a stuck/dead state is
      visible if HP somehow runs out
  - On `BLEManager.onHitDetected` fire (see below): increment hits +
    streak, trigger hit animation overlay.
  - On `BLEManager.onShotFired` fire: increment shots.
  - On streak break (shot without a hit within ~750 ms): reset
    `streak`, optionally show a small "streak broken" hint.
  - Reset and Exit buttons in the top bar.

- `ReactionTimeView.swift` (new)
  - State machine: `.idle` → `.armed` → `.waiting` → `.cued` →
    `.scored` → back to `.armed`.
  - Big "Tap to start round" button in `.idle`. Once tapped:
    - `.armed`: shows "Get ready..." with a slim countdown line
      animating from full to empty over a randomized 1.5–5 s
      interval. **Use `Task.sleep` with a randomly chosen
      duration, not a fixed timer**, so the user can't game it.
    - `.cued`: full-screen high-contrast cue (e.g., screen flashes
      a saturated colour, a big "FIRE" word appears, haptic
      `.heavy` impact, optional short tone). Capture
      `cueTimestamp = ContinuousClock.now`. User physically aims
      and shoots the vest.
    - On `BLEManager.onHitDetected` fire after `cueTimestamp` is
      set: `reactionMs = (now - cueTimestamp).milliseconds`.
      Transition to `.scored`, show big numeric time + a grade
      label (suggested bands: < 250 ms "ELITE", < 350 ms "FAST",
      < 500 ms "AVG", ≥ 500 ms "SLOW"). Append to a session-local
      `times: [Int]` array.
  - Anti-cheat: if a hit arrives **before** the cue (user shot
    early), don't score it as fast — flag the round as
    "JUMPED THE GUN", red-tinted, doesn't count toward stats.
    Same state-machine flow then resets to `.armed`.
  - Session stats panel: best time, average, last 10 times sparkline.
  - Reset (clears the times array) and Exit buttons in the top bar.

- `BLEManager.swift`
  - **Add a hit-detection signal exposed to training views.** Stash
    `previousHealth: UInt8?` (start nil). Inside the
    `.stateUpdate` case, compare new health against previousHealth
    and, if it strictly decreased, fire `onHitDetected?(
      newHealth: UInt8, delta: UInt8)`. Update previousHealth
    afterward. Reset previousHealth to nil on disconnect /
    game-over so the next session starts clean. **Don't diff inside
    the view** — the source of truth stays in `BLEManager`.
  - **Add a shot-fired signal** similarly: cache `previousMagAmmo:
    UInt16?` and fire `onShotFired?()` when it strictly decreased.
    Used by FreePracticeView for accuracy.
  - **Health-increased case:** when health goes up (respawn or
    reset), do NOT fire `onHitDetected`. Same for mag-ammo refill
    after a reload — `onShotFired` only fires on a strict
    decrease.

- `AppFlow.swift`
  - Recommend a new `.training` phase. Two `Equatable` cases
    suffice — adding sub-mode state inside the views is fine, no
    need for `.training(.free)` / `.training(.reaction)` cases.
  - Alternatively keep flow at `.lobby` and present the training
    flow as a `.fullScreenCover`. Either is acceptable — pick the
    one that keeps the diff smaller and back-button behaviour
    cleaner.

- `GameManager.swift`
  - **Do not** route training through `GameManager`. That class is
    the mesh-driven multiplayer lifecycle (lobby, teams, mesh
    messages). Training is single-device. State lives in
    `TrainingSession`.

**Hit animation — showcase-quality, layered:**

The animation isn't an afterthought — it's the visible payoff of the
whole hardware stack, so aim high. Build it as a `ZStack` overlay
composed of independent layers fired in sync. Use SwiftUI's
`.transition` + `withAnimation(.spring(...))` and `TimelineView` for
the multi-frame ramps. **No 3rd-party libraries** — keep it pure
SwiftUI.

Layers (fire all together on hit, each with its own duration):

1. **Radial impact flash** — a coloured radial gradient from screen
   center, opacity 0.85 → 0 over ~250 ms, ease-out. Hue tied to
   streak (cool blue at streak ≤ 5, hot orange ≥ 10, deep red ≥ 20).
2. **Geometric hit marker** — four short chevron/tick segments
   pointing toward center, scale 0 → 1.2 → 1.0 with a small overshoot
   spring, then fade. ~280 ms total. This is the
   competitive-shooter idiom — it should feel *crisp*, not bouncy.
3. **Score popup** — `+1` (or larger numbers when in a streak
   multiplier band) drifts up ~80 pt from the impact point, scales
   from 0.5 → 1.0, fades. ~600 ms. Monospaced numeric font, weight
   .heavy.
4. **Concentric ring** — single stroked circle, scales 0 → 3.0,
   opacity 0.6 → 0, ~400 ms. Adds a satisfying "impact radiates
   outward" feel.
5. **Camera shake** — apply a tiny `.offset` on the root content
   driven by a damped-sine wave for ~150 ms. ±2 pt is plenty;
   anything more feels obnoxious.
6. **Brief slow-mo on big milestones** — when streak hits 5/10/25,
   add a quarter-second `0.5x` time-scale dim by overlaying a dark
   vignette + freezing animation progress. Don't apply to every hit;
   reserve for milestones so it stays a treat.
7. **Haptic** — `UIImpactFeedbackGenerator(style: .heavy)` for
   normal hits, `.notificationOccurred(.success)` from
   `UINotificationFeedbackGenerator` for milestone hits.
8. **Sound (optional, mute toggle in top bar)** — a single short
   "impact" sound effect, played via AVFoundation
   `AVAudioPlayer`. Layer a slightly higher-pitched "ping" for
   milestone hits. Bundle as `.caf` or `.wav` resources. If you
   don't have sources, ship the visual-only version first and
   leave a comment noting where to add audio.

The reaction-time cue animation should be its own thing, not the hit
animation — bigger, simpler, full-screen. A saturated colour wash +
a single large word ("FIRE") in a heavy display font + a `.heavy`
haptic at the moment of the cue. Don't combine the two effects.

**Notes / gotchas:**

- **Detecting a hit cleanly:** the firmware sends
  `RSP_STATE_UPDATE` (0xA0) for any state change — fires reduce
  `magAmmo`, hits reduce `health`, reloads change both. The hit
  signal is **health strictly decreased between consecutive 0xA0
  updates**. Cache the previous health in `BLEManager`.
- **Shots fired vs hits:** `magAmmo` strict-decrement = shot.
  `health` strict-decrement = hit. Reload increases mag (doesn't
  count as either). Respawn restores both (doesn't count).
- **Friendly-fire requirement is non-negotiable.** Without it the
  vest drops own-player hits silently. Easiest mistake to make,
  hardest one to debug.
- **The vest must be paired** before either training mode can
  start. Check `bleManager.vestPaired`. Route to pairing if not.
- **No mesh broadcasts.** Don't send `CMD_BROADCAST_GAME_CFG` or
  any lobby commands. Use only `CMD_CONFIG`, `CMD_START`,
  `CMD_RESPAWN`, `CMD_GAME_OVER`, `CMD_RELOAD`.
- **Respawn behaviour:** the suggested `maxHealth = 255, damage =
  1` config gives ~255 hits before death, plenty for any session.
  If you'd rather use normal HP (e.g. 100/25): wire
  `onDeathNotification` in `BLEManager` to auto-call
  `sendRespawn()` after a 1–2 s delay so the user isn't stuck
  with a non-firing gun. Either approach is fine — pick one and
  document it.
- **Simulator testability:** since the simulator has no
  CoreBluetooth, add a DEBUG-only "trigger mock hit" button on
  both training views that calls the `onHitDetected` closure
  directly with synthetic values. Lets you iterate on the
  animation entirely on the simulator without hardware.
- **Streak-break detection:** for `FreePracticeView`, track the
  last shot's timestamp. If `onShotFired` arrives without
  `onHitDetected` within ~750 ms, the streak breaks. Tune the
  window if it feels too forgiving / too strict on hardware.
- **If something about the animation choreography, sub-mode
  layout, scoring bands, or audio integration is genuinely
  ambiguous after reading this entry — stop and ask** before
  shipping. The point of the showcase is to look intentional, and
  guessing the wrong direction wastes a rebuild.

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
