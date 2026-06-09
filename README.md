# Flock Hunter BLE

A passive Bluetooth Low Energy (BLE) Flock Safety camera detector built for the **ESP32-2432S028R** (Cheap Yellow Display / CYD) board. Scans for BLE advertisements with the Flock Safety manufacturer ID (0x09C8) — completely passive, no transmitting.

## Credits

This project evolved from the original **[Flock You](https://github.com/colonelpanichacks/flock-you)** project by **[colonelpanichacks](https://github.com/colonelpanichacks)**. BLE manufacturer ID detection research by **@wgreenberg**.

This firmware and documentation were built with **[Claude](https://claude.ai)** by Anthropic.

## How It Works

Flock Safety cameras broadcast BLE advertisements containing the manufacturer ID **0x09C8** (registered to XUNTONG). This firmware continuously scans for BLE advertisements and matches against this manufacturer ID. When a match is found, the buzzer sounds, the screen shows detection details, and the event is logged to the SD card as CSV.

### Why BLE?

WiFi-based detection (used by [Flock Hunter CYD](https://github.com/LuxStatera/flock-hunter-cyd-wifi)) matches MAC address OUI prefixes in 802.11 frames. BLE detection is a complementary approach — some cameras may be detectable via BLE when their WiFi radio is quiet. Running both a WiFi detector and a BLE detector gives the best coverage.

## Hardware

### ESP32-2432S028R (CYD) Specs
- **MCU:** ESP32-D0WD-V3 (dual-core 240MHz)
- **Display:** 2.8" ILI9488 TFT, 320x480 pixels
- **RGB LED:** R:4, G:16, B:17 (active low)
- **Backlight:** Pin 21
- **SD Card:** Built-in micro SD slot (CS:5)
- **Speaker (optional):** Pin 26 — wire a small speaker or piezo buzzer to GPIO 26 for audio alerts
- **USB:** USB-C (CH340 serial + power) + Micro USB (power only)

## SD Card Logging

Insert a **FAT32-formatted micro SD card** and the detector automatically logs BLE detections as CSV.

### Folder Structure

Each boot creates a new session:
```
/flock/
  session_001/
    csv/ble_detections.csv
  session_002/
    csv/ble_detections.csv
```

### CSV Format

```csv
timestamp_ms,mac,rssi,range,name
15234,D4:E9:F4:B1:40:0C,-62,CLOSE,Flock Camera
28451,A8:3B:76:12:34:56,-78,NEAR,
```

GPS coordinates will be added to the CSV when GPS module support is added.

## UI Screens

### Boot Screen
Displays "Flock Hunter" title, "Based on Flock You" credit, "BLE Edition" label, and manufacturer ID.

### Scanning Screen
- Green header bar with "FLOCK HUNTER BLE" title
- Animated "BLE SCAN..." text
- Manufacturer ID indicator box (0x09C8)
- Live BLE device counter and camera counter
- "X IN RANGE" live counter
- SD card and CSV recording status
- Uptime display

### Alert Screen
Triggered on new camera detection:
- Red flashing for 1 second, then solid display for 4 seconds
- "FLOCK CAMERA DETECTED" banner
- Details: BLE detection type, MAC address, signal strength with range estimate (CLOSE/NEAR/FAR), manufacturer ID, device name, hits, status

### Camera List Screen
- Shows the 4 most recent detected cameras
- Green/red status dots for active/stale cameras
- Displays for 5 seconds before returning to scan mode

## LED Indicators

| State | LED Color | Behavior |
|-------|-----------|----------|
| Boot | Blue | Solid |
| Scanning | Green | Pulsing (sine wave breathing) |
| Detection | Red | Solid during alert |

## Range Estimates

Range labels (CLOSE / NEAR / FAR) are rough estimates based on RSSI signal strength. Actual range varies with environment — walls, trees, antenna orientation, and transmit power all affect the signal.

## Building & Flashing

### What You Need
- **ESP32-2432S028R** board (2.8" CYD with ILI9488 display, USB-C variant)
- **Micro SD card** (FAT32 formatted, any size) — for CSV logging
- **USB-C cable** (data cable, not charge-only)
- A computer (Windows, macOS, or Linux)

### Step 1: Install PlatformIO

**Option A — VS Code (recommended):**
1. Install [VS Code](https://code.visualstudio.com/)
2. Open VS Code, go to Extensions (Ctrl+Shift+X / Cmd+Shift+X)
3. Search for "PlatformIO IDE" and install it
4. Restart VS Code when prompted

**Option B — CLI only:**
```bash
pip install platformio
```

### Step 2: Download This Project
```bash
git clone https://github.com/LuxStatera/flock-hunter-cyd-ble.git
cd flock-hunter-cyd-ble
```

### Step 3: Update Serial Port

Open `platformio.ini` and change the port to match your board:
```ini
upload_port = /dev/cu.usbserial-110    ; <-- change this
monitor_port = /dev/cu.usbserial-110   ; <-- change this
```

### Step 4: Flash
```bash
pio run -t upload
```

## Serial Output

```
[FLOCK HUNTER BLE] Booting...
[DISPLAY] w=320 h=480 (ILI9488)
[SD] Card ready — 7627MB
[SD] Session: /flock/session_001
[FLOCK HUNTER BLE] Scanning for MFR ID 0x09C8
[BLE ALERT] D4:E9:F4:B1:40:0C RSSI:-62 Flock Camera
```

## Coming Soon

### GPS Mapping (Optional)

Support for an **ATGM336H GPS module** connected via the CN1 expansion header (GPIO 22/27 + 3V3 + GND). This will add GPS coordinates to each CSV detection entry for mapping in Google Earth, Google Maps, or QGIS.

## Flock Hunter Family

- **[Flock Hunter CYD WiFi](https://github.com/LuxStatera/flock-hunter-cyd-wifi)** — WiFi detector with 32 OUI prefixes + PCAP capture
- **[Flock Hunter CYD BLE](https://github.com/LuxStatera/flock-hunter-cyd-ble)** — Bluetooth detector scanning for manufacturer ID 0x09C8 (this project)
- **[Flock Hunter D1 Mini WiFi](https://github.com/LuxStatera/flock-hunter-d1-mini-wifi)** — Compact WiFi detector with OLED display + piezo buzzer
- **[Flock Camera Emulator](https://github.com/LuxStatera/flock-camera-emulator)** — ESP32 test tool for validating detectors

## Legal Disclaimer

This device is a **passive receiver only**. It does not transmit, connect to, or interfere with any Bluetooth devices. It passively listens for BLE advertisements — the same way any Bluetooth-enabled phone or laptop does. Check your local laws before use. This project is for educational and research purposes.
