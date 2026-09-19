# We turned an ESP32 into a SiLA server

*A tiny hardware demo, plus a useful warning label for lab automation.*

Pick up an M5Stack CoreS3. Flash it. Connect it to Wi-Fi. The 240 MHz microcontroller now advertises itself on the lab network as a SiLA server, publishes live data from its onboard sensors, and opens an outbound TLS connection to a cloud SiLA endpoint.

That sentence sounds almost suspiciously easy. The code behind it is where the lesson lives.

## What we built

Epsilan is native C firmware built on ESP-IDF 5.4.3. It turns an M5Stack CoreS3 into a compact SiLA 2 server.

The current build:

- Reads onboard motion, optical, proximity, and power telemetry twice per second and shows it on an instrument-panel display.
- Exposes acceleration, angular rate, ambient light, proximity, and power state as typed, observable SiLA properties.
- Runs a local gRPC server on port 50052 and announces it through mDNS.
- Implements `SiLAService`, `Accelerometer`, `Gyroscope`, `OpticalSensor`, `PowerStatus`, and `CloudConfiguration`, including feature-definition retrieval.
- Joins 2.4 GHz Wi-Fi through BLE provisioning and a QR code, with USB setup as a bench fallback.
- Stores its UUID, Wi-Fi state, screen brightness, and cloud settings in nonvolatile storage.
- Opens the SiLA 2 server-initiated cloud connection over HTTP/2 and TLS, handles cloud requests, streams every telemetry property, accepts cancellation, and reconnects with backoff.

A generated Python facade can discover the board, inspect its feature definitions, and subscribe without carrying hand-written protobuf code. Tilt the device, and the motion values move in the client; cover the optical sensor, and the proximity value reacts.

The hardware is small, yet it has enough room for this experiment: 16 MB of flash, 8 MB of PSRAM, built-in Wi-Fi, a display, and several onboard sensors. Those figures come from the [CoreS3 hardware documentation](https://docs.m5stack.com/en/core/CoreS3).

## Why this matters

Lab integration lives at the edge. A shaker speaks a terse serial command set. A balance sends a line of text when its reading settles. A home-built module exposes 2 GPIO pins and an I2C sensor.

SiLA gives each of those devices a typed, discoverable contract. Once acceleration becomes an observable SiLA property, the caller works with `X`, `Y`, and `Z` in meters per second squared. It no longer needs to know which sensor register produced the value.

Putting that contract on the microcontroller creates some interesting options. Instrument builders can ship the standard with the hardware. Labs can add a small network bridge to a sensor, relay, pump, barcode reader, or serial device. A cloud connection can start from inside the lab, avoiding an inbound firewall rule.

And the form factor is hard to ignore. The server fits in your hand, boots quickly, has direct access to GPIO, UART, I2C, and SPI, and can sit inside an instrument enclosure.

This widens the range of hardware that can participate in a SiLA system. It also gives the lab automation community a concrete embedded implementation to inspect, measure, break, and improve.

The demo stops at the CoreS3's onboard hardware. Connecting a shaker or balance adds an electrical interface, a device-specific command set, timeouts, parsing, and recovery rules. SiLA gives the result a common shape. The instrument conversation still has to be written and tested.

## Flashing takes one command

The first setup installs a pinned ESP-IDF toolchain. After that, the normal path is short:

```sh
bash scripts/idf.sh build
bash scripts/idf.sh -p "$PORT" flash
```

The current image builds to about 1.67 MiB. Each 4 MiB application slot still has 58% free.

USB flashing has a few physical wrinkles. You need the correct serial port, the board may need a 3-second reset-button hold to enter download mode, and keeping a factory backup is wise. Those are small chores for one device on a workbench.

At 50 devices, firmware distribution, rollback, inventory, configuration drift, and physical recovery become a fleet problem. The partition table already reserves 2 application slots. The updater, image signing, health check, and rollback policy still need to be built. ESP-IDF supplies the underlying [OTA and rollback machinery](https://docs.espressif.com/projects/esp-idf/en/v5.4.3/esp32s3/api-reference/system/ota.html), which helps. We still have to turn those primitives into a supportable product.

## The microcontroller makes you own the stack

This prototype had to handle details that disappear inside a mature server kit:

- gRPC message framing and protobuf encoding
- HTTP/2 stream state, flow control, and cancellation
- TLS certificate chains, ALPN, clock synchronization, and reconnect backoff
- mDNS discovery, feature-definition storage, request routing, and bounded memory
- Wi-Fi provisioning, persistent settings, diagnostics, and recovery behavior

Every one of those items has failure modes. A half-read frame needs buffering. A dead socket needs a retry policy. A subscription needs a lifetime. A certificate check needs a usable clock. A command that moves hardware needs stronger retry semantics than a sensor read.

The prototype deliberately keeps tight limits: 1 local TCP client, requests up to 512 bytes, 8 local HTTP/2 streams, cloud messages up to 2 KiB, and 12 cloud telemetry subscriptions. These limits make memory use easier to reason about. They also show how quickly a general server becomes a product of its own.

Security and conformance work remains. The LAN endpoint currently uses plaintext gRPC. The cloud connection validates the gateway certificate, while device enrollment and per-device credentials still need a design. Secure boot, flash encryption, key provisioning, and a systematic SiLA conformance review belong on the production list.

The [UniteLabs Connector Development Kit](https://docs.unitelabs.io/connector-development/getting-started/overview/) sits at the other end of this tradeoff. Its Python framework already carries SiLA 2 version 1.1 behavior, cloud connectivity, feature libraries, testing support, and maintained protocol code. Typed Python methods and decorators generate the SiLA surface, so connector authors can spend their time on the instrument. Its hardware-communication layer also provides serial connection and command abstractions, as shown in the [CDK hardware communication guide](https://docs.unitelabs.io/connector-development/tutorial/hardware-communication/).

That division of labor matters. The oddities in an instrument protocol already demand enough attention.

## What Linux buys you

An ESP32 can run continuously for months. Published systems have completed [30 days of continuous bioreactor monitoring](https://pubmed.ncbi.nlm.nih.gov/41908431/) and a [100-day deployment inside a CO2 incubator](https://pubmed.ncbi.nlm.nih.gov/42463569/).

For a one-off QInstruments shaker, an older balance with a serial port, or another unusual instrument, I would still consider a Raspberry Pi or another Linux edge computer. Mature drivers and familiar diagnostic tools can cut development and support work.

| Concern | M5Stack CoreS3 prototype | Raspberry Pi or Linux host |
|---|---|---|
| Service recovery | Reconnect logic and watchdog behavior live in the firmware. A stuck task needs deliberate detection and recovery. | [`systemd`](https://www.freedesktop.org/software/systemd/man/latest/systemd.service.html) can restart a failed process, apply start limits, order dependencies, and run watchdog checks. |
| Logs | Current logs go to the USB serial console. History disappears unless another system captures it. Flash logging needs retention rules and wear management. | `journald` collects service output and can keep it on disk with size and retention limits. See the [journal storage controls](https://www.freedesktop.org/software/systemd/man/252/journald.conf.html). |
| Remote diagnosis | Each diagnostic, metric, and recovery action needs firmware support or a cloud path. | SSH, `journalctl`, process inspection, packet capture, and familiar file tools are already there. |
| Instrument connections | GPIO and UART are close to the hardware. RS-232 and RS-485 still need the correct transceiver, wiring, isolation, and protocol code. | USB host support and the Linux driver model make common USB-to-serial adapters easier to use. Raspberry Pi also [documents its GPIO UARTs](https://www.raspberrypi.com/documentation/computers/configuration.html#configure-uarts), with the same need for correct voltage conversion. |
| SiLA development | C code must manage schemas, buffers, stream state, errors, and memory ceilings. | The full Python CDK can generate the SiLA surface and handle protocol behavior. |
| Updates | The firmware has space for dual-slot OTA, while delivery and rollback are future work. | Packages, files, service units, and remote deployment tools are available immediately. |
| Storage and buffering | Flash is limited, and frequent writes need care. Long offline periods require a custom queue. | A filesystem gives logs, databases, queue files, and room for long offline periods. |
| Capacity | The current server accepts 1 local client and fixed-size messages. New workloads need explicit memory budgets. | More memory and storage leave room for several clients, richer telemetry, and local protocol traces. |

ESP-IDF has watchdogs and can store a crash dump in flash for later analysis. The [ESP32-S3 core-dump documentation](https://docs.espressif.com/projects/esp-idf/en/v5.4.3/esp32s3/api-guides/core_dump.html) covers that path. These are capable building blocks. Linux packages the equivalent operational habits into tools that lab and IT teams already know.

The Pi has costs too. It draws more power, takes more space, has a larger software surface to patch, and adds an operating system to the bill of materials. Direct GPIO timing also deserves care on a multitasking OS.

## Where the ESP32 fits

I would choose an ESP32 when the connector is becoming part of the instrument itself: a fixed feature set, bounded messages, direct sensor or actuator access, fast boot, tight power or space limits, and a planned device-management path. A custom bioreactor fits this pattern well because local control can continue through a network outage.

I would choose Linux when the connector sits beside an existing instrument and needs unfamiliar USB drivers, substantial local storage, remote investigation, or frequent protocol changes.

For one shaker or balance, Linux may win on engineering time. Across a fleet of identical devices, the firmware investment is easier to justify and the ESP32 can become the better product architecture. Both platforms can run continuously; the design of power, recovery, updates, and safe states decides how well they do it.

## The useful lesson for SiLA

Epsilan proves that a useful slice of SiLA 2 can run on a microcontroller and talk to ordinary SiLA tooling. The experiment also draws the engineering boundary with a thick marker.

SiLA can travel all the way down to the device. The right place for the server still depends on who will update it, inspect it at 2 a.m., recover its logs, and explain what happened after a 14-hour run.
