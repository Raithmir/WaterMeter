# Wio Tracker L1 Pro – wM-Bus T1 survey

Listens on 868.95 MHz for wM-Bus T1 and C1 telegrams, decodes Diehl IZAR readings and alarms, and keeps a table of every meter heard (any brand: ID, type, signal, position).
The table (up to 512 meters) is saved to the 2 MB QSPI flash and house labels to the internal flash, so a survey can continue across walks. Each walk also adds one row per meter to `history.csv`, and meters whose reading can't be decoded get their raw telegram saved to `raw.csv`. Plugged into a computer, the tracker shows up as a USB drive with these files.

<img width="3064" height="4080" alt="PXL_20260926_064548099" src="https://github.com/user-attachments/assets/662a8164-85dc-4df8-b418-35ad8acd4fa6" />
<img width="3064" height="4080" alt="PXL_20260926_064600662" src="https://github.com/user-attachments/assets/59fe5693-1595-4612-be4a-9b03093528a0" />

## Build / flash
Ready-made: download `wio-tracker-survey.uf2` from the [latest release](https://github.com/Raithmir/WaterMeter/releases/latest), double-tap reset → a USB drive appears → copy the file onto it.

From source:

    pio run
Double-tap reset → a USB drive appears → copy `.pio/build/wio_tracker_l1/firmware.uf2` onto it.
(or: `pio run -t upload`)

First boot formats the 28 KB internal flash area (clears any old Meshtastic settings) and the QSPI flash (FAT12, drive label `WMBUS`). A survey saved in internal flash by an older build is moved to QSPI automatically.
Back to Meshtastic any time via https://flasher.meshtastic.org

## Controls
- Joystick up/down: select meter
- Joystick press: list / detail view
- Joystick left/right (detail view): set house number. Hold to repeat. An unlabelled meter starts next to the last number you used. Stepping to 0 removes the label.
- Detail view, second line: for IZAR meters made by Sappel (manufacturer `SAP`), the number printed on the meter, e.g. `H25XA036488`, so you can check you're labelling the right one. Other meters show manufacturer and radio mode there.
- Detail view, `used 1234 l/6d 205 l/d`: water used since the meter's last walk (from the history rows), over how long, and per day. A steady high rate with no leak alarm is worth a look (a dripping overflow, say). It shows from a meter's second walk. `pos 5 fixes +-4m` = position quality; the coordinates are in `survey.csv` and on the phone map
- Joystick left (list view): settings screen (see below; right goes back)
- Joystick right (list view): diagnostics screen (any of left/right/press goes back)
- User button (detail view): hunt page on/off, see below
- User button (list view): sort by RSSI / last seen. Either way, meters heard since power-on come first; RSSI sorts those by their latest signal (so the list follows you as you walk) and the rest by their saved best.
- Screen turns off after 2 minutes without a button press (changeable in settings); the next press only wakes it. A new leak or low battery wakes it too.
- Beeps: three short = meter newly reporting a leak; two low = battery below 3.5 V; on the hunt page, one per telegram, higher the stronger

## Settings
Up/down picks a setting (the list scrolls), press changes it (screen off steps through its choices), right goes back. Saved to the internal flash a few seconds after the last change.
- Bluetooth (default off): lets a phone connect, see below. `linked` = a phone is connected
- GPS: off puts the GPS module in standby, which saves power (e.g. when leaving the tracker by your own meter). No positions are recorded then. Times keep running from the last GPS time, if there was one since power-on
- Screen off: 30 s, 1 min, 2 min, 5 min or never
- Beeps: leak, low battery, start-up and hunt page beeps
- Sort: same as the user button
- Show: which meters the list shows: all, not heard (since power-on: what's left of the walk) or unlabelled. The selected meter stays in the list until you move off it, so it doesn't vanish the moment it's heard or labelled
- Forget phones: removes every paired phone (Bluetooth must be on). Pair again from the page afterwards

The top line shows the Bluetooth name (`WMBUS-` + 4 characters unique to the tracker).

## Bluetooth (phone page)
The phone page is at **https://raithmir.github.io/WaterMeter/wio-wmbus-survey/web/** (source in `web/`). It works in Chrome on Android, or Chrome/Edge on a computer; iPhone Safari has no Web Bluetooth.

<img width="1080" height="1964" alt="Screenshot_20260926-074930~2" src="https://github.com/user-attachments/assets/024a4c6b-246e-44da-b1d3-19bd870c7f35" />

- Device view: the tracker's screen live on a photo of the tracker; tap around the joystick to push it, its middle to press, and the user button. Plain view: a big screen with arrow buttons
- Log: what the tracker prints on serial, with a box for serial commands
- Files: downloads `survey.csv`, `history.csv`, `raw.csv` and `track.csv` without a cable, and deletes the three logs one at a time (Delete asks first; download before deleting to keep a copy). The tracker keeps listening while it does
- Map: every meter at its estimated position on OpenStreetMap, coloured by leak/alarm/labelled (Status) or litres per day since the last walk (Use), with a circle for its spread; tap one for its house, serial, reading, use, alarms and signal, and a link to its Usage chart. "From tracker" loads the survey and the walk track over Bluetooth; "Open files" takes `survey.csv` and/or `track.csv` from the USB drive and needs no Bluetooth (so any browser works, iPhone too). The track shows as blue lines, one per walk; Last walk / All walks picks which. The map itself needs internet
- Usage: one meter's litres per day between walks, as a bar chart (tap a bar for its dates and litres) and a table of readings, with the average over all of them. Loads `history.csv` from the tracker or from a file, like the map. Rows within 6 hours count as one walk

1. Settings → Bluetooth → on
2. Open [the page](https://raithmir.github.io/WaterMeter/wio-wmbus-survey/web/), press Connect and pick `WMBUS-xxxx`
3. The first time, the tracker shows a 6-digit code and the phone asks for it. After that the phone reconnects without it

With a phone watching, the screen is kept up to date for it even while the tracker's own screen is off, and buttons pressed on the page don't turn the tracker's screen on. The link needs the pairing code, so nobody else nearby can connect. The console uses the standard Nordic UART service, so BLE serial terminal apps (e.g. nRF Toolbox, Serial Bluetooth Terminal) work too once paired.

## List view
Top line: `41/48 S9 3.92V` = meters heard since power-on / meters in the table (with Show set to not heard: `7 left`; unlabelled: `5 unlab`), GPS satellites used in the fix (`S-` = GPS switched off), battery (`LOW` below 3.5 V), `BT` while a phone is connected.

`#12     L   12.345  -71*`
- Label (or meter ID if unlabelled)
- Flag: `L` leaking now, `l` leaked previously, `!` other alarm
- Reading in m³, or manufacturer + device type (e.g. `KAM cold`) for meters whose reading isn't decoded
- Last RSSI (the detail view also shows the best); `*` = not heard since power-on (values from the saved survey)

## Hunt page
For finding which house a meter belongs to. From a meter's detail view, press the user button:

    217e06c8  House 12
         -64 dBm
    [██████████|░░░░░]
    peak -58  x14
    seen 2s ago

The big number and bar are the latest telegram's RSSI (bar from -110 to -40 dBm). The tick and `peak` are the strongest since you opened the page, so you can walk past and come back to where it peaked. Each telegram also beeps, higher the stronger (off with the Beeps setting). `(lost?)` after the time = no telegram for over twice the meter's broadcast interval (IZAR meters say what that is). Up/down hunts the next meter, left/right still labels, the user button goes back to the details and press to the list.

## Diagnostics screen
Frames decoded ok / failed (`err` always climbs a little: noise matching the sync word), GPS satellites and fix, battery, whether a computer has the USB drive, QSPI flash and save ring, snapshot count and labels, log bytes waiting to be appended, uptime. The serial `s` command shows the same and more.

## Other meters
Sensus iPERL readings are decoded with the public default key, or a per-meter key set with `k`. Their alarms come from the standard OMS status byte: `battLow` (power low) and `error` (permanent or temporary error). Meters that can't be decoded show manufacturer + type and get raw telegrams in `raw.csv`.

## Position
Each meter keeps its 5 strongest GPS-tagged receptions. Only receptions with a fresh fix count (at most 1.5 s old, HDOP 2.5 or better; `GPS_MAX_AGE_MS`/`GPS_MAX_HDOP` in main.cpp). The position is a signal-weighted average of those, and `+-Xm` is how spread out they are (smaller = more trustworthy). Walk past on both sides for the best estimate. The GPS is set to use GPS, BeiDou and GLONASS satellites (the L76K uses only the first two by default), so more of them are in view between houses.

## Saving
- Survey → QSPI flash: at most every 30 s, when something worth keeping changed (new meter, reading, alarms, better position). Snapshots are written in turn through a hidden `state.bin` (1 MB, or 512/256 KB if the flash has no free 1 MB run), so no part of the flash wears faster than the rest, and a power cut mid-save falls back to the previous snapshot.
- Log rows wait inside the snapshots and are appended to `history.csv`/`raw.csv` when a computer is plugged in (or a buffer is ¾ full), since each append rewrites the FAT. `h`/`r` on serial show them either way.
- Labels (and keys) → internal flash: 3 s after the last edit

## Files
- `survey.csv`: one row per meter, latest state (same columns as the serial `d` dump, strongest meters first). IZAR meters also report `billing_litres` + `billing_date` (the reading the meter stored on the billing date the water company set, 31 Dec on ours, so reading − billing = use since then; blank until a new meter reaches its first billing date; wmbusmeters calls these "last month"), `battery_years` left and `period_s` between broadcasts. `used_litres`, `used_days` and `litres_per_day` are the use since the meter's last walk, as on the detail view (blank until its second walk). `serial` is the number printed on the meter (e.g. `H25XA036488`, see [Printed serial numbers](#printed-serial-numbers-izar)); only Sappel-made IZAR meters (manufacturer `SAP`) carry it, so it's blank for the rest.
- `history.csv`: one row per meter per walk: `utc,id,label,mfct,type,litres,billing_litres,billing_date,alarms,battery_years,rssi,serial` (files started before these were renamed keep the old `last_month_*` header names; the columns are the same. Files started before `serial` was added keep their header without it, and newer rows have it as an extra last column). A meter heard again within 6 h counts as the same walk, even across a power cycle; an alarm change always adds a row. Rows need GPS time, so meters heard before the first fix are logged when it arrives, stamped with that time.
- `track.csv`: where you walked, `utc,lat,lon`, a point every 5 m (`TRACK_STEP_M`) while the GPS has a fix with HDOP 5 or better (`TRACK_MAX_HDOP`; looser than for meter positions, so the track starts sooner after leaving home). Points wait in RAM and are appended once you have stood still for a minute (so switch off after getting home, not on the doorstep), at most 5 minutes later while walking, or when a computer is plugged in. A power cut mid-walk can lose the last few minutes of it.
- `raw.csv`: for meters whose reading isn't decoded, one raw telegram per walk (hex, CRCs removed, the form wmbusmeters accepts), with the radio mode. Before a GPS fix the time is left blank.

History and raw rows are collected in RAM (and saved in the survey snapshots) and appended to the files when a computer is plugged in. Around 50 houses walked weekly is ~150 KB of history a year, and a 1 km walk adds ~8 KB of track (~400 KB a year weekly); the ~1 MB left beside `state.bin` holds nearly two years of both. `HCLEAR` starts afresh, or `HCLEAR t` (Delete beside track.csv on the phone page) clears just the track.

## Printed serial numbers (IZAR)
Sappel-made IZAR meters (manufacturer `SAP`) don't broadcast a separate serial number. Their printed number is packed into the radio address, so the tracker works it out from the header, the same way wmbusmeters' `izar` driver does. It appears in `survey.csv`, `history.csv`, the `d` dump and on the meter's detail screen. Other meters leave it blank.

    H 25 X A 036488
    │ │  │ │ └ serial, 6 digits
    │ │  │ └ diameter code letter
    │ │  └ meter type code letter
    │ └ year made (2025)
    └ supplier code letter

The letters are the maker's codes; the tracker only reproduces them, it doesn't say what they stand for. Meters from the same batch share a prefix, e.g. all the meters on our street start with `H25VA`.

**Meter ID and header → serial.** The tracker's `id` (e.g. `217e06c8`) is the 4 address bytes as wmbusmeters prints them. `ver` and `type` are the next two header bytes, which SAP frames reuse for the letters. So on these meters the `type` shown (`water (04)`) comes from the supplier letter, not from a real device type. Letters count from `A` = 1:

- year and serial = `id & 0x03FFFFFF` in decimal, 8 digits: the first 2 are the year, the last 6 the serial (`0x017e06c8` = 25036488 → `25`, `036488`)
- supplier = `((type & 0x0F) << 1) | (ver >> 7)` (4, 0x60 → 8 = `H`)
- type letter = `(ver & 0x7C) >> 2` (0x60 → 24 = `X`)
- diameter = `((ver & 0x03) << 3) | (id >> 29)` (0x60, 0x217e06c8 → 1 = `A`)

**Serial → meter ID.** This lets you find a meter in the survey from the number on its label. The [meter ID converter](https://raithmir.github.io/WaterMeter/meter-id/) does this for you (source in [`meter-id/`](../meter-id/)). With supplier `s`, type letter `k` and diameter `d` as numbers (`A` = 1) and `n` = year × 1000000 + serial:

- `id = ((d & 7) << 29) | n`: `H25XA036488` → `(1 << 29) | 25036488` = `217e06c8`
- `ver = ((s & 1) << 7) | (k << 2) | (d >> 3)` = `60`, and the low 4 bits of `type` = `s >> 1` = `4`

In practice only the ID matters. For a diameter letter `A`, `id` = `0x20000000 + n`. For example, `H25VA994697` → 25994697 = `0x018ca5c9` → `218ca5c9` (Python: `'%08x' % (0x20000000 + 25994697)`).

## USB drive
Plug into a computer and a read-only drive called `WMBUS` appears with the files above. `survey.csv` is written, and waiting log rows appended, at the moment you plug in.

While a computer has the drive the tracker keeps receiving and saving the survey, but new log rows wait until it's unplugged (changing files underneath the computer would confuse it). To get newer files, eject and replug.

## Serial (115200, line-based)
    pio device monitor
The same commands work from the command box on the phone page.
- `s` status: build date, QSPI flash, save ring, USB drive, battery, frame counters, GPS reception (sentences ok/bad, fix age, HDOP), how the GPS started since it last woke (seconds to first fix, to a fix good enough for the track (HDOP 5) and for meter positions (HDOP 2.5), when bytes were first sent with `G`, GLONASS satellites in view or `not seen` if the GLONASS setting didn't take), Bluetooth and settings
- `d` dump table as CSV (label, printed serial, mode T1/C1a/C1b, manufacturer, type, reading, alarms, UTC, lat, lon, spread)
- `c` clear survey (labels and history kept)
- `h` print history.csv, `r` print raw.csv, `t` print track.csv (not while a computer has the drive: open the files there)
- `G <hex>` pass up to 100 bytes to the GPS, e.g. a CASIC command for testing; its binary replies show as `# gps <class> <id>: <payload hex>`. `G` alone answers `# G ready`, `# G fix` (has a good fix) or `# G off`
- `GN` echo the GPS's NMEA sentences to the console, until `GN` again
- `HCLEAR` delete history.csv, raw.csv and track.csv; `HCLEAR h`, `HCLEAR r` or `HCLEAR t` deletes just that one (also the Delete buttons in the phone page's Files tab). Not while a computer has the drive
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
- "no position yet" in detail view → that meter hasn't been heard since the GPS got a fix (the `S` number on the list view is satellites used in the fix, and the diagnostics screen says whether there's a fix). Switched on outdoors, the first fix takes about 30 s and the track starts about 10 s later; indoors or in a pocket between houses it takes minutes, so switch on a minute or two before leaving, by a window. The GPS forgets the satellites when switched off. Sending it orbit data, the time and a position (assisted GPS) was tried: its firmware (URANUS5 V5.3.0.0) acknowledges them but ignores them, even with current data
- "flash error: no saving" → send `FORMAT`
