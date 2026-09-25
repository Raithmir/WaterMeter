# wM-Bus Water Meter Reader

Read your water meter from inside your home, and see your water use and leak alarms in [Home Assistant](https://www.home-assistant.io).

Many water meters have a radio module that broadcasts the meter reading every few seconds, so the water company can read it from the street. The broadcast uses a standard called wireless M-Bus (wM-Bus). This project uses a small, inexpensive ESP32 radio board to listen for your meter's broadcasts.

It's written for the **Diehl IZAR** radio module (IZAR RC 868 i W R4, a clip-on module on top of the meter), which broadcasts unencrypted. Other wM-Bus meters may work with changes to the config, but aren't covered here.

## How it works

There are two stages, each with its own firmware (the program that runs on the board):

1. **Survey: find your meter's ID.** Every meter broadcasts an ID, and your neighbours' meters broadcast too. You take the board near your meter to find out which ID is yours. You don't need Home Assistant for this, and there's ready-made firmware you can install from your web browser.
2. **Gateway: read your meter permanently.** You put your meter's ID into the gateway config and install it on the board, then leave the board somewhere in range of the meter. It sends every reading to Home Assistant.

The same board does both jobs: install the survey first, then replace it with the gateway.

## What you need

- **A radio board.** Either:

  | Board | Good for |
  |---|---|
  | **Heltec WiFi LoRa 32 V3** | Beginners. It has a small screen that shows the meter ID and reading, so you can see what's happening without a computer. |
  | **Seeed XIAO ESP32S3 + Wio-SX1262 kit** | About half the price, but no screen. You see the results on a web page, in Home Assistant, or in the logs. |

  Both come with an antenna. Connect it before you use the board: the radio can't hear the meter without it. On the XIAO kit, use the larger LoRa antenna rather than the flat one, and connect it to the socket on the radio module.
- **A USB-C cable that carries data.** Some cables only charge, and the computer won't see the board through them. If nothing happens when you plug the board in, try another cable.
- **A computer with Chrome or Edge**, to install the firmware. Other browsers can't talk to USB devices.
- **A 2.4 GHz Wi-Fi network.** The boards can't use 5 GHz. Most home routers offer both bands, often under the same network name, so this usually just works.
- **For the gateway: Home Assistant**, with the ESPHome Device Builder add-on installed. See [Step 2](#step-2-set-up-the-gateway).

Have a Seeed Wio Tracker L1 instead? It has its own survey firmware with a screen, GPS and saved results: see [`wio-wmbus-survey`](wio-wmbus-survey/).

## Step 1: Find your meter ID

Your meter's ID is 8 characters long and uses the digits 0–9 and letters A–F, for example `2124589C`. The survey shows it with `0x` in front (`0x2124589C`), which is the form the gateway config needs.

The ID is **not** the serial number printed on the meter, so you can't read it off the meter. You have to listen for it.

### Install the survey firmware

1. Download the survey firmware for your board from the [latest release](https://github.com/Raithmir/WaterMeter/releases/latest):
   - Heltec: `water-meter-survey.factory.bin`
   - XIAO: `water-meter-survey-xiao.factory.bin`
2. Plug the board into your computer.
   - **XIAO, first time only:** the kit arrives with other firmware (Meshtastic) on it. Hold down the tiny **BOOT** button on the XIAO while you plug in the cable, then let go. This puts the board in download mode.
   - **Heltec:** just plug it in. If it doesn't work later, hold **PRG**, press and release **RST**, then release PRG, and try again.
3. Open [web.esphome.io](https://web.esphome.io) in Chrome or Edge, choose **Connect** and pick the board from the list.
   - If no board appears, see [Troubleshooting](#troubleshooting).
4. Choose **Install**, select the `.bin` file you downloaded, and wait until it says the installation is complete.
5. Restart the board: press **RST** on the Heltec, or unplug and replug the XIAO.

### Connect it to Wi-Fi (optional)

Wi-Fi isn't needed to find the ID, but it lets you see the results from your phone or computer. It's the easiest way to use the XIAO, which has no screen.

- **From web.esphome.io:** with the board still plugged in, choose **Connect** again, then **Configure Wi-Fi**, and enter your network name and password. Afterwards, **Visit Device** opens the board's web page.
- **From your phone:** if the board has no Wi-Fi details, or can't reach your network, it starts its own Wi-Fi network called `Water Meter Survey Setup` (or `Water Meter Survey XIAO Setup`). Connect your phone to it, and a page opens where you choose your network and enter the password. If the page doesn't open by itself, go to `http://192.168.4.1` in the phone's browser.

The board remembers these Wi-Fi details until new firmware is installed. Installing the gateway later means entering them again, or better, putting them in the gateway config (Step 2).

### Take it to your meter

Hold the board close to your water meter, or to the cover over it. Any USB power works, so you don't need a computer there:

- **A USB power bank.** Some power banks switch off when the device draws very little power. If the board keeps switching off, try a different one.
- **Your phone**, with a USB-C to USB-C cable. Most Android phones with USB-C, and iPhone 15 and later, can power the board. Some Android phones need USB power output or OTG switched on first.
- **A LiPo battery.** Both boards have a battery connector and charge the battery over USB.

Then see what the board found. The survey lists **Diehl meters it has heard at least twice at -85 dBm or stronger** (see [Reading the logs](#reading-the-logs) for what that means). It keeps the list until the board loses power.

| Where | How |
|---|---|
| **Heltec screen** | The large text is the strongest meter heard in the last few seconds. Short press **PRG** to step through the found meters; long press to clear the list. |
| **XIAO light** | The yellow light blinks each time it hears a Diehl meter at -85 dBm or stronger, so it blinks more often the closer you are to one. |
| **Web page** | On the same Wi-Fi, open `http://water-meter-survey.local` (Heltec) or `http://water-meter-survey-xiao.local` (XIAO). It shows the found meters and live logs. If that address doesn't open, which happens on some phones and computers, look up the board's IP address in your router's list of connected devices and open that instead. |
| **Logs** | Plug the board into a computer, open [web.esphome.io](https://web.esphome.io), choose **Connect**, then **Logs**. (Unplugging the power to do this clears the list, unless a LiPo battery keeps the board running.) |
| **Home Assistant** | If you have it, the board appears under Settings → Devices & services. Add it to see the `Survey Found Meters` sensor. |

With the XIAO, keep the board powered after visiting your meter and walk back into Wi-Fi range, then open the web page.

#### Reading the logs

When the survey finds a new meter, it logs:

```
[I][survey:...]: NEW meter 0x2124589C  -71dBm  b8=..  (total 1)
```

Every 30 seconds it also logs the whole list, strongest signal first:

```
[I][survey:...]: Found 3 meter(s), strongest first:
[I][survey:...]:   0x2124589C  -70dBm  b8=..
[I][survey:...]:   0x21245A11  -78dBm  b8=..
[I][survey:...]:   0x2124601F  -84dBm  b8=..
```

- `0x2124589C` is the meter ID.
- `-70dBm` is the signal strength (RSSI). Numbers closer to 0 are stronger: -70 is stronger than -80. Meter signals are weak: most meters sit in a pit under a lid, and the lid and ground block a lot of the signal. Around -70 while holding the board right over the meter is normal.
- `FRAME` lines show each broadcast from a Diehl meter as it arrives, with its raw data after `HEX:`. You only need them to check your meter's serial number (below).

### Make sure it's yours

Your meter is usually the one whose signal becomes clearly the strongest when you hold the board right next to it. But on an estate of similar houses, meters can be close together, and the strongest signal isn't always yours. To be sure, do one of these:

- **Check the serial number.** In the logs, find a `FRAME` line from your meter and copy the long `HEX:` value. Paste it into the [wmbusmeters analyzer](https://wmbusmeters.org/analyze/). Compare the `prefix` and `serial_number` it shows with the numbers printed on your meter, and `total_m3` with the reading on the dial.
  - To match a `FRAME` line to an ID: the ID appears in the hex backwards, two characters at a time. For `2124589C`, look for `9C582421`.
- **Try it in the gateway.** Set up the gateway with that ID (Step 2), and check that its reading matches the dial.

## Step 2: Set up the gateway

The gateway listens only for your meter and sends its readings to Home Assistant. It uses every broadcast it can decode, however weak, so it can live indoors. A spot at the front of the house nearest the meter, such as a windowsill, works best. Once it's running, the **Water Meter RSSI** sensor shows how well it hears the meter. It's designed for Home Assistant: without it, the Heltec's screen still shows the current reading, but nothing records it.

Because the gateway needs your meter ID built in, there's no ready-made download. You build it yourself with ESPHome, which turns a text config file into firmware. The easiest way is the **ESPHome Device Builder** add-on in Home Assistant.

### 1. Install the ESPHome Device Builder

In Home Assistant, go to Settings → Add-ons → Add-on store, install **ESPHome Device Builder**, start it, and open its web UI.

### 2. Create the config

1. In ESPHome, choose **New device** and name it `water-meter-gateway` (Heltec) or `water-meter-gateway-xiao` (XIAO), to match the name inside the config. Skip the installation it offers.
2. Choose **Edit** on the new device, delete everything in the file, and paste in the contents of the gateway config for your board:
   - Heltec: [`water-meter-gateway.yaml`](water-meter-gateway.yaml)
   - XIAO: [`water-meter-gateway-xiao.yaml`](water-meter-gateway-xiao.yaml)
3. Fill in your details:

   | Replace | With |
   |---|---|
   | `YOUR_WIFI_SSID` | Your Wi-Fi network name |
   | `YOUR_WIFI_PASSWORD` | Your Wi-Fi password |
   | `YOUR_METER_ID` | Your meter ID from the survey, **including the `0x`** |

   For example:

   ```yaml
   wmbus_meter:
     - id: water_meter         # leave this line as it is
       meter_id: 0x2124589C    # your ID, exactly as the survey showed it
   ```

   Put your real Wi-Fi details in the config. The gateway can also be set up from its `Water Meter Gateway Setup` Wi-Fi network, like the survey, but Wi-Fi details entered that way are forgotten whenever you update the gateway.
4. Choose **Save**.

**Why the `0x` matters:** it tells ESPHome that the ID is written in hexadecimal. Without it:

- An ID made only of digits, such as `12345678`, is read as an ordinary number and means a different meter. Everything seems to work, but the gateway never shows a reading.
- An ID with letters, such as `2124589C`, stops the build with `Expected integer, but cannot parse ... as an integer`.

Upper or lower case letters both work. Don't change `id: water_meter`: that's a name used inside the config, not your meter ID.

### 3. Install it on the board

The first installation has to be over USB:

1. Choose **Install**, then **Manual download**. If asked for a format, pick the factory format. Building takes several minutes the first time.
2. Install the downloaded `.bin` file with [web.esphome.io](https://web.esphome.io), the same way as the survey.

After that, the board updates over Wi-Fi: choose **Install** → **Wirelessly** whenever you change the config.

### 4. Add it to Home Assistant

Once the gateway is on your Wi-Fi, Home Assistant finds it. Go to Settings → Devices & services, and choose **Configure** on the discovered ESPHome device. Within about a minute, **Water Total** shows your meter reading.

### What you get

- **Readings:** water total (m³), total on the last billing date, meter battery life (years), meter signal strength, how often the meter broadcasts.
- **Alarms:** water leak, meter fault, current and previous alarm codes, and **Water Meter Stale**, which turns on if nothing has been heard from the meter for 6 hours.
- **Gateway health:** uptime, Wi-Fi signal and ESPHome version.

For daily or monthly water use, create a **Utility Meter** helper in Home Assistant based on Water Total. You can also add Water Total to the Energy dashboard as water consumption.

**Heltec screen:** short press **PRG** to switch between the reading, diagnostics and alarm pages. Long press to turn the screen off or on.

**XIAO light:** blinks each time a reading arrives from your meter, about every 8 seconds.

### Building from the command line

If you'd rather not use Home Assistant's add-on, install ESPHome on your computer (`pip install esphome`), download the config file, fill in the details as above, plug in the board and run:

```bash
esphome run water-meter-gateway.yaml
```

This builds the firmware, installs it (pick the USB port when asked), then shows the logs. Later updates can go over Wi-Fi. The survey configs can be built the same way.

## Troubleshooting

| Problem | What to try |
|---|---|
| No board appears at web.esphome.io | Use Chrome or Edge. Try another USB cable, since some only charge. Put the board in download mode: XIAO, hold BOOT while plugging in; Heltec, hold PRG and press RST. On Windows or macOS, the Heltec may need the Silicon Labs CP210x USB driver. |
| Installed, but nothing happens | Restart the board: press RST (Heltec), or unplug and replug (XIAO). |
| Board keeps switching off on a power bank | The power bank switches off at low power draw. Try a different power bank, your phone, or a LiPo battery. |
| Survey finds no meters | Check the antenna is connected. Hold the board right next to the meter or its cover. The list only includes Diehl meters heard twice with a good signal. |
| Can't open the web page | The board must be on the same Wi-Fi as your phone or computer. Try the board's IP address from your router instead of the `.local` address. |
| Gateway runs but never shows a reading | Check `meter_id` has `0x` in front and matches the survey, and isn't the serial number from the meter. Check the gateway is in range. Compare the signal strength with the survey's. |
| `Expected integer, but cannot parse ...` | `meter_id` is missing its `0x`. |
| **Water Meter Stale** turns on | The gateway hasn't heard the meter for 6 hours. Move the gateway closer, and check the meter's battery life. |
| XIAO receives nothing | Check the antenna is on the radio module's socket. |

## Privacy

The board picks up your neighbours' meters as well as yours. Only read and store your own meter, or meters whose owners have asked you to.

## Technical reference

### Config files

| File | Board | Purpose |
|---|---|---|
| `water-meter-survey.yaml` | Heltec WiFi LoRa 32 V3 | Survey, results on the OLED |
| `water-meter-survey-xiao.yaml` | XIAO ESP32S3 + Wio-SX1262 | Survey, headless |
| `water-meter-gateway.yaml` | Heltec WiFi LoRa 32 V3 | Gateway with OLED display |
| `water-meter-gateway-xiao.yaml` | XIAO ESP32S3 + Wio-SX1262 | Gateway, headless |

Built on [SzczepanLeon/esphome-components](https://github.com/SzczepanLeon/esphome-components) (`wmbus_common`, `wmbus_radio`, `wmbus_meter`).

Every push compiles all four configs in GitHub Actions. Pushing a `v*` tag publishes a release with the Heltec and XIAO survey firmware and the Wio Tracker survey firmware. The gateways aren't released, because the meter ID is compiled in.

### Meter

| Item | Value |
|---|---|
| Driver | `izar` |
| Mode | T1 (868.95 MHz) |
| Encryption | None |
| Transmit interval | ~8 s |

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

Same radio chip as the Heltec, with built-in LiPo charging. The radio connects over the B2B connector.

| Signal | GPIO |
|---|---|
| SPI SCK / MOSI / MISO | 7 / 9 / 8 |
| CS / RESET / BUSY / DIO1 | 41 / 42 / 40 / 39 |
| Antenna switch enable (RF_SW) | 38, held high |
| User LED (active low) | 21 |

Pin sources: Seeed's [one_channel_hub BSP](https://github.com/Seeed-Studio/one_channel_hub/blob/4cc771ac02da1bd18be67509f6b52d21bb0feabd/components/smtc_ral/bsp/sx126x/seeed_xiao_esp32s3_devkit_sx1262.c) and the [Wio-SX1262 datasheet](https://files.seeedstudio.com/products/SenseCAP/Wio_SX1262/Wio-SX1262_Module_Datasheet.pdf).

- DIO2 selects transmit or receive (`rf_switch: true`), and GPIO38 enables the switch.
- The TCXO is powered from DIO3 (`has_tcxo: true`). Seeed uses 3.0 V; the module accepts 1.7–3.3 V.
- The FPC antenna is for testing only.

### Survey details

- The found list holds Diehl meters (manufacturer code `0x4C30`) heard twice at -85 dBm or stronger. The gateways have no limit: they use every frame from your meter that decodes. `b8` is byte 8 of the frame (the version field).
- `Survey Found Meters` is sorted strongest first and cut to Home Assistant's 255-character limit, about 15 meters. The 30-second log always has the full list.
- The XIAO survey also has `Survey Strongest Meter`, `Survey Strongest RSSI`, `Survey Frame Count` and a `Survey Clear Found Meters` button.
- The surveys set `api: reboot_timeout: 0s`, so they don't restart when nothing connects to them.

### Manual flashing

With [esptool](https://github.com/espressif/esptool), in download mode:

```bash
esptool --chip esp32s3 --baud 460800 write-flash 0x0 firmware.factory.bin
```

### Build troubleshooting

| Error | Fix |
|---|---|
| `logger` shadowed | Keep the `components:` filter on `external_components`. |
| `wmbus_meter` needs time | A `time:` platform (`homeassistant`) must be present. |
