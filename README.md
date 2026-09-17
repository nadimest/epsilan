# Epsilan

C firmware experiments toward a SiLA 2 instrument on M5Stack CoreS3.

The first firmware brings up the display and BMI270 accelerometer, publishes readings over USB at 2 Hz, and accepts USB commands. It creates and persists an Epsilan server UUID and the selected backlight setting in NVS. On an unconfigured board it starts secure BLE Wi-Fi provisioning compatible with Espressif's official app; once joined, it reconnects automatically after a restart. It does not implement gRPC or SiLA yet. The architecture and planned cloud connection are described in [RESEARCH.md](RESEARCH.md).

Use the full CoreS3. CoreS3 SE lacks the motion sensor. This build uses Espressif's CoreS3 BSP 3.0.0 and BMI270 driver 1.1.0, with transitive dependencies pinned in `firmware/dependencies.lock`. Different LCD revisions may require a newer BSP.

## Build on macOS

Requires Git and Python 3.11. The setup script downloads ESP-IDF v5.4.3 and installs the ESP32-S3 tools inside `.tools/`.

```sh
bash scripts/setup.sh
bash scripts/idf.sh build
```

## Flash and use

Flashing replaces the application and partition table. A verified full factory backup of the first connected board is kept locally under `backups/` and excluded from Git. Keep that backup when cleaning the workspace.

If necessary, hold the CoreS3 RESET button for about three seconds until the green LED lights, then release. Check the port with `ls /dev/cu.usbmodem*` and substitute it below.

```sh
PORT=/dev/cu.usbmodemXXXX
bash scripts/idf.sh -p "$PORT" flash
.tools/idf-tools/python_env/idf5.4_py3.11_env/bin/python scripts/usb_console.py --port "$PORT" --seconds 10 --command info
.tools/idf-tools/python_env/idf5.4_py3.11_env/bin/python scripts/usb_console.py --port "$PORT" --seconds 5 --command 'backlight 30'
```

The display should show acceleration in m/s² and memory information. Tilt the device to change the gravity vector. `info` prints the persistent Epsilan UUID, memory, and backlight setting. Commands are newline terminated: `info`, `backlight 1..100`, and `wifi <ssid-without-spaces> <password>`.

On first boot the display shows a provisioning QR code, a `PROV_XXXXXX` BLE service name, and a proof-of-possession (PoP). In Espressif's [ESP BLE Provisioning app for iOS](https://apps.apple.com/us/app/esp-ble-provisioning/id1473590141) or [Android](https://play.google.com/store/apps/details?id=com.espressif.provble), scan the code, select a 2.4 GHz network, and enter its password. The QR content is provisioning data for that app, so a normal camera app is not expected to open it as a web page. The direct USB `wifi` command remains available for bench setup.

Provisioning state and Wi-Fi credentials survive restart. On the main screen, press and hold **Reset Wi-Fi** to erase the saved network and return to provisioning; a normal tap does not reset it. Setup secrets are random per board and stored in NVS. The minimum brightness is deliberately above zero for bring-up. This is a development USB interface, not a SiLA endpoint.

Two 4 MiB application partitions reserve space for future OTA. This does not enable OTA by itself. The remaining flash is unassigned. The current firmware initializes NVS and connects to Wi-Fi, but it does not implement OTA, cloud connectivity, gRPC, SiLA, or change eFuses.

## Restore this board's factory backup

Use the verified backup for this board, not an image from another device. With the board in download mode:

```sh
PORT=/dev/cu.usbmodemXXXX
.tools/serial-venv/bin/python -m esptool --chip esp32s3 --port "$PORT" write-flash 0 backups/cores3-factory-2026-09-16.bin
```

The separate `serial-venv` is the local esptool 5.4.0 environment used during initial bring-up. The setup script installs ESP-IDF's esptool 4.x instead; its equivalent restore command is `python -m esptool --chip esp32s3 --port PORT write_flash 0 BACKUP.bin` using the IDF Python environment.
