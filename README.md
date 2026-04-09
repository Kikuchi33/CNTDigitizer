# CNT Digitizer Application

The CNT Digitizer is a real-time data acquisition and visualization tool for the CNT Digitizer sensor. It connects to the digitizer over USB serial or WiFi (AirLift ESP32), displays live current measurements across up to 16 channels, and saves data and screenshots locally.

---

## Quick Start (USB Serial)

1. Plug the CNT Digitizer into your computer via USB.
2. Double-click `gui.exe`.
3. Enter the correct COM port in the **Port** field.
4. Click **Connect**.
5. Click **Start**.

Live data should now appear in the plot window.

---

## Quick Start (WiFi)

1. Power the CNT Digitizer from a USB wall adapter or USB power bank.
2. On your PC, connect to the WiFi network **CNT_Digitizer** (password: `cnt12345`).
3. Double-click `gui.exe`.
4. Check **Use WiFi (AirLift UDP)**.
5. Set **Teensy IP** to `192.168.4.1` (this is fixed — the board must uses this address).
6. Leave ports at their defaults (Listen: 5005, Teensy: 5006).
7. Click **Connect**, then **Start**.

> The board creates its own WiFi access point — you do not need a router. Any USB 5V power source (wall adapter, power bank) works. The board does not require a PC connection to power on.

---

## Setting Up Your COM Port (Windows, USB mode only)

### Step 1 — Find Your COM Port

1. Press `Windows + X`
2. Click **Device Manager**
3. Expand **Ports (COM & LPT)**
4. Look for something like:

        USB Serial Device (COM3)
        USB Serial Device (COM4)

The number in parentheses is your COM port.

### Step 2 — Enter the Port

#### Option A (Recommended)

Enter the port directly into the GUI (e.g. `COM3`) and click **Connect**.

#### Option B (Optional)

Edit `port.txt` so it contains only your port name:

    COM3

If `port.txt` is missing the application defaults to `COM4`.

---

## Using the Application

### Connection Panel

| Control | Description |
|---|---|
| **Use WiFi (AirLift UDP)** | Switch between USB serial and WiFi transport |
| **Serial Port** | COM port for USB mode (e.g. `COM4`) |
| **Teensy IP** | IP address of the board in WiFi mode (always `192.168.4.1`) |
| **Listen Port** | UDP port this PC receives data on (default `5005`) |
| **Teensy Port** | UDP port the board receives commands on (default `5006`) |
| **Connect / Disconnect** | Open or close the connection |

### Device Parameters

These sliders configure the hardware in real time. Changes are sent to the board immediately — you do not need to stop and restart measurement.

| Parameter | Description | Default |
|---|---|---|
| **Bias (mV)** | Bias voltage applied to the sensor | 1000 mV |
| **Gate (mV)** | Gate voltage applied to the sensor | −1000 mV |
| **Bias Sample Delay (ms)** | How often the bias voltage is sampled | 1000 ms |
| **Measurement Delay (ms)** | How often current measurements are taken | 100 ms |

### Measurement

- Click **Start** to begin. The board enters measurement mode and data begins streaming.
- All active device parameters are pushed to the board automatically when you click Start.
- Click **Stop** to end the run. A CSV file containing all 16 channels is saved automatically to `output/` with the session name and parameters recorded in the file header.
- Each run is called a **session** and is named by timestamp (e.g. `run_20250304_141522`).

### Plot Panel

| Control | Description |
|---|---|
| **Auto-scroll (10s)** | Keeps the last 10 seconds visible as data streams in |
| **Overlay all 16 channels** | Plot all channels simultaneously with distinct colours |
| **Channel toggles** | When overlay is on, individually show/hide each channel |
| **Channel slider** | When overlay is off, select a single channel to display |
| **EMA Smoothing** | Apply exponential moving average to reduce noise |
| **Alpha** | EMA weight — lower = smoother, higher = closer to raw signal |
| **Max Points** | Number of samples to keep in memory (200 – 200,000) |
| **Clear Data** | Wipe the current plot without stopping measurement |

### Saving Data

#### Auto-save on Stop

When you click **Stop**, the full 16-channel dataset for that session is saved automatically:

    output/run_YYYYMMDD_HHMMSS.csv

The CSV includes a header with the session name, bias voltage, gate voltage, and both delay settings so your measurement conditions are always recorded alongside the data.

#### Manual CSV Save

Click **Save CSV** at any time during or after a run.

    output/dataset_YYYYMMDD_HHMMSS.csv

Enable **All channels in CSV** to include all 16 channels. Otherwise only the selected channel is saved.

#### Screenshot

Click **Save Screenshot (BMP)** or press the button in the View section.

    output/screenshot_YYYYMMDD_HHMMSS.bmp

---

## Fullscreen Mode

Press **F11** to toggle between fullscreen and windowed mode. The application starts in fullscreen by default.

---

## Files Created Automatically

The application creates the `output/` folder automatically. You do not need to create it manually.

    output/
    output/digitizer.log
    output/run_*.csv                  ← auto-saved per session on Stop
    output/dataset_*.csv              ← manual saves
    output/screenshot_*.bmp

---

## Log File

All runtime events are logged to:

    output/digitizer.log

Include this file when reporting issues.

---

## Powering the Board

The board runs from any 5V USB source. USB connection to a PC is **not** required for WiFi operation.

| Power source | Use case |
|---|---|
| USB cable to PC | Development, serial mode, firmware flashing |
| USB wall adapter (5V, 1A+) | Lab bench WiFi operation |
| USB power bank | Portable / untethered WiFi operation |

If using a power bank, confirm it does not auto-shutoff under low current load.

---

## Troubleshooting

### Application Will Not Start

Ensure the following files are present in the same folder as `gui.exe`:

    libgcc_s_seh-1.dll
    libstdc++-6.dll
    libwinpthread-1.dll
    glfw3.dll

### Failed to Open Serial Port

- Incorrect COM port selected.
- Port already in use — close Arduino IDE or any open serial monitor.
- Faulty or charge-only USB cable.
- Device not properly connected.

Steps: re-check Device Manager, unplug and reconnect, try a different cable, restart the application.

### WiFi Network Not Appearing

- Confirm the board is powered (activity LED should be blinking).
- If the board was just powered on, wait 3–5 seconds for the AP to initialise.
- If the LED blinks rapidly twice on startup, WiFi initialised successfully. If it does not, the AirLift module may not be seated correctly.
- Confirm the WiFiNINA firmware on the AirLift module is up to date (use the Arduino IDE WiFiNINA firmware updater if unsure).

### Connected to WiFi But GUI Shows Disconnected

- Confirm **Teensy IP** is set to `192.168.4.1`.
- Confirm **Listen Port** is `5005` and **Teensy Port** is `5006`.
- Ensure no firewall is blocking UDP on those ports. On Windows, allow the application through Windows Defender Firewall if prompted.
- Try disconnecting and reconnecting in the GUI.

### Connected But No Data Appears

- Ensure you clicked **Start**.
- In WiFi mode: the board learns your PC's IP from your first outgoing packet — the GUI sends a connect handshake automatically, but if the board was restarted after connecting you may need to disconnect and reconnect in the GUI.
- Try unplugging and reconnecting the device (USB mode) or power-cycling it (WiFi mode).

### Unexpected Negative or Erratic Sensor Values

Possible causes: sensor hardware fault, incorrect wiring, electrical noise, or bias/gate voltage misconfiguration. Try adjusting the Bias and Gate sliders. If persistent, contact the sensor maintainer.

### Device Does Not Appear in Device Manager

- Try a different USB port or cable.
- Install the appropriate USB serial driver for the microcontroller.
- Test on a different computer.

---

## System Requirements

- Windows 10 or Windows 11
- USB port (for serial mode or power)
- WiFi adapter (for WiFi mode)
- OpenGL-capable graphics hardware

---

## Support

If problems persist, contact the CNT Digitizer maintainer.

Current maintainer: Nick K.
