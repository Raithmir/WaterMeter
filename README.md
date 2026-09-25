# wM-Bus Water Meter Reader

ESPHome configs for reading a Diehl IZAR water meter (IZAR RC 868 i W R4 clip-on module) over wireless M-Bus and publishing readings to Home Assistant.

Built on [SzczepanLeon/esphome-components](https://github.com/SzczepanLeon/esphome-components) (`wmbus_common`, `wmbus_radio`, `wmbus_meter`).

## Configs

| File | Hardware | Purpose |
|---|---|---|
| `water-meter-survey.yaml` | Heltec WiFi LoRa 32 V3 | Survey: logs every wM-Bus frame to find your meter ID |
| `water-meter-survey-xiao.yaml` | Seeed XIAO ESP32S3 + Wio-SX1262 kit | Survey: the same, headless (results in the logs and Home Assistant) |
| `water-meter-gateway.yaml` | Heltec WiFi LoRa 32 V3 | Reader: fixed gateway with OLED display |
| `water-meter-gateway-xiao.yaml` | Seeed XIAO ESP32S3 + Wio-SX1262 kit | Reader: headless gateway, lower-cost hardware |

Rename the files in the table if yours differ.

## Meter details

| Item | Value |
|---|---|
| Driver | `izar` |
| Mode | T1 (868.95 MHz) |
| Encryption | None |
| Transmit interval | ~8 s |

## Hardware

### Heltec WiFi LoRa 32 V3

ESP32-S3 + SX1262, with an onboard SSD1306 OLED.

| Signal | GPIO |
|---|---|
| SPI SCK / MOSI / MISO | 9 / 10 / 11 |
| CS / RESET / BUSY / DIO1 | 8 / 12 / 13 / 14 |
| OLED SDA / SCL / RST | 17 / 18 / 21 |
| Vext (OLED power, active low) | 36 |
| PRG button | 0 |

### Seeed XIAO ESP32S3 + Wio-SX1262 kit

This kit costs about half as much as the Heltec. It has the same radio chip, no display, and built-in LiPo charging. The radio connects over the B2B connector.

| Signal | GPIO |
|---|---|
| SPI SCK / MOSI / MISO | 7 / 9 / 8 |
| CS / RESET / BUSY / DIO1 | 41 / 42 / 40 / 39 |
| Antenna switch enable (RF_SW) | 38, held high |
| User LED (active low) | 21 |

Pin sources: Seeed's [one_channel_hub BSP](https://github.com/Seeed-Studio/one_channel_hub/blob/4cc771ac02da1bd18be67509f6b52d21bb0feabd/components/smtc_ral/bsp/sx126x/seeed_xiao_esp32s3_devkit_sx1262.c) and the [Wio-SX1262 datasheet](https://files.seeedstudio.com/products/SenseCAP/Wio_SX1262/Wio-SX1262_Module_Datasheet.pdf).

Notes:
- DIO2 selects transmit or receive (`rf_switch: true`), and GPIO38 enables the switch.
- The TCXO is powered from DIO3 (`has_tcxo: true`). Seeed uses 3.0 V; the module accepts 1.7–3.3 V.
- Use the bundled LoRa antenna. The FPC antenna is for testing only.
- The kit ships with Meshtastic installed, so the first flash must be over USB.

## Usage

### New to ESPHome?

[ESPHome](https://esphome.io) turns a YAML file into firmware for the board. You need:

- One of the boards above and a USB-C **data** cable (some cables only charge).
- ESPHome, installed one of two ways:
  - **In Home Assistant:** install the ESPHome Device Builder add-on (Settings → Add-ons). Create a new device, replace its YAML with the contents of the config from this repo, then choose Install.
  - **On your computer:** `pip install esphome`, then use the `esphome` commands shown below.

Every config has placeholders to fill in before you compile:

| Placeholder | Replace with |
|---|---|
| `YOUR_WIFI_SSID` | Your Wi-Fi network name |
| `YOUR_WIFI_PASSWORD` | Your Wi-Fi password |
| `YOUR_METER_ID` | Your meter ID with a `0x` prefix, e.g. `0x2124589C` (gateway configs only; see step 2) |

If you leave the Wi-Fi placeholders in, or the board can't join your network, it starts a hotspot named `Water Meter ... Setup`. Join it from your phone and a setup page asks for your Wi-Fi details. [web.esphome.io](https://web.esphome.io) can also set them over USB (see step 1).

The first flash of a new board must be over USB. After that, ESPHome can update it over Wi-Fi (OTA). Once the board is on your Wi-Fi, Home Assistant should discover it under Settings → Devices & services. Add it there to get the entities.

### 1. Survey: find your meter ID

Every wM-Bus meter broadcasts an 8-character hexadecimal ID, for example `2124589C`. The gateway only decodes the meter whose ID you give it, so you need this ID first.

- The ID is **not** the serial number printed on the meter, so you can't read it off the meter body. Listen for it with the survey config.
- Your neighbours' meters broadcast too, so expect to see several IDs.

#### Quickest: prebuilt survey firmware

You don't need to install ESPHome to find the ID. The survey needs no meter ID, so there's ready-made firmware for it. Use Chrome or Edge on a computer:

1. Download the survey firmware for your board from the [latest release](https://github.com/Raithmir/WaterMeter/releases/latest): `water-meter-survey.factory.bin` for the Heltec or `water-meter-survey-xiao.factory.bin` for the XIAO. (For the Wio Tracker L1, use `wio-tracker-survey.uf2` and follow the [Wio instructions](wio-wmbus-survey/).)
2. Plug the board in over USB, open [web.esphome.io](https://web.esphome.io) and choose **Connect**, then pick the board's port. If no port appears or the connection fails, put the board in bootloader mode first (see [Flashing](#flashing)).
3. Choose **Install**, select the `.bin` file you downloaded and wait for the upload to finish. Press RST (Heltec) or unplug and replug (XIAO) afterwards.
4. Optional: choose **Connect** again, then **Configure Wi-Fi** to put the board on your network so Home Assistant can find it. Without Wi-Fi, the survey still works over USB.
5. Choose **Logs** and carry on from step 2 below.

#### Building it yourself

1. Put your Wi-Fi details in the survey config for your board (`water-meter-survey.yaml` for the Heltec, `water-meter-survey-xiao.yaml` for the XIAO), plug the board in over USB and run:

   ```bash
   esphome run water-meter-survey.yaml
   ```

   (Use the XIAO file name instead if that's your board.) Pick the USB port when asked. Once the upload finishes, the command keeps showing the device logs. To watch the logs again later, run `esphome logs` with the same file name.
2. Take the board close to your water meter and watch for lines like this:

   ```
   [I][survey:...]: NEW meter 2124589C  -58dBm  b8=..  (total 1)
   ```

   `2124589C` is the meter ID. `NEW meter` lines only appear for Diehl meters heard twice at -70 dBm or stronger. The `FRAME` lines log every frame, from any brand and at any signal strength.
3. The same information appears in other places:
   - **Heltec OLED:** the large text shows the ID of the strongest meter heard in the last few seconds. Short press PRG to step through the found meters, and long press to clear the list.
   - **XIAO LED:** blinks for each frame from a Diehl meter at -70 dBm or stronger, so it blinks more as you get close to one.
   - **Home Assistant:** the `Survey Found Meters` sensor lists the found meters as `ID:RSSI:b8`. The XIAO survey also has `Survey Strongest Meter` and `Survey Strongest RSSI` (the strongest meter heard in the last few seconds, like the Heltec's OLED) and a `Survey Clear Found Meters` button.
4. Your meter is usually the one whose signal (RSSI, in dBm, closer to 0 is stronger) rises clearly above the rest when you hold the board next to it. On an estate with identical properties, the strongest signal is not always yours, so confirm it in one of two ways:
   - **Check the serial number:** copy the `HEX:` value from a `FRAME` line for that meter into the [wmbusmeters analyzer](https://wmbusmeters.org/analyze/). Compare the decoded `prefix` and `serial_number` with the markings on the meter, and `total_m3` with the dial.
   - **Test it in the gateway:** follow step 2 with that ID and check that Water Total matches the dial.

Don't try to read the ID from the raw hex. The ID bytes are stored in reverse order, so `2124589C` appears in the frame as `...304C9C582421...`. Use the ID the survey prints.

Have the Wio Tracker L1? The [`wio-wmbus-survey`](wio-wmbus-survey/) firmware lists the same IDs on its screen and in `survey.csv`.

### 2. Reader: add the ID to the gateway

Open the gateway config (`water-meter-gateway.yaml` or `water-meter-gateway-xiao.yaml`), fill in your Wi-Fi details, and replace `YOUR_METER_ID` with your ID. Put `0x` in front of the ID:

```yaml
wmbus_meter:
  - id: water_meter         # leave this as it is: the config's internal name
    meter_id: 0x2124589C    # 0x + the 8 characters from the survey
```

- **Put `0x` in front of the ID.** ESPHome reads `meter_id` as a number, and `0x` tells it the number is hexadecimal:
  - Without `0x`, an ID that contains only digits, such as `12345678`, is read as a decimal number. That listens for a different meter (`0xBC614E`), so the config compiles but the gateway never receives a reading.
  - Without `0x`, an ID that contains letters fails with `Expected integer, but cannot parse ... as an integer`.
- Use the ID from the survey, not the serial number printed on the meter.
- Upper or lower case both work, and quotes are optional.
- Don't change `id: water_meter`. The rest of the config uses that name to find the meter.

Then compile and flash, over USB the first time:

```bash
esphome run water-meter-gateway.yaml
```

Within a minute or so the logs should show a reading, and Water Total should appear in Home Assistant. If nothing arrives, check the ID first. After the first flash, you can send updates over OTA.

### Entities exposed to Home Assistant

- **Sensors:** water total (m³, `total_increasing`), last month total, battery life (years), meter RSSI, transmit period, gateway uptime, WiFi signal.
- **Text sensors:** current alarms, previous alarms, last month reading date, ESPHome version.
- **Binary sensors:** water leak, meter fault (any alarm), and stale (no telegram for 6 hours, counted from boot if none has arrived yet).

For daily or monthly consumption, point a Utility Meter helper at Water Total.

### Heltec OLED controls

- Short press PRG: cycle pages (main reading, diagnostics, alarm history).
- Long press PRG: display on/off.

### XIAO status LED

In the gateway, the yellow user LED flashes on each received telegram from your meter, roughly every 8 seconds.

## Flashing

**Heltec V3:** to enter the bootloader, hold PRG and press RST. To flash a compiled image manually:

```bash
esptool --chip esp32s3 --baud 460800 write-flash 0x0 firmware.factory.bin
```

**XIAO ESP32S3:** hold BOOT while plugging in USB, then flash as above.

Both boards can also be flashed from the browser at [web.esphome.io](https://web.esphome.io) with a `.factory.bin` file (see step 1 under Usage).

## Troubleshooting

| Symptom | Check |
|---|---|
| Gateway runs but never shows a reading | `meter_id` has the `0x` prefix and matches the survey ID (not the serial on the meter) |
| `Expected integer, but cannot parse ...` | `meter_id` is missing its `0x` prefix |
| Compile error: `logger` shadowed | Keep the `components:` filter on `external_components` |
| `wmbus_meter` needs time | A `time:` platform (`homeassistant`) must be present |
| XIAO receives nothing | GPIO38 held high, antenna on the LoRa U.FL port |
| Weak signal | Antenna type and placement first; compare RSSI with the Heltec in the same spot |
| Stale alarm | Gateway position, meter module battery life |

## Privacy

Receivers will also pick up neighbouring meters. Only decode and store readings from your own meter, or from meters whose owners have asked you to.
