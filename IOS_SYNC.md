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

*(none right now — add new entries here)*

---

## Applied

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
