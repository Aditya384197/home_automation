# ESP32 Offline Smart Home

A local, offline-first ESP32 smart-home controller designed for reliable
day-to-day operation without Internet access.

The firmware provides:

-   Local Wi-Fi Access Point (AP)
-   Captive-portal style local web interface
-   Five-relay hardware support
-   Three relays enabled by default
-   Relay 4 and Relay 5 optional/configurable
-   Physical wall-switch inputs for all five relays
-   Web control + physical-switch control in parallel
-   Persistent relay state/configuration using NVS
-   Relay renaming from the web interface
-   AP SSID/password configuration
-   Local OTA firmware update
-   OTA password protection
-   OTA password change workflow
-   OTA upload progress display
-   Local DNS redirection for captive-portal behavior
-   Watchdog monitoring for the physical-switch and DNS tasks
-   Debounced physical switch detection
-   Fail-safe relay initialization
-   No STA/Internet dependency

------------------------------------------------------------------------

## 1. Project concept

This project turns an ESP32 into a self-contained smart-home controller.

The ESP32 creates its own Wi-Fi network. A phone or another Wi-Fi device
connects directly to the ESP32 and opens the local control page.

There is no requirement for:

-   Internet
-   Cloud service
-   Router
-   External server
-   Mobile application

The main control path is:

``` text
Phone / Browser
      |
      | Wi-Fi
      v
ESP32 Access Point
      |
      +---- Local Web Server ----> Relay control
      |
      +---- Physical Switch Task -> Relay control
      |
      +---- NVS Storage ----------> Configuration/state persistence
      |
      +---- OTA ------------------> Firmware update
```

The system therefore has two parallel user-control methods:

1.  Web interface
2.  Physical wall switches

Both ultimately control the same relay state inside the ESP32.

------------------------------------------------------------------------

# 2. Hardware architecture

## Relay GPIO assignment

  Relay          GPIO Default status     Physical switch GPIO
  --------- --------- ---------------- ----------------------
  Relay 1     GPIO 16 Enabled                         GPIO 32
  Relay 2     GPIO 17 Enabled                         GPIO 33
  Relay 3     GPIO 18 Enabled                         GPIO 25
  Relay 4     GPIO 19 Optional                        GPIO 26
  Relay 5     GPIO 21 Optional                        GPIO 27

The firmware defines:

``` c
#define RELAY_COUNT 5
```

Therefore the firmware is structurally prepared for five relay channels.

Relay 1, Relay 2 and Relay 3 are enabled by default.

Relay 4 and Relay 5 are disabled by default and can be enabled from:

``` text
Settings
  -> Relay Configuration
```

------------------------------------------------------------------------

# 3. Relay naming

Every relay has a configurable name.

Default names:

``` text
Relay 1 -> Living Room Light
Relay 2 -> Ceiling Fan
Relay 3 -> Charging Socket
Relay 4 -> Relay 4
Relay 5 -> Relay 5
```

The user can rename any relay.

For example:

``` text
Relay 1 -> Bedroom Light
Relay 2 -> Ceiling Fan
Relay 3 -> Charging Socket
Relay 4 -> Water Pump
Relay 5 -> Porch Light
```

The name shown on the main page comes from the saved relay
configuration.

Relay names are limited to 31 bytes/characters by the firmware storage
field.

Control characters are rejected to prevent malformed configuration data.

------------------------------------------------------------------------

# 4. Optional Relay 4 and Relay 5

Relay 4 and Relay 5 are intentionally optional.

This means the default interface remains simple with only the first
three relays visible.

If the user needs another device:

``` text
Settings
  -> Relay Configuration
  -> Enable Relay 4
```

or:

``` text
Settings
  -> Relay Configuration
  -> Enable Relay 5
```

The user can also enable both.

After saving:

``` text
Relay 4 / Relay 5
```

become visible on the main control page.

Their corresponding physical switch input also becomes active.

When an optional relay is enabled, the firmware reads its current
physical switch position and adopts that position as the initial relay
state.

------------------------------------------------------------------------

# 5. Physical wall-switch system

The project supports physical switches for all five relay channels.

The intended low-voltage logic is:

``` text
ESP32 GPIO
     |
     +---- physical switch ---- GND
```

The GPIO uses the ESP32's internal pull-up.

Therefore:

``` text
Switch open   -> GPIO HIGH
Switch closed -> GPIO LOW
```

The firmware defines:

``` c
#define SWITCH_ACTIVE_LEVEL 0
```

So a closed switch is interpreted as the active/on command.

## Important electrical warning

The physical-switch GPIO inputs are intended for low-voltage logic only.

Do NOT connect 220/230 V mains directly to an ESP32 GPIO.

The ESP32 input must only see a safe, correctly isolated low-voltage
signal.

If a conventional household mains wall switch is being repurposed, the
mains side must be electrically isolated from the ESP32 input circuitry
using an appropriate certified interface/isolator or other
professionally designed low-voltage interface.

Mains wiring should be performed by a qualified person.

------------------------------------------------------------------------

# 6. Physical switch + web control synchronization

The physical switch system and web interface operate in parallel.

Example:

``` text
Web page -> Relay 2 ON
```

The relay turns on.

Then someone physically changes Switch 2.

The ESP32 detects the physical transition and changes Relay 2
accordingly.

The web page continuously polls:

``` text
/api/status
```

and therefore reflects the current relay state.

The important design principle is:

> The physical switch does not directly power the appliance. It sends a
> low-voltage command to the ESP32, and the ESP32 controls the relay.

This keeps the logical control path centralized.

------------------------------------------------------------------------

# 7. Physical-switch debounce

Mechanical switches do not change state perfectly. They can electrically
bounce for a short period.

The firmware uses:

``` c
#define SWITCH_DEBOUNCE_SAMPLES 3
#define SWITCH_POLL_MS          20
```

The switch task samples the inputs and requires a stable transition
before accepting it as a real user action.

This prevents one physical switch action from being interpreted as
multiple rapid relay commands.

------------------------------------------------------------------------

# 8. Power-cycle behavior

The project is designed with repeated power interruptions in mind.

Relay state is stored in NVS.

At startup:

1.  NVS is initialized.
2.  Saved relay states/configuration are loaded.
3.  Relay GPIOs are initialized.
4.  Physical switch GPIOs are initialized.
5.  Relays are safely initialized OFF.
6.  Saved relay state is applied.
7.  Physical-switch monitoring starts.
8.  The ESP32 starts its Wi-Fi AP.
9.  DNS and HTTP services start.

A physical switch task establishes a startup baseline without
immediately generating a relay command.

This is important because the saved relay state should not be
overwritten merely because the switch task has just started.

A later physical switch transition changes the relay state.

------------------------------------------------------------------------

# 9. Fail-safe relay initialization

At boot, every relay output is first driven to its logical OFF level.

The code explicitly performs:

``` text
Relay 1 -> OFF
Relay 2 -> OFF
Relay 3 -> OFF
Relay 4 -> OFF
Relay 5 -> OFF
```

before applying the restored configuration/state.

This reduces the chance of an unintended output pulse during
initialization.

The relay polarity can be changed with:

``` c
#define RELAY_ACTIVE_LEVEL 1
```

If the relay module is active-low, change it to:

``` c
#define RELAY_ACTIVE_LEVEL 0
```

Do not change this value unless the actual relay board requires the
opposite polarity.

------------------------------------------------------------------------

# 10. Local Wi-Fi AP

The ESP32 operates as an AP-only device.

Default values:

``` text
SSID:       ESP32-SMART-HOME
Password:   ChangeMe123
Channel:    6
Max clients: 4
```

The AP uses:

``` text
IP:       192.168.4.1
Gateway:  192.168.4.1
Netmask:  255.255.255.0
```

The web interface is therefore available locally at:

``` text
http://192.168.4.1/
```

The device does not require a station connection to another Wi-Fi
network.

------------------------------------------------------------------------

# 11. AP Configuration

From the web interface:

``` text
Settings
  -> AP Configuration
```

The user can change:

-   SSID
-   AP password

The password must be:

``` text
8-63 characters
```

SSID length is:

``` text
1-32 characters
```

After saving:

``` text
Save & Restart
```

the ESP32 stores the values in NVS and restarts.

Because the AP itself restarts, the browser may temporarily report that
the connection was lost. This is expected.

Reconnect to the newly configured Wi-Fi network after the ESP32
restarts.

------------------------------------------------------------------------

# 12. NVS persistent configuration

The firmware uses ESP-IDF NVS for persistent storage.

The namespace is:

``` text
home_cfg
```

Stored data includes:

``` text
relay states
relay enabled flags
relay names
AP SSID
AP password
OTA password
```

The important NVS keys are:

``` text
relay
renable
rnames
ap_ssid
ap_pass
ota_pass
```

This means configuration is retained across normal restarts and power
interruptions.

------------------------------------------------------------------------

# 13. OTA firmware update

OTA is completely local.

The workflow is:

``` text
Settings
  -> OTA Update
  -> Select .bin
  -> Upload & Restart
```

The browser first asks for the OTA password.

The password is sent in the HTTP header:

``` text
X-OTA-Password
```

The firmware verifies it before accepting the firmware upload.

If the password is missing:

``` text
401 Unauthorized
```

If it is incorrect:

``` text
403 Forbidden
```

If the password is correct, the firmware upload proceeds.

------------------------------------------------------------------------

# 14. OTA upload process

The firmware uses ESP-IDF OTA APIs.

The sequence is:

``` text
1. Check OTA password
2. Check OTA lock
3. Find next OTA partition
4. Begin OTA
5. Receive firmware in chunks
6. Write chunks to OTA partition
7. Finish OTA
8. Set boot partition
9. Restart ESP32
```

The firmware buffer is:

``` c
#define OTA_BUFFER_SIZE 4096
```

The browser displays upload progress.

Example:

``` text
Uploading firmware...       37%
```

At successful completion:

``` text
OTA successful. Restarting...
```

The ESP32 then restarts and boots from the updated OTA partition.

------------------------------------------------------------------------

# 15. OTA password

The OTA password is separate from the Wi-Fi/AP password.

This distinction is intentional.

Example:

``` text
Wi-Fi password
    |
    +-- controls access to the ESP32 AP

OTA password
    |
    +-- authorizes firmware replacement
```

Changing the Wi-Fi password does not automatically change the OTA
password.

Changing the OTA password does not change the Wi-Fi password.

------------------------------------------------------------------------

# 16. Default OTA password

The firmware contains a default fallback OTA password:

``` text
OTA@ESP32#2026
```

This is used when no valid OTA password has previously been stored in
NVS.

For an actual deployed device, change the OTA password from:

``` text
Settings
  -> OTA Password
```

Do not treat the default password as a production secret.

------------------------------------------------------------------------

# 17. Changing the OTA password

The OTA password configuration page requires three fields:

``` text
Old password
New password
Confirm new password
```

The firmware validates:

-   all fields exist
-   all passwords are 8-63 characters
-   old password is correct
-   new password and confirmation match
-   new password differs from the old password

Only then is the new password stored in NVS.

The comparison uses a constant-time comparison routine.

This avoids a straightforward character-by-character timing comparison.

------------------------------------------------------------------------

# 18. OTA security model

The OTA password provides an additional authorization layer on top of
the private ESP32 Wi-Fi network.

However, this project is still a local HTTP system.

It does NOT provide:

-   HTTPS/TLS
-   certificate authentication
-   signed firmware verification
-   Secure Boot
-   Flash Encryption

Therefore OTA protection should be understood as an access-control
layer, not as a complete cryptographic secure-boot architecture.

For a high-security production deployment, ESP-IDF security features
such as Secure Boot v2, Flash Encryption and a properly
authenticated/signed firmware update architecture should be considered.

The OTA password is stored in NVS and is used by the firmware at
runtime.

------------------------------------------------------------------------

# 19. OTA partitioning

The project is intended to use an OTA-capable partition table
containing:

``` text
nvs
otadata
phy_init
ota_0
ota_1
```

The two OTA application partitions allow firmware updates without
overwriting the currently running application image.

Conceptually:

``` text
Flash
+----------------+
| Bootloader     |
+----------------+
| Partition tbl  |
+----------------+
| NVS            |
+----------------+
| OTA metadata   |
+----------------+
| PHY init       |
+----------------+
| OTA_0          |
+----------------+
| OTA_1          |
+----------------+
```

The ESP32 writes the new firmware to the inactive OTA partition and then
switches the boot partition after a successful update.

------------------------------------------------------------------------

# 20. Main web interface

The main page intentionally stays simple.

It shows only enabled relays.

Each relay provides:

``` text
Relay name
Current ON/OFF state
Toggle control
```

Example:

``` text
Living Room Light       [ ON ]
Ceiling Fan             [ OFF ]
Charging Socket         [ ON ]
```

If Relay 4 is enabled, it appears automatically.

If Relay 5 is enabled, it appears automatically.

If both are disabled, they remain hidden from the main page.

------------------------------------------------------------------------

# 21. Settings interface

The settings interface uses a slide-in drawer.

From the main page:

``` text
[ Settings icon ]
```

opens the settings panel from the right.

The main page is slightly dimmed while the drawer is open.

The settings menu contains:

``` text
OTA Update
OTA Password
Relay Configuration
AP Configuration
```

Each item opens its own configuration page.

The individual page provides:

``` text
Back
```

which returns to the settings menu.

The top-right close button closes the settings drawer with the reverse
slide animation.

This keeps the main control screen visually clean.

------------------------------------------------------------------------

# 22. Relay Configuration page

The relay configuration page shows all five relay slots.

For every relay:

``` text
Relay number
Relay GPIO
Physical Switch GPIO
Name
```

Relay 1-3 are always enabled.

Relay 4-5 have enable/disable controls.

Example:

``` text
Relay 4
Relay GPIO 19
Physical Switch GPIO 26

[ Enabled ]

Name:
Water Pump
```

Saving the configuration writes the complete relay configuration to NVS.

The firmware also saves relay state together with the configuration
snapshot.

------------------------------------------------------------------------

# 23. Disabling an optional relay

When Relay 4 or Relay 5 is disabled:

1.  Its enabled flag becomes false.
2.  Its relay state is forced OFF.
3.  Its physical output is driven OFF.
4.  The configuration is saved.
5.  It disappears from the main page.

The relay remains available for future re-enabling.

------------------------------------------------------------------------

# 24. Captive portal behavior

The firmware runs a small local DNS server on:

``` text
UDP 53
```

DNS queries are redirected to:

``` text
192.168.4.1
```

Several common connectivity-check URLs are also redirected to the local
web page.

This helps phones and operating systems recognize the ESP32 AP as a
local captive portal.

Supported probe paths include:

``` text
/generate_204
/hotspot-detect.html
/connecttest.txt
/ncsi.txt
/connectivitycheck.gstatic.com/generate_204
/success.txt
```

This is not Internet access. It is local redirection only.

------------------------------------------------------------------------

# 25. HTTP API

The firmware exposes a small local API.

## Get status

``` http
GET /api/status
```

Returns current relay states and relay configuration.

The browser uses this endpoint to keep the interface synchronized.

------------------------------------------------------------------------

## Relay control

``` http
GET /api/relay?relay=1&state=1
```

Example:

``` text
relay=1
state=1
```

means:

``` text
Relay 1 ON
```

while:

``` text
relay=1
state=0
```

means:

``` text
Relay 1 OFF
```

Valid relay numbers are:

``` text
1-5
```

------------------------------------------------------------------------

## AP configuration

Read:

``` http
GET /api/settings
```

Update:

``` http
POST /api/settings
```

The POST body contains:

``` json
{
  "ssid": "ESP32-SMART-HOME",
  "password": "example123"
}
```

The ESP32 saves the values and restarts.

------------------------------------------------------------------------

## Relay configuration

``` http
POST /api/relays
```

The browser sends the enabled flags and names for all five relay slots.

Relay 1-3 are forced enabled by firmware.

Relay 4-5 are user-configurable.

------------------------------------------------------------------------

## OTA password

``` http
POST /api/ota-password
```

The body contains:

``` json
{
  "oldPassword": "old-password",
  "newPassword": "new-password",
  "confirmPassword": "new-password"
}
```

------------------------------------------------------------------------

## OTA firmware

``` http
POST /api/ota
```

The firmware binary is sent as:

``` text
application/octet-stream
```

The request must include:

``` http
X-OTA-Password: <OTA password>
```

------------------------------------------------------------------------

# 26. Concurrency and mutex design

The firmware uses three mutexes:

``` text
relay_mutex
storage_mutex
ota_mutex
```

## relay_mutex

Protects:

``` text
relay_state
relay_enabled
relay_name
```

It also protects coordinated relay GPIO updates.

## storage_mutex

Protects NVS transactions.

This prevents concurrent configuration writes from corrupting the
logical configuration flow.

## ota_mutex

Prevents multiple OTA operations from running at the same time.

The global OTA flag:

``` c
ota_in_progress
```

also blocks relay/configuration changes while an OTA operation is
active.

------------------------------------------------------------------------

# 27. Watchdog behavior

The firmware configures the ESP-IDF Task Watchdog Timer with:

``` text
Timeout: 10 seconds
```

The physical-switch task registers with the watchdog.

The local DNS task also registers with the watchdog and periodically
resets it.

This provides protection against those long-running tasks becoming
permanently stuck.

The watchdog is initialized before those tasks are created.

------------------------------------------------------------------------

# 28. Relay state persistence

Whenever the relay state changes through a physical switch or web
command, the new state is stored in NVS.

Therefore:

``` text
Power ON
   |
Relay state = saved state
```

and after a power interruption:

``` text
Power OFF
   |
Power ON
   |
NVS restored
   |
Relay state restored
```

This is intended to make repeated power interruptions less disruptive.

------------------------------------------------------------------------

# 29. Important distinction: state vs switch position

The physical switch is treated as an input command source.

At boot, the firmware establishes a baseline from the physical switch
without automatically changing the restored relay state.

Only a later physical transition is treated as a new command.

This avoids the following unwanted behavior:

``` text
Web: Relay ON
Power cut
Power returns
Switch did not move
Firmware sees switch position
Firmware unnecessarily forces Relay OFF
```

Instead, the saved relay state can be restored.

------------------------------------------------------------------------

# 30. 24/7 operation design goals

The firmware has been structured with continuous operation in mind.

Important design choices include:

-   No cloud dependency
-   No Internet dependency
-   NVS persistence
-   Mutex-protected shared state
-   Physical switch debounce
-   Safe relay initialization
-   OTA partitioning
-   OTA mutual exclusion
-   Watchdog supervision
-   Small local HTTP interface
-   Small local DNS task
-   Bounded input buffers
-   Validation of configuration values
-   Constant-time password comparison

However, no firmware can guarantee failure-free operation for every
hardware/environment combination.

For a true year-round installation, the hardware side is equally
important:

-   Use a reliable regulated power supply.
-   Use an appropriate relay module for the load.
-   Provide proper electrical isolation.
-   Use suitable enclosures.
-   Keep mains wiring separated from ESP32 low-voltage wiring.
-   Protect against surges where appropriate.
-   Use correctly rated terminals, wire and protection devices.
-   Ensure adequate thermal conditions.

------------------------------------------------------------------------

# 31. Power interruption considerations

The ESP32 will reboot when its supply disappears and later returns.

On restart, the firmware initializes NVS and restores
configuration/state.

The relay outputs are first driven to their safe OFF electrical level
before the saved logical state is applied.

This minimizes uncontrolled output behavior during boot.

Frequent power interruptions should therefore be expected as a normal
operating condition rather than treated as an exceptional software
event.

------------------------------------------------------------------------

# 32. Firmware defaults

The main defaults currently defined in the firmware are:

``` text
AP SSID:
ESP32-SMART-HOME

AP password:
ChangeMe123

AP channel:
6

AP IP:
192.168.4.1

Relay active level:
1

Default OTA password:
OTA@ESP32#2026
```

Relay defaults:

``` text
Relay 1:
Enabled
GPIO 16
Switch GPIO 32
Living Room Light

Relay 2:
Enabled
GPIO 17
Switch GPIO 33
Ceiling Fan

Relay 3:
Enabled
GPIO 18
Switch GPIO 25
Charging Socket

Relay 4:
Disabled
GPIO 19
Switch GPIO 26

Relay 5:
Disabled
GPIO 21
Switch GPIO 27
```

------------------------------------------------------------------------

# 33. First-time setup

Recommended first boot sequence:

### Step 1

Flash the firmware.

### Step 2

Power the ESP32.

### Step 3

Connect a phone to:

``` text
ESP32-SMART-HOME
```

using the configured AP password.

### Step 4

Open:

``` text
http://192.168.4.1/
```

If the operating system opens the captive portal automatically, use that
page.

### Step 5

Verify Relay 1-3.

### Step 6

Go to:

``` text
Settings -> Relay Configuration
```

if Relay 4 or Relay 5 is required.

### Step 7

Rename the relays.

### Step 8

If desired, change:

``` text
Settings -> AP Configuration
```

### Step 9

Immediately change the default OTA password:

``` text
Settings -> OTA Password
```

------------------------------------------------------------------------

# 34. Recommended deployment checklist

Before permanent installation:

-   [ ] Change the default AP password.
-   [ ] Change the default OTA password.
-   [ ] Verify every relay GPIO.
-   [ ] Verify every physical-switch GPIO.
-   [ ] Verify relay polarity.
-   [ ] Test all relay channels.
-   [ ] Test all physical switches.
-   [ ] Test web control.
-   [ ] Test web/physical synchronization.
-   [ ] Test power interruption recovery.
-   [ ] Test Relay 4 enable/disable.
-   [ ] Test Relay 5 enable/disable.
-   [ ] Test relay renaming.
-   [ ] Test AP configuration.
-   [ ] Test OTA with the correct password.
-   [ ] Confirm that an incorrect OTA password is rejected.
-   [ ] Confirm that the device reboots correctly after OTA.
-   [ ] Check all mains-side wiring separately and safely.

------------------------------------------------------------------------

# 35. Build environment

This firmware is written for ESP-IDF.

The source uses ESP-IDF APIs including:

``` text
FreeRTOS
ESP Wi-Fi
ESP-NETIF
ESP HTTP Server
ESP OTA
ESP Partition
NVS
GPIO
Task Watchdog
```

The project should be built using the ESP-IDF version selected by the
repository's build configuration.

Do not randomly mix incompatible ESP-IDF versions without testing.

------------------------------------------------------------------------

# 36. Important source files

A normal repository layout is expected to contain files similar to:

``` text
project/
├── CMakeLists.txt
├── partitions.csv
├── sdkconfig.defaults
├── README.md
├── main/
│   ├── CMakeLists.txt
│   └── main.c
└── .github/
    └── workflows/
        └── build.yml
```

The exact repository layout may vary, but the firmware logic itself is
primarily contained in:

``` text
main/main.c
```

------------------------------------------------------------------------

# 37. What this project intentionally does NOT include

The current architecture intentionally does not depend on:

-   Cloud control
-   Internet
-   MQTT
-   Home Assistant
-   Alexa
-   Google Home
-   External database
-   Smartphone application
-   Remote WAN access
-   Online scheduler

This keeps the controller self-contained and functional even when the
Internet is unavailable.

------------------------------------------------------------------------

# 38. Security limitations

This is an offline local-control system, not a hardened enterprise
security appliance.

The AP password controls access to the local Wi-Fi network.

The OTA password separately protects firmware update authorization.

But because the interface is HTTP rather than HTTPS:

``` text
Traffic is not cryptographically encrypted.
```

For a home LAN/AP-only installation this may be an acceptable trade-off,
but it should be understood before deploying the device in a hostile
environment.

For higher security, the architecture should be extended with ESP-IDF
security features and authenticated/signed firmware.

------------------------------------------------------------------------

# 39. Troubleshooting

## Cannot see the ESP32 Wi-Fi network

Check:

-   ESP32 power
-   boot logs
-   AP SSID
-   AP password
-   Wi-Fi channel
-   antenna/module hardware

------------------------------------------------------------------------

## Web page does not open

Try:

``` text
http://192.168.4.1/
```

Also disconnect the phone from other networks if the operating system is
trying to route traffic through another connection.

------------------------------------------------------------------------

## Relay does not switch

Check:

1.  Relay GPIO.
2.  Relay module supply.
3.  Relay active-high/active-low polarity.
4.  Ground/common wiring.
5.  Relay enable state.
6.  Boot logs.

If the relay module is active-low, change:

``` c
#define RELAY_ACTIVE_LEVEL 0
```

------------------------------------------------------------------------

## Physical switch does not work

Check:

``` text
Switch -> GPIO
Switch -> GND
```

and verify the assigned GPIO.

Current mapping:

``` text
Switch 1 -> GPIO 32
Switch 2 -> GPIO 33
Switch 3 -> GPIO 25
Switch 4 -> GPIO 26
Switch 5 -> GPIO 27
```

Do not apply mains voltage to these inputs.

------------------------------------------------------------------------

## Relay 4/5 do not appear

Open:

``` text
Settings
 -> Relay Configuration
```

Enable the required relay and save.

The relay should then appear on the main page.

------------------------------------------------------------------------

## AP configuration changed but phone disconnected

This is expected.

The ESP32 restarts after saving the new AP configuration.

Reconnect using the new SSID/password.

------------------------------------------------------------------------

## OTA password rejected

Verify:

-   You are using the OTA password, not the Wi-Fi password.
-   The password is at least 8 characters.
-   The password has not previously been changed.
-   There is no accidental leading/trailing whitespace.

------------------------------------------------------------------------

## OTA upload fails

Check:

-   Correct `.bin` file
-   Correct target board
-   Correct partition table
-   OTA partition availability
-   Correct OTA password
-   Stable power supply
-   Do not interrupt power during an update

------------------------------------------------------------------------

# 40. Design summary

The final control architecture is:

``` text
                 ┌───────────────────────┐
                 │        ESP32          │
                 │                       │
Phone ─ Wi-Fi ──>│ Local Web Server      │
                 │       │               │
                 │       v               │
                 │   Relay State         │
                 │       │               │
                 │       v               │
                 │  Relay GPIOs 1-5      │
                 │                       │
Switch 1 ───────>│ GPIO 32               │
Switch 2 ───────>│ GPIO 33               │
Switch 3 ───────>│ GPIO 25               │
Switch 4 ───────>│ GPIO 26               │
Switch 5 ───────>│ GPIO 27               │
                 │                       │
                 │ NVS                   │
                 │ ├─ relay states       │
                 │ ├─ relay config       │
                 │ ├─ AP credentials     │
                 │ └─ OTA password       │
                 │                       │
                 │ OTA / Partition       │
                 └───────────────────────┘
```

The key principle is simple:

> Web control and physical switches are two input methods for one
> centralized relay-control state.

This keeps the behavior predictable, local and independent of Internet
availability.

------------------------------------------------------------------------

# 41. Final note

This README describes the firmware behavior and configuration
represented by the current project version.

Before making future modifications, preserve the existing GPIO
assignments, NVS keys, OTA partition structure and synchronization logic
unless a deliberate architecture change is required.

For a permanent mains installation, the software should be considered
only one part of the system. Electrical isolation, relay ratings,
enclosure, power supply, surge protection and safe mains wiring are
equally important.
