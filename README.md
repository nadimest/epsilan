# Epsilan

Native C firmware for running a compact SiLA 2 server on an M5Stack CoreS3.

The firmware brings up the display and BMI270 accelerometer, publishes readings over USB at 2 Hz, and exposes them as a SiLA observable property over plaintext gRPC. It creates and persists an Epsilan server UUID and the selected backlight setting in NVS. On an unconfigured board it starts secure BLE Wi-Fi provisioning compatible with Espressif's official app; once joined, it reconnects automatically after a restart.

The same property is available through the SiLA 2 server-initiated cloud connection over TLS. A generated Python client can discover the board, fetch its feature definitions, and subscribe without carrying hand-written protobuf code.

Use the full CoreS3. CoreS3 SE lacks the motion sensor. This build uses Espressif's CoreS3 BSP 3.0.0 and BMI270 driver 1.1.0, with transitive dependencies pinned in `firmware/dependencies.lock`. Different LCD revisions may require a newer BSP.

Read [the longer engineering note](DEMO.md) for what this proves, where an ESP32 fits in lab automation, and which operational pieces still need work.

## Where this fits

An ESP32 is credible for months of continuous operation when power, watchdogs, failure states, and firmware updates are engineered deliberately. It fits custom instruments and bioreactors where local sensing and control must continue through a network outage.

Linux buys a mature driver environment, persistent logs, remote shell access, and the full Python SiLA development stack. That can lower development and support costs for one-off serial or USB connectors.

For a bioreactor, keep time-sensitive control and safe output states local. SiLA can expose setpoints, readings, alarms, and operations to supervisory systems while the device keeps running on its own.

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

The display should show acceleration in m/s², memory information, its IP address, and `SiLA :50052`. Tilt the device to change the gravity vector. `info` prints the persistent Epsilan UUID, memory, backlight, and cloud settings. Commands are newline terminated: `info`, `backlight 1..100`, and `wifi <ssid-without-spaces> <password>`.

On first boot the display shows a provisioning QR code, a `PROV_XXXXXX` BLE service name, and a proof-of-possession (PoP). In Espressif's [ESP BLE Provisioning app for iOS](https://apps.apple.com/us/app/esp-ble-provisioning/id1473590141) or [Android](https://play.google.com/store/apps/details?id=com.espressif.provble), scan the code, select a 2.4 GHz network, and enter its password. The QR content is provisioning data for that app, so a normal camera app is not expected to open it as a web page. The direct USB `wifi` command remains available for bench setup.

Provisioning state and Wi-Fi credentials survive restart. On the main screen, press and hold **Reset Wi-Fi** to erase the saved network and return to provisioning; a normal tap does not reset it. Setup secrets are random per board and stored in NVS. The minimum brightness is deliberately above zero for bring-up. The USB command interface remains available independently of the network SiLA endpoint.

## Browse and generate a Python connector

The server advertises `_sila._tcp.local` and currently implements:

- `org.silastandard/core/SiLAService/v1`
- `io.epsilan/sensors/Accelerometer/v1`
- `io.epsilan/cloud/CloudConfiguration/v1`

The `CloudConfiguration` feature exposes `SetCloudConnection` with endpoint, port, TLS, and enabled parameters, plus read-only properties for the saved values. A successful update persists to NVS and restarts the device after returning the RPC response.

Using Labplane's `unitelabs-sila` integration:

```sh
cd /path/to/labplane
uv run sila-codegen discover --timeout 5
uv run sila-codegen generate DEVICE_IP:50052 \
  --name EpsilanCoreS3 \
  --output /tmp/epsilan_core_s3.py
```

The generated facade can subscribe to the onboard sensor without knowing its protobuf schema:

```python
import asyncio

from epsilan_core_s3 import EpsilanCoreS3


async def main() -> None:
    async with await EpsilanCoreS3.connect(
        "DEVICE_IP:50052", timeout=8_000
    ) as device:
        samples = await device.accelerometer.subscribe_acceleration(timeout=4_000)
        try:
            async for sample in samples:
                print(sample)  # {'X': ..., 'Y': ..., 'Z': ...}
        finally:
            samples.close()


asyncio.run(main())
```

Timeouts passed to `unitelabs-sila` are milliseconds. The current native server is a deliberately small first implementation: plaintext only, one active TCP client at a time, a 512-byte request limit, and up to eight concurrent HTTP/2 streams. Close a browser or generated connector before opening the next client; long-running observable streams remain open until the client cancels or disconnects.

Two 4 MiB application partitions reserve space for future OTA. This does not enable OTA by itself. The remaining flash is unassigned. The outbound SiLA 2 v1.1 cloud transport now connects over TLS, handles unary commands and properties, streams acceleration to up to four subscribers, accepts cancellation, and reconnects with backoff. Cloud messages are capped at 2 KiB. Device enrollment and per-device authentication, observable commands, binary transfer, OTA management, secure boot, flash encryption, and eFuse changes are not implemented yet.

## Restore this board's factory backup

Use the verified backup for this board, not an image from another device. With the board in download mode:

```sh
PORT=/dev/cu.usbmodemXXXX
.tools/serial-venv/bin/python -m esptool --chip esp32s3 --port "$PORT" write-flash 0 backups/cores3-factory-2026-09-16.bin
```

The separate `serial-venv` is the local esptool 5.4.0 environment used during initial bring-up. The setup script installs ESP-IDF's esptool 4.x instead; its equivalent restore command is `python -m esptool --chip esp32s3 --port PORT write_flash 0 BACKUP.bin` using the IDF Python environment.
