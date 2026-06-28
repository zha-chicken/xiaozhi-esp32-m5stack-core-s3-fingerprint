# M5Stack CoreS3 Fingerprint Firmware

This branch is prepared for M5Stack CoreS3 with an M5Stack Unit Finger2 fingerprint module. After flashing, the device boots into a fingerprint-locked UI. Agent conversation is blocked until a local fingerprint match succeeds.

## Hardware

- M5Stack CoreS3
- M5Stack Unit Finger2 connected by UART
- Tested wiring:
  - Finger2 TX -> CoreS3 G18
  - Finger2 RX -> CoreS3 G17
  - 5V and GND from the Grove connector
- USB Type-C from the computer to CoreS3 for build flashing and serial monitor

The firmware also probes Port A UART variants and the older FPC1020A protocol, but the verified setup is Finger2 on G18/G17 at 115200 baud.

## Build Environment

Install ESP-IDF 5.5 or newer. This local setup used the ESP-IDF package under PlatformIO:

```bash
source ~/.platformio/packages/framework-espidf/export.sh
export PATH=~/.platformio/tools/tool-cmake/bin:~/.platformio/tools/tool-ninja:$PATH
```

Any equivalent ESP-IDF environment with `idf.py`, CMake, Ninja, and the ESP32-S3 toolchain should work.

## Optional First-Boot Wi-Fi

No real Wi-Fi password is committed to Git. If you want the firmware to auto-connect on first boot, copy the example file and fill in local credentials:

```bash
cp components/78__esp-wifi-connect/wifi_defaults_local.example.h \
   components/78__esp-wifi-connect/wifi_defaults_local.h
```

Then edit `components/78__esp-wifi-connect/wifi_defaults_local.h`:

```cpp
#define XIAOZHI_DEFAULT_WIFI_SSID "YOUR_WIFI_SSID"
#define XIAOZHI_DEFAULT_WIFI_PASSWORD "YOUR_WIFI_PASSWORD"
```

`wifi_defaults_local.h` is ignored by Git. If it is not present and no Wi-Fi is stored in NVS, the device falls back to the normal Wi-Fi configuration AP flow.

## Build

From the repository root:

```bash
idf.py set-target esp32s3
idf.py build
```

The checked-in default board selection is `m5stack-core-s3`, so no menuconfig step is needed on a fresh clone. If you changed build configuration, make sure it still targets CoreS3 before building.

## Flash

Connect CoreS3 over USB Type-C. On this machine the port was `/dev/cu.usbmodem3101`:

```bash
idf.py -p /dev/cu.usbmodem3101 flash
```

Use the port shown on your machine if different. To monitor logs:

```bash
idf.py -p /dev/cu.usbmodem3101 monitor
```

Exit monitor with `Ctrl+]`.

> NOTE: To enter download mode manually, hold reset for about 3 seconds until the internal indicator turns green, then release.

## Fingerprint Behavior

- Boot default: locked.
- Locked UI: center badge shows `LOCK`, `指纹解锁`, and `通过后可对话`; ticker shows `请按指纹解锁`.
- Short touch / wake word / direct Agent listen calls are blocked while locked.
- A successful fingerprint match unlocks conversation for 60 seconds.
- After unlocking, tap the avatar once to start Agent listening.
- After the unlock window expires and the device returns to idle, the lock UI comes back.

## Local Enrollment Test

Long-press the CoreS3 touch screen for about 3 seconds to start a local enrollment test for fingerprint ID 1. Follow the serial logs while placing and lifting the same finger.

Useful logs:

```text
Fpc1020a: finger2 ready ... users=1
M5StackCoreS3Board: Local fingerprint enrollment test started for id=1
Fpc1020a: Fingerprint unlock matched user=1 ... unlocked_for_ms=60000
M5StackCoreS3Board: Conversation blocked by fingerprint lock ...
```

## Service Endpoint

With the current OTA configuration, the device receives the WebSocket endpoint from `wexiyi.com` and connects to:

```text
wss://wexiyi.com/agents/ws
```

If the server rejects pairing or WebSocket handshakes, the fingerprint lock and local enrollment test can still be validated independently from serial logs.
