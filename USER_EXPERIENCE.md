# Epsilan device journey

## Product decision

Ship one generic, signed Epsilan image. Do not bake a laboratory's Wi-Fi password,
cloud token, or SiLA endpoint into a flashed binary.

Each device receives a stable device ID and a per-device setup secret at
manufacture or first wired installation. The technician then claims the device
for a laboratory and supplies its network configuration locally. From that point
the device makes an outbound TLS connection to the Epsilan cloud gateway. It
does not require an inbound connection through the laboratory firewall.

This keeps a replacement board, a demo unit, and a production instrument on the
same installation path. It also means Wi-Fi credentials can be changed without
reflashing firmware.

## Recommended first-use flow

```mermaid
sequenceDiagram
    participant T as Technician phone / desktop
    participant D as Epsilan device
    participant C as Epsilan cloud

    T->>D: Power on
    D->>T: Screen shows QR code and setup state
    T->>C: Sign in and scan QR code
    C->>T: Create short-lived claim session
    T->>D: BLE provisioning: claim token + lab Wi-Fi details
    D->>C: Outbound TLS connection; prove device identity
    C->>D: Confirm tenant, configuration, update channel
    D->>T: Screen shows Connected and assigned instrument
```

The technician should see only three states on the CoreS3 screen:

1. **Ready to set up** — QR code, device name, and a short setup code.
2. **Connecting** — the selected Wi-Fi network name and progress.
3. **Connected** — laboratory/instrument name, cloud connection state, IP address,
   and sensor or serial-device status.

The setup code is a proof that the person holding the device is the person
claiming it. The mobile or desktop application receives a short-lived claim
token from the cloud and passes it locally to the device. The device then uses
that token only to establish its own durable device identity with the cloud.

## Local transport choices

Use **BLE provisioning as the primary flow**. It avoids the disruptive step of
asking a phone to leave its normal Wi-Fi network to join the device. It also lets
the phone stay connected to the cloud during setup.

Keep these recovery and deployment paths:

| Path | Use | User interaction |
|---|---|---|
| BLE + QR code | Normal installation | Scan, select lab Wi-Fi, approve claim |
| SoftAP + QR code | BLE unavailable or desktop setup | Join the temporary device network, open the setup page/app |
| USB serial | Manufacturing, bench work, recovery | A local installer sends credentials and claim data |
| USB flash | First installation or repair only | Flash the generic signed image; no lab secrets in the image |

The prototype now implements the primary BLE path using ESP-IDF provisioning
and Espressif's official mobile app. Its screen shows the app-compatible QR
code, BLE service name, and proof-of-possession. Provisioning runs only while
the device is unconfigured or after an explicit long-press network reset. A
SoftAP recovery path remains a future option if field experience justifies its
flash and maintenance cost.

ESP32-S3 Wi-Fi is 2.4 GHz. The installer must make this clear before setup; a
laboratory with a 5 GHz-only SSID needs a compatible 2.4 GHz or dual-band
network. Later releases should add WPA2-Enterprise support for managed lab
networks.

## Buttons and recovery

Give the physical device predictable behavior:

| Action | Result |
|---|---|
| Short press | Wake screen / show connection and device status |
| Hold 3 seconds | Open setup mode without deleting the cloud identity |
| Hold 10 seconds, then confirm on screen | Remove Wi-Fi and local configuration, retain firmware and hardware identity |
| USB boot mode | Repair and manufacturing only |

Do not use a network reset to silently unclaim a device. A separate cloud action
with a local confirmation should transfer or remove ownership.

## Cloud roles

Keep the two network roles separate:

- **Control plane**: claim, credentials/certificates, configuration, health,
  device inventory, audit events, and firmware rollout.
- **SiLA gateway**: maps the device's outbound reduced C transport to the SiLA
  gRPC service visible to lab software. The ESP32 need not expose a full inbound
  gRPC server on the laboratory LAN.

The ESP32 sends telemetry and receives commands through a single authenticated
outbound connection over TLS on port 443. For the first prototype, use an
application-defined framed protobuf stream. The cloud gateway performs normal
gRPC/SiLA work. This preserves SiLA semantics while fitting ESP32 memory and
network constraints.

## ESPHome-like lifecycle, implemented for Epsilan

ESPHome is a useful product reference: it provides straightforward OTA updates,
safe recovery, and a dashboard. Its native OTA protocol and runtime are not a
good coupling point for our custom C SiLA device agent.

Build a small Epsilan management layer instead:

1. Device checks an HTTPS update manifest after connecting and periodically
   thereafter.
2. The manifest selects a version by hardware model, feature set, and rollout
   channel (`stable`, `pilot`, `development`).
3. Device downloads a signed image into the inactive OTA partition.
4. On the next boot it verifies Wi-Fi, display/sensor initialization, and cloud
   reachability before marking the image healthy.
5. A failed first boot automatically returns to the previous image; the cloud
   records the failure.

The existing partition table already has two 4 MiB OTA application slots and
OTA metadata, which is the required starting point. ESP-IDF supports OTA
rollback, signed images with Secure Boot, and anti-rollback once the product
security model is ready. Do not burn irreversible eFuse security settings during
prototype work.

## Delivery sequence

1. **Bring-up complete**: sensor telemetry and controlled commands on USB.
2. **Setup prototype complete**: BLE provisioning through Espressif's app, QR
   screen, persistent Wi-Fi state, and a long-press network-reset flow.
3. **Cloud milestone**: device claim API and outbound TLS connection; use it for
   health and sensor telemetry.
4. **SiLA milestone**: reduced protobuf command/event stream on the device and
   a cloud gRPC/SiLA gateway.
5. **Lifecycle milestone**: signed HTTPS OTA manifest, staged rollouts, health
   confirmation, rollback, inventory, and audit trail.

## Sources

- [ESP-IDF OTA and rollback](https://docs.espressif.com/projects/esp-idf/en/v5.2/esp32s3/api-reference/system/ota.html)
- [ESP-IDF security and signed OTA](https://docs.espressif.com/projects/esp-idf/en/v5.3.2/esp32s3/security/security.html)
- [ESPHome OTA mechanisms](https://esphome.io/components/ota/)
