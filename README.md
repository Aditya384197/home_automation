# ⚡ ESP32 SMART HOME

### Offline • Local • 5-Relay Ready

A clean, offline-first ESP32 home automation controller with web +
physical-switch control, persistent configuration and local OTA updates.

------------------------------------------------------------------------


## Firmware Flashing

For a complete ESP32 firmware installation, flash the following files at the specified addresses:

| File | Flash Address |
|---|---:|
| `bootloader.bin` | `0x1000` |
| `partition-table.bin` | `0x8000` |
| `ota_data_initial.bin` | `0xF000` |
| `offline_smart_home.bin` | `0x20000` |

> **Note:** These addresses are for a complete initial firmware flash.  
> For OTA updates, use only `offline_smart_home.bin` through the built-in OTA update system.


## 🔐 Default Access

  Setting                   Default
  ------------------------- ------------------
  **Wi-Fi / AP Password**   `ChangeMe123`
  **OTA Password**          `OTA@ESP32#2026`
  **Local Address**         `192.168.4.1`

**Change both passwords before permanent installation.**

-   **Wi-Fi password** → controls access to the ESP32 network.
-   **OTA password** → authorizes firmware updates.

They are independent; changing one does not change the other.

------------------------------------------------------------------------

## ⚙️ Core Features

-   **3 relays enabled by default**
-   **Relay 4 & 5 optional**
-   Rename any relay from **Settings → Relay Configuration**
-   Physical switches for up to **5 relays**
-   Web control and physical control work in parallel
-   Relay states/configuration survive power interruptions through NVS
-   Local AP configuration
-   Password-protected OTA
-   OTA password can be changed using **old → new → confirm**
-   OTA upload progress
-   Local captive-portal style access
-   Designed for continuous local operation

------------------------------------------------------------------------

## 🔌 Relay & Physical Switch GPIO

  Channel     Relay GPIO   Physical Switch GPIO Default
  --------- ------------ ---------------------- --------------
  Relay 1        GPIO 16                GPIO 32 ON / Enabled
  Relay 2        GPIO 17                GPIO 33 ON / Enabled
  Relay 3        GPIO 18                GPIO 25 ON / Enabled
  Relay 4        GPIO 19                GPIO 26 Optional
  Relay 5        GPIO 21                GPIO 27 Optional

Relay 4/5 can be enabled whenever required. Once enabled, they
automatically become available on the main page **and their
corresponding physical-switch input becomes active**.

### Physical switch logic

The switch input is a **low-voltage ESP32 input** using the configured
GPIO and GND. The ESP32 detects the switch transition, updates the relay
state, and the web UI reflects the new state.

**Never connect 220/230 V mains directly to an ESP32 GPIO.** Use an
appropriate isolated/safe low-voltage interface for physical wall
switches.

------------------------------------------------------------------------

## 🎛️ Relay Configuration

Go to:

**Settings → Relay Configuration**

You can:

-   Enable/disable optional Relay 4 and Relay 5
-   Rename any relay
-   Configure the relay according to the appliance it controls

Examples:

``` text
Relay 1 → LED
Relay 2 → Fan
Relay 3 → Charging Socket
Relay 4 → Pump
Relay 5 → Light
```

Only enabled relays appear on the main control page.

------------------------------------------------------------------------

## 🌐 AP Configuration

Go to:

**Settings → AP Configuration**

Change:

-   SSID
-   Wi-Fi/AP password

The AP operates locally at:

``` text
192.168.4.1
```

No Internet connection is required.

After saving AP settings, the ESP32 restarts and the phone must
reconnect using the new credentials.

------------------------------------------------------------------------

## 🔄 OTA Update

Go to:

**Settings → OTA Update**

1.  Enter the OTA password.
2.  Select the correct `.bin` firmware.
3.  Start the update.
4.  Monitor upload progress.
5.  ESP32 restarts automatically after a successful update.

OTA is performed locally through the ESP32 AP.

### OTA Password

Go to:

**Settings → OTA Password**

Change it using:

``` text
Old Password
New Password
Confirm New Password
```

The Wi-Fi password and OTA password are separate.

------------------------------------------------------------------------

## ⚡ Power & State Recovery

The project is intended to tolerate normal repeated power interruptions.

Relay configuration, names and saved states are stored in **NVS**. On
restart, the firmware initializes the outputs safely and restores the
saved configuration/state.

Physical-switch monitoring uses debounce logic so normal mechanical
switch bounce does not produce repeated commands.

------------------------------------------------------------------------

## 🛠️ Hardware Note

For a permanent installation, use:

-   Properly rated relay modules
-   Suitable regulated ESP32 power
-   Electrical isolation between mains and GPIO circuitry
-   Appropriate enclosure and protection
-   Correctly rated mains wiring and terminals

The ESP32 GPIO side is **low voltage only**.

------------------------------------------------------------------------

## 📌 Quick Start

``` text
1. Flash firmware
2. Connect to ESP32-SMART-HOME
3. Open 192.168.4.1
4. Configure relay names / optional relays
5. Change Wi-Fi password
6. Change OTA password
7. Test web + physical control
8. Install only after electrical safety checks
```

### Smart Home

**Local control. No cloud. No Internet dependency.**
