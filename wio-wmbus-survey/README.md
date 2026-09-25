# Wio Tracker L1 Pro – wM-Bus T1 survey

Listens on 868.95 MHz for wM-Bus T1 and C1 telegrams, decodes Diehl IZAR readings and alarms, and keeps a table of every meter heard (any brand: ID, type, signal, position).
The table (up to 512 meters) is saved to the 2 MB QSPI flash and house labels to the internal flash, so a survey can continue across walks. Each walk also adds one row per meter to `history.csv`, and meters whose reading can't be decoded get their raw telegram saved to `raw.csv`. Plugged into a computer, the tracker shows up as a USB drive with these files.

## Build / flash
    pio run
Double-tap reset → a USB drive appears → copy `.pio/build/wio_tracker_l1/firmware.uf2` onto it.
(or: `pio run -t upload`)

First boot formats the 28 KB internal flash area (clears any old Meshtastic settings) and the QSPI flash (FAT12, drive label `WMBUS`). A survey saved in internal flash by an older build is moved to QSPI automatically.
Back to Meshtastic any time via https://flasher.meshtastic.org

## Controls
- Joystick up/down: select meter
- Joystick press: list / detail view
- Joystick left/right (detail view): set house number. Hold to repeat. An unlabelled meter starts next to the last number you used. Stepping to 0 removes the label.
- Joystick left (list view): settings screen (see below; press goes back)
- Joystick right (list view): diagnostics screen (any of left/right/press goes back)
- User button: sort by best RSSI / last seen
- Screen turns off after 2 minutes without a button press (changeable in settings); the next press only wakes it. A new leak or low battery wakes it too.
- Beeps: three short = meter newly reporting a leak; two low = battery below 3.5 V

## Settings
Up/down picks a setting, left/right changes it, press goes back. Saved to the internal flash a few seconds after the last change.
- Bluetooth (default off): lets a phone connect, see below. `linked` = a phone is connected
- GPS: off puts the GPS module in standby, which saves power (e.g. when leaving the tracker by your own meter). No positions are recorded then. Times keep running from the last GPS time, if there was one since power-on
- Screen off: 30 s, 1 min, 2 min, 5 min or never
- Beeps: leak, low battery and start-up beeps
- Sort: same as the user button
- Forget phones: removes every paired phone (Bluetooth must be on). Pair again from the page afterwards

The top line shows the Bluetooth name (`WMBUS-` + 4 characters unique to the tracker).

## Bluetooth (phone page)
`web/index.html` is a page for Chrome on Android (or Chrome/Edge on a computer; iPhone Safari has no Web Bluetooth). It shows the tracker's screen live, has the joystick and user button, a log of what the tracker prints on serial with a box for serial commands, and downloads `survey.csv`, `history.csv` and `raw.csv` without a cable. The tracker keeps listening while it does.

1. Settings → Bluetooth → on
2. Open the page (it has to come from an `https://` address, e.g. GitHub Pages, for the browser to allow Bluetooth), press Connect and pick `WMBUS-xxxx`
3. The first time, the tracker shows a 6-digit code and the phone asks for it. After that the phone reconnects without it

With a phone watching, the screen is kept up to date for it even while the tracker's own screen is off, and buttons pressed on the page don't turn the tracker's screen on. The link needs the pairing code, so nobody else nearby can connect. The console uses the standard Nordic UART service, so BLE serial terminal apps (e.g. nRF Toolbox, Serial Bluetooth Terminal) work too once paired.

## List view
Top line: `N48  S9  3.92V` = meters in the table, GPS satellites in view (`S-` = GPS switched off), battery (`LOW` below 3.5 V), `BT` while a phone is connected.

`#12     L   12.345  -71*`
- Label (or meter ID if unlabelled)
- Flag: `L` leaking now, `l` leaked previously, `!` other alarm
- Reading in m³, or manufacturer + device type (e.g. `KAM cold`) for meters whose reading isn't decoded
- Best RSSI; `*` = not heard since power-on (values from the saved survey)

## Diagnostics screen
Frames decoded ok / failed (`err` always climbs a little: noise matching the sync word), GPS satellites and fix, battery, whether a computer has the USB drive, QSPI flash and save ring, snapshot count and labels, log bytes waiting to be appended, uptime. The serial `s` command shows the same and more.

## Other meters
Sensus iPERL readings are decoded with the public default key, or a per-meter key set with `k`. Their alarms come from the standard OMS status byte: `battLow` (power low) and `error` (permanent or temporary error). Meters that can't be decoded show manufacturer + type and get raw telegrams in `raw.csv`.

## Position
Each meter keeps its 5 strongest GPS-tagged receptions. Only receptions with a fresh fix count (at most 1.5 s old, HDOP 2.5 or better; `GPS_MAX_AGE_MS`/`GPS_MAX_HDOP` in main.cpp). The position is a signal-weighted average of those, and `+-Xm` is how spread out they are (smaller = more trustworthy). Walk past on both sides for the best estimate.

## Saving
- Survey → QSPI flash: at most every 30 s, when something worth keeping changed (new meter, reading, alarms, better position). Snapshots are written in turn through a hidden `state.bin` (1 MB, or 512/256 KB if the flash has no free 1 MB run), so no part of the flash wears faster than the rest, and a power cut mid-save falls back to the previous snapshot.
- Log rows wait inside the snapshots and are appended to `history.csv`/`raw.csv` when a computer is plugged in (or a buffer is ¾ full), since each append rewrites the FAT. `h`/`r` on serial show them either way.
- Labels (and keys) → internal flash: 3 s after the last edit

## Files
- `survey.csv`: one row per meter, latest state (same columns as the serial `d` dump, strongest meters first). IZAR meters also report `billing_litres` + `billing_date` (the reading the meter stored on the billing date the water company set, 31 Dec on ours, so reading − billing = use since then; blank until a new meter reaches its first billing date; wmbusmeters calls these "last month"), `battery_years` left and `period_s` between broadcasts.
- `history.csv`: one row per meter per walk: `utc,id,label,mfct,type,litres,billing_litres,billing_date,alarms,battery_years,rssi` (files started before these were renamed keep the old `last_month_*` header names; the columns are the same). A meter heard again within 6 h counts as the same walk, even across a power cycle; an alarm change always adds a row. Rows need GPS time, so meters heard before the first fix are logged when it arrives, stamped with that time.
- `raw.csv`: for meters whose reading isn't decoded, one raw telegram per walk (hex, CRCs removed, the form wmbusmeters accepts), with the radio mode. Before a GPS fix the time is left blank.

Rows are collected in RAM (and saved in the survey snapshots) and appended to the files when a computer is plugged in. Around 50 houses walked weekly is ~150 KB of history a year; the ~1 MB left beside `state.bin` holds about six years of that.

## USB drive
Plug into a computer and a read-only drive called `WMBUS` appears with the files above. `survey.csv` is written, and waiting log rows appended, at the moment you plug in.

While a computer has the drive the tracker keeps receiving and saving the survey, but new log rows wait until it's unplugged (changing files underneath the computer would confuse it). To get newer files, eject and replug.

## Serial (115200, line-based)
    pio device monitor
The same commands work from the command box on the phone page.
- `s` status: build date, QSPI flash, save ring, USB drive, battery, frame counters, GPS reception (sentences ok/bad, fix age, HDOP), Bluetooth and settings
- `d` dump table as CSV (label, mode T1/C1a/C1b, manufacturer, type, reading, alarms, UTC, lat, lon, spread)
- `c` clear survey (labels and history kept)
- `h` print history.csv, `r` print raw.csv (not while a computer has the drive: open the files there)
- `HCLEAR` delete history.csv and raw.csv
- `l <id> <label>` set label, e.g. `l 1a2b3c4d 12A`; `l <id>` removes it (a stored key is kept)
- `L` list labels (`,key` = meter has an AES key)
- `k <id> <32 hex digits>` set a meter's AES key (e.g. a Sensus with a non-default key); `k <id>` clears it
- `FORMAT` erase everything (internal + QSPI flash) and reboot

## Host tests
Decoder (`wmbus.h`), and survey storage + CSV rows (`snapring.h`, `survey.h`: snapshot ring incl. wrap-around and power cuts mid-save, timestamps, position estimate, column counts):

    g++ -std=c++17 -Isrc test/test_host.cpp -o t && ./t
    g++ -std=c++17 -Isrc test/test_survey.cpp -o ts && ./ts

GitHub Actions runs both, builds the firmware and compiles the ESPHome configs on every push. Pushing a tag `v*` publishes a GitHub release with this firmware as `wio-tracker-survey.uf2`, plus the Heltec and XIAO survey firmware.

## If it doesn't work
- Screen blank → check serial; the OLED address is auto-detected (0x3C/0x3D). Garbled → set `DISPLAY_SH1106 0` in main.cpp
- On the diagnostics screen `err` always climbs a little (random noise matching the sync word). If `ok` stays 0 while your meter is transmitting, compare with the Heltec
- `# survey saved ... FAILED` on serial → send `c` or `FORMAT`
- "QSPI error: small survey" on screen → the QSPI flash didn't start; the survey falls back to internal flash, which only has room for roughly 100 meters
- No `WMBUS` drive → check it's a data cable; the drive only appears a few seconds after boot. The boot screen shows the build date and `flash ok` / `FLASH FAIL`; `s` on serial gives the details. With a flash failure Windows shows a removable disk with no media
- Nothing at all → check `radio init failed` on serial
- Page can't find the tracker → Bluetooth on in settings? Only one phone can be connected at a time. After "Forget phones" (or a `FORMAT`), also remove `WMBUS-xxxx` from the phone's Bluetooth settings before pairing again
- "no position yet" in detail view → that meter hasn't been heard since the GPS got a fix (the `S` number on the list view is satellites in view, and the diagnostics screen says whether there's a fix; first fix outdoors can take up to 15 min)
- "flash error: no saving" → send `FORMAT`
