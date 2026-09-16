# wM-Bus Water Meter Reader

ESPHome configs for reading a Diehl IZAR water meter (IZAR RC 868 i W R4 clip-on module) over wireless M-Bus and publishing readings to Home Assistant.

Built on [SzczepanLeon/esphome-components](https://github.com/SzczepanLeon/esphome-components) (`wmbus_common`, `wmbus_radio`, `wmbus_meter`).

## Configs

| File | Hardware | Purpose |
|---|---|---|
| `water-meter-survey.yaml` | Heltec WiFi LoRa 32 V3 | Survey: logs every wM-Bus frame to find your meter ID |
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

### 1. Survey: find your meter ID

1. Flash `water-meter-survey.yaml` to the Heltec.
2. Watch the logs; each frame shows its meter ID, raw hex and RSSI. The OLED shows a frame count and the strongest meter.
3. Confirm your meter by matching the decoded `prefix` and `serial_number` against the markings on the meter body.
4. Check the decoded `total_m3` against the dial.

On an estate with identical properties, several Diehl meters will usually be visible. The strongest signal is not always yours, so confirm by serial number.

### 2. Reader: run the gateway

Set your values in the gateway config:

```yaml
wifi:
  ssid: "YOUR_SSID"
  password: "YOUR_WIFI_PASSWORD"

wmbus_meter:
  - id: water_meter
    meter_id: 0xXXXXXXXX
```

Then compile and flash:

```bash
esphome run water-meter-gateway.yaml
```

After the first flash, updates can be sent over OTA.

### Entities exposed to Home Assistant

- **Sensors:** water total (m³, `total_increasing`), last month total, battery life (years), meter RSSI, transmit period, gateway uptime, WiFi signal.
- **Text sensors:** current alarms, previous alarms, last month reading date, ESPHome version.
- **Binary sensors:** water leak, meter fault (any alarm), and stale (no telegram for 6 hours).

For daily or monthly consumption, point a Utility Meter helper at Water Total.

### Heltec OLED controls

- Short press PRG: cycle pages (main reading, diagnostics, alarm history).
- Long press PRG: display on/off.

### XIAO status LED

The yellow user LED flashes on each received telegram, roughly every 8 seconds.

## Flashing

**Heltec V3:** to enter the bootloader, hold PRG and press RST. To flash a compiled image manually:

```bash
esptool --chip esp32s3 --baud 460800 write-flash 0x0 firmware.factory.bin
```

**XIAO ESP32S3:** hold BOOT while plugging in USB, then flash as above.

## Troubleshooting

| Symptom | Check |
|---|---|
| Compile error: `logger` shadowed | Keep the `components:` filter on `external_components` |
| `wmbus_meter` needs time | A `time:` platform (`homeassistant`) must be present |
| XIAO receives nothing | GPIO38 held high, antenna on the LoRa U.FL port |
| Weak signal | Antenna type and placement first; compare RSSI with the Heltec in the same spot |
| Stale alarm | Gateway position, meter module battery life |

## Privacy

Receivers will also pick up neighbouring meters. Only decode and store readings from your own meter, or from meters whose owners have asked you to.
