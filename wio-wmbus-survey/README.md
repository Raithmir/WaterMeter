# Wio Tracker L1 Pro – wM-Bus T1 survey

Listens on 868.95 MHz for wM-Bus T1 and C1 telegrams, decodes Diehl IZAR readings and alarms, and keeps a table of every meter heard (any brand: ID, type, signal, position).
The table and house labels are saved to the internal flash, so a survey can continue across walks.

## Build / flash
    pio run
Double-tap reset → a USB drive appears → copy `.pio/build/wio_tracker_l1/firmware.uf2` onto it.
(or: `pio run -t upload`)

First boot formats the 28 KB internal flash area (clears any old Meshtastic settings).
Back to Meshtastic any time via https://flasher.meshtastic.org

## Controls
- Joystick up/down: select meter
- Joystick press: list / detail view
- Joystick left/right (detail view): set house number. Hold to repeat. An unlabelled meter starts next to the last number you used. Stepping to 0 removes the label.
- User button: sort by best RSSI / last seen
- Beeps: short = meter first heard this session, three long = meter newly reporting a leak

## List view
`#12     L   12.345  -71*`
- Label (or meter ID if unlabelled)
- Flag: `L` leaking now, `l` leaked previously, `!` other alarm
- Reading in m³, or manufacturer + device type (e.g. `KAM cold`) for meters whose reading isn't decoded
- Best RSSI; `*` = not heard since power-on (values from the saved survey)

## Position
Each meter keeps its 5 strongest GPS-tagged receptions. The position is a signal-weighted average of those, and `+-Xm` is how spread out they are (smaller = more trustworthy). Walk past on both sides for the best estimate.

## Saving
The save format changed in this version, so any survey saved by the previous build is ignored (labels are kept).

- Survey: at most every 30 s while new data arrives
- Labels: 3 s after the last edit

## Serial (115200, line-based)
    pio device monitor
- `d` dump table as CSV (label, mode T1/C1a/C1b, manufacturer, type, reading, alarms, UTC, lat, lon, spread)
- `c` clear survey (labels kept)
- `l <id> <label>` set label, e.g. `l 217e06c8 12A`; `l <id>` removes it
- `L` list labels
- `FORMAT` erase everything on flash and reboot

## Host test of the decoder
    g++ -std=c++17 -Isrc test/test_host.cpp -o t && ./t

## If it doesn't work
- Screen blank → check serial; the OLED address is auto-detected (0x3C/0x3D). Garbled → set `DISPLAY_SH1106 0` in main.cpp
- `er` always climbs a little (random noise matching the sync word). If `ok` stays 0 while your meter is transmitting, compare with the Heltec
- `# survey saved ... FAILED` on serial → flash area full; send `c` or `FORMAT`
- Nothing at all → check `radio init failed` on serial
- "flash error: no saving" → send `FORMAT`
