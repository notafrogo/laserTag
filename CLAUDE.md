# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

NRF52840 Zephyr firmware for a laser-tag system. Two binaries built from this tree:

- `emitter/` — the "gun." BLE peripheral to the phone (`LT-E-XXXX`), BLE central to the vest, IR LED transmitter (PWM 38 kHz), Coded-PHY mesh broadcaster + observer.
- `reciever/` — the "vest." BLE peripheral to the emitter (`LT-V-XXXX`), VS1838B IR receiver.
- `common/` — shared protocol definitions (`protocol.h`), IR encoder/decoder (`ir_protocol.[ch]`).

Companion iOS app lives at `~/Documents/laserTag/` and has its own `CLAUDE.md` covering the app side.

## ‼️ Critical: keeping the iOS app in sync

**Whenever you make a firmware change that affects the phone↔emitter BLE interface, add a `### PENDING` entry to `IOS_SYNC.md` in the SAME commit.**

The phone↔emitter surface area:
- `common/include/protocol.h` — `CMD_*` / `RSP_*` opcodes, payload sizes, packed structs
- `emitter/src/ble_phone.{c,h}` — GATT service / characteristic UUIDs and properties
- `emitter/src/main.c::on_phone_rx` — command/response semantics

`IOS_SYNC.md` is the explicit channel between this repo and the iOS app sync routine. The routine reads pending entries *first*, applies them, then falls back to git-log heuristics for mechanical changes that didn't get a note. If your change is non-obvious and you skip the note, the routine has to guess — which usually means the non-obvious parts get done wrong.

**You need a PENDING note when:**
- Adding an opcode whose UX implication isn't obvious from the bytes
- Changing the meaning of an existing opcode
- A new iOS file / view / tab is required
- The payload shape changes in a way that needs migration on the app side
- Multi-file orchestration on the iOS side ("when X happens, do Y and Z")

**You don't need a note when:**
- Adding an opcode that mirrors an existing pattern (the routine picks these up from the diff)
- Internal-only changes: mesh, IR protocol, vest↔emitter BLE, LED behaviour, build/devicetree, scanner tuning

Entry format is documented at the top of `IOS_SYNC.md`. Keep entries terse — file paths + verbs + nouns, not paragraphs. Always commit the note alongside the firmware change so the SHA→note mapping is unambiguous.

## Board target

**`promicro_nrf52840`**, not the nRF52840 DK. The Pro Micro has a different LED pin (`led0 = P0.15`), no usable on-board J-Link (SWD requires probing castellated pads), a small LDO that's sensitive to concurrent radio + IR LED load, and a board-file `pwm0` claim on the LED pin that you have to disable in the receiver overlay to avoid GPIO contention.

## Build & flash

```bash
cd emitter/    # or reciever/
west build -b promicro_nrf52840 -p
```

The `-p` (pristine) is mandatory after any `app.overlay` or `prj.conf` change — Zephyr caches the merged devicetree and won't pick up overlay edits without it.

Two flash paths:

```bash
# UF2 drag-and-drop: double-tap reset to enter bootloader, then drop:
build/zephyr/zephyr.uf2

# SWD via west (if external J-Link attached):
west flash --erase   # --erase wipes NVS along with app region
```

UF2 only writes the app region — it **cannot** erase NVS. This is why the receiver firmware deliberately doesn't persist pairing across boots (see `IOS_SYNC.md` Applied section / the comment in `reciever/src/main.c::main()` near the settings init).

## File layout

```
common/include/
  protocol.h          phone↔emitter opcodes + emitter↔vest opcodes + mesh types + packed structs
  ir_protocol.h       IR TX/RX API (hit frames + pairing frames)
common/src/
  ir_protocol.c       CRC-8, PWM-driven TX, edge-counting RX with end-of-frame timeout
emitter/src/
  main.c              state machine, phone CMD dispatch, IR/trigger handling, vest connection orchestration
  ble_phone.{c,h}     GATT service for phone, RSP_LOG relay (PLOG_INF/WRN/ERR macros)
  ble_vest.{c,h}      GATT client for vest, GATT discovery state machine
  game_state.{c,h}    health / ammo / reload / kills / deaths
  mesh.{c,h}          Coded PHY broadcaster + observer, dedup ring, relay queue
emitter/app.overlay   trigger button (P0.29), PWM0 → IR LED (P1.04, H0H1 high drive)
reciever/src/
  main.c              vest state machine, IR pairing handler, GATT command dispatch
  vest_ble.{c,h}      GATT service for emitter (Hit TX, Cmd RX)
reciever/app.overlay  IR RX pin (P0.29), pwm0 deliberately disabled
```

## Wire-protocol summary

The full table lives in the iOS repo's `CLAUDE.md`. Key invariants when editing `protocol.h`:

- `CMD_*` = phone→emitter, `RSP_*` = emitter→phone (notify on the emitter TX characteristic)
- `VEST_CMD_*` = emitter→vest (write to vest Cmd RX characteristic)
- `MESH_*` = NRF↔NRF Coded PHY mesh
- Multi-byte fields are little-endian
- Packed structs (`__attribute__((packed))`) use `sys_get_le16` / `sys_put_le16` / `sys_get_le32` / `sys_put_le32` for reads/writes — never reinterpret-cast directly
- Opcode `0x9B` (`RSP_LOG`) is the BLE-relayed firmware log channel; payload is `severity[1] + UTF-8 text`. Use the `PLOG_INF/WRN/ERR` macros in `emitter/src/ble_phone.h` for any log line that should reach the phone — they call both Zephyr `LOG_*` *and* the relay. Be sparing during latency-sensitive windows (e.g. GATT discovery against the vest): too many notifications can fill the host's TX queue and cause iOS to drop the phone link.

## Pro Micro power gotcha

The Pro Micro's regulator is borderline when the radio is running two concurrent connections plus the mesh scanner plus the IR LED. Symptoms are gradual rail droop, missed BLE supervision packets, and connection drops mid-pairing. Mitigations already in place:

- Receiver overlay disables `pwm0` to avoid GPIO contention on P0.15
- IR PWM uses `pwm_set` with nanosecond timing (driver-clock independent)
- `pairing_tx_work_handler` stops firing IR the moment `ble_vest_link_up()` is true
- Mesh scanner is paused from `vest_conn_connected` until `on_vest_gatt_ready` completes
- Receiver pairing is ephemeral (cleared on every boot) to make UF2-only flashing recoverable

If you introduce new concurrent activity (more notifications, faster timers, higher TX power), test for these failure modes on hardware before pushing.

## Diagnostic instrumentation

`ir_protocol.c` has a 1 Hz edge-count log (`IR edges/sec: N`) so you can confirm the TSOP is producing edges even when no valid frames are received. Used during bring-up; leave in place.

`emitter/src/main.c::scan_recv` logs every 32nd received advertisement with `addr`, `rssi`, `primary_phy`, `len` — useful for confirming the scanner is seeing both 1M (vest discovery) and Coded PHY (mesh) traffic.

Both are cheap; remove only if they ever become noisy in a real game session.
