# CNT Digitizer Application

CNT Digitizer is a real-time data acquisition and visualization tool for the CNT Digitizer sensor.

This application connects to the digitizer over a serial (COM) port, displays live current measurements, and allows saving data and screenshots locally.

---

## Quick Start (Windows)

1. Plug the CNT Digitizer into your computer via USB.
2. Double-click `gui.exe`.
3. Enter the correct COM port in the **Port** field.
4. Click **Connect**.
5. Click **Start Measurement**.

Live data should now appear in the plot window.

---

## Setting Up Your COM Port (Windows)

### Step 1 — Find Your COM Port

1. Press `Windows + X`
2. Click **Device Manager**
3. Expand **Ports (COM & LPT)**
4. Look for something like:

    USB Serial Device (COM3)  
    USB Serial Device (COM4)

The number in parentheses is your COM port.

---

### Step 2 — Enter the Port

#### Option A (Recommended)

Enter the port directly into the GUI, for example:

    COM3

Then click **Connect**.

---

#### Option B (Optional)

Edit the file `port.txt` so it contains only your port name, for example:

    COM3

If `port.txt` is missing, the application defaults to `COM4`.

---

## Using the Application

### Connect

- Enter COM port.
- Click **Connect**.
- Status indicator should display **CONNECTED**.

### Start Measurement

- Click **Start Measurement**.
- Status indicator changes to **RUNNING**.
- Live data appears in the graph.

### Stop Measurement

- Click **Stop Measurement**.

---

## Saving Data

### Save CSV Data

Click **Save CSV**.

Files are saved to:

    output/dataset_YYYYMMDD_HHMMSS.csv

If “Save all channels” is enabled, all channels are stored. Otherwise, only the selected channel is saved.

---

### Save Screenshot

Click **Save Screenshot (BMP)**.

Files are saved to:

    output/screenshot_YYYYMMDD_HHMMSS.bmp

---

## Fullscreen Mode

- Press **F11** to toggle between fullscreen and windowed mode.
- The application starts in fullscreen by default.
- In windowed mode, the window can be resized normally.

---

## Files Created Automatically

You do not need to manually create any folders.

The application automatically creates:

    output/
    output/digitizer.log
    output/dataset_*.csv
    output/screenshot_*.bmp

---

## Log File

All runtime events are logged to:

    output/digitizer.log

If reporting an issue, include this file.

---

## Troubleshooting

### Application Will Not Start

If Windows reports a missing DLL, ensure the following files are present in the same folder as `gui.exe`:

    libgcc_s_seh-1.dll
    libstdc++-6.dll
    libwinpthread-1.dll
    glfw3.dll

---

### Failed to Open Serial Port

Possible causes:

- Incorrect COM port selected.
- Port already in use (close Arduino IDE or any serial monitor software).
- Faulty or charge-only USB cable.
- Device not properly connected.

Steps to resolve:

1. Re-check Device Manager for the correct COM port.
2. Unplug and reconnect the device.
3. Try a different USB cable.
4. Restart the application.

---

### Connected But No Data Appears

- Ensure you clicked **Start Measurement**.
- Try unplugging and reconnecting the device.
- Restart the application.

---

### Unexpected Negative or Erratic Sensor Values

Possible causes:

- Sensor hardware malfunction.
- Incorrect wiring.
- Electrical noise.
- Bias or gate voltage misconfiguration.

If persistent, contact the sensor maintainer.

---

### Device Does Not Appear in Device Manager

- Try a different USB port.
- Try a different USB cable.
- Install the appropriate USB serial driver if required by the microcontroller.
- Test on a different computer.

---

## System Requirements

- Windows 10 or Windows 11
- USB port
- OpenGL-capable graphics hardware

---

## Support

If problems persist, contact the CNT Digitizer maintainer.

Current maintainer: Nick K.