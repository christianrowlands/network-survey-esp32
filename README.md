<picture>
  <source media="(prefers-color-scheme: dark)" srcset="flasher/img/ns-lockup-dark.svg">
  <img src="flasher/img/ns-lockup-light.svg" alt="Network Survey" width="280">
</picture>

# Network Survey board firmware

Firmware that turns an inexpensive ESP32 board into a passive Wi-Fi and
Bluetooth LE survey sensor for the Network Survey iPhone app.

iOS has no API for scanning Wi-Fi, and it hides the real addresses of
Bluetooth devices. So the board does the listening: it hears 802.11 frames and
BLE advertisements, summarizes them, and sends the summaries to the phone over
Bluetooth LE. The phone adds location and does everything else: storage, CSV
and pcapng files, MQTT and gRPC streaming, and upload to NS Analytics.

The iPhone app lives in its own repository:
[network-survey-ios](https://github.com/christianrowlands/network-survey-ios).

## Receive only

The board never transmits 802.11 frames. Wi-Fi capture uses promiscuous
receive on one channel at a time, set by the firmware: no probe requests, no
active scans, no association, no deauthentication or any other injected frame.
BLE scanning is passive too (no scan requests). The board transmits only its
own Bluetooth LE advertising and the link to your phone.

`scripts/check-passive.sh` enforces this in CI by failing the build if 802.11
transmit, active scan or deauthentication code appears in the firmware.

Use the board only where you are allowed to survey.

## Supported boards

| Board | Chip | Bands | Status |
|---|---|---|---|
| Espressif ESP32-C5-DevKitC-1 | ESP32-C5 | 2.4 and 5 GHz | Tested on hardware |
| Seeed Studio XIAO ESP32-C5 | ESP32-C5 | 2.4 and 5 GHz | Builds; pins from vendor docs, not yet tested on hardware |
| Espressif ESP32-C6-DevKitC-1 | ESP32-C6 | 2.4 GHz | Builds; pins from vendor docs, not yet tested on hardware |
| Espressif ESP32-S3-DevKitC-1 | ESP32-S3 | 2.4 GHz | Builds; pins from vendor docs, not yet tested on hardware |
| Espressif ESP32-C3-DevKitM-1 | ESP32-C3 | 2.4 GHz | Builds; pins from vendor docs, not yet tested on hardware |

The ESP32-C5 is the recommended chip: it is the only one that receives on
5 GHz. Pin maps, GPS wiring, battery input and enclosure notes are in
[`docs/hardware.md`](docs/hardware.md).

## Getting the firmware onto a board

**Web installer.** The easiest way. Open the installer page in Chrome or Edge
on a computer, plug the board in with a USB data cable, and click Install for
your board. The page, <https://install.networksurvey.app/>, is published from
this repository to GitHub Pages with every release. Its
source and local test steps are in [`flasher/README.md`](flasher/README.md).

**esptool.** Each [release](https://github.com/christianrowlands/network-survey-esp32/releases)
has a full flash image per board:

```sh
python -m esptool --chip esp32c5 -p PORT write-flash 0x0 \
    network-survey-board-esp32c5-devkitc1-<version>-full-flash.bin
```

Use your board's chip (`esp32c5`, `esp32c6`, `esp32s3`, `esp32c3`). A full
flash also deletes the board's Bluetooth pairings.

**Build it yourself.** See below.

After the first install, the iPhone app updates the board over Bluetooth.

## Pairing with the app

A new board has no pairings, so it is ready to pair. In the app, open
Survey > Boards and tap the board under Nearby (it shows as `NS-XXXX`). When
iOS asks, tap Pair. To pair a board that already has a phone, hold its BOOT
button for about 3 seconds until the LED blinks cyan, then pair. LED colors,
button actions and the pairing rules are in
[`firmware/README.md`](firmware/README.md#led-and-button).

## Building

Install [ESP-IDF v6.0.1](https://docs.espressif.com/projects/esp-idf/en/v6.0.1/)
and activate it in your shell. Then, from the repository root:

```sh
scripts/build-all-targets.sh                     # every board
scripts/build-all-targets.sh esp32c5-devkitc1    # one board
```

Images go to `dist/<board>/`. Flashing a local build, `idf.py` use and the
board options are in [`firmware/README.md`](firmware/README.md).

## Tests and checks

These need only a C compiler, not ESP-IDF:

```sh
make -C test/host           # host tests: 802.11 parser, aggregator, channel planner,
                            # link framing, OTA, pairing policy, BLE, NMEA, raw frames,
                            # protocol fixtures
scripts/check-passive.sh    # no 802.11 transmit, active scan or deauthentication code
scripts/check-text.sh       # no em dashes or home-directory paths in tracked files
```

`scripts/gen-proto.sh --check` confirms the generated protocol code matches
`proto/ns_board.proto` (needs `protoc`, see [`proto/README.md`](proto/README.md)).
CI runs all of these and builds every board on each push
([`.github/workflows/firmware.yml`](.github/workflows/firmware.yml)).

## Board protocol

The board and the app talk over one custom GATT service. Messages are Protocol
Buffers defined in [`proto/ns_board.proto`](proto/ns_board.proto), the source of
truth. [`docs/protocol.md`](docs/protocol.md) describes advertising, pairing and
security, framing, control commands, records, raw frames, firmware updates and
versioning. Anyone can build a compatible board or app from it.

## Firmware updates and signing

The app installs updates over Bluetooth into the second of two app slots. The
board checks the image's SHA-256 before it switches, and rolls back to the old
image if the new one does not start up and confirm itself.

Official releases are signed with an Ed25519 key that is held only as a CI
secret. The app verifies the signed release manifest and each image against
the public keys built into it before it sends anything to the board. The board
itself does not check signatures, so a phone that is paired with the board can
install any firmware; secure boot on the chip is possible future work. The
details are in:

- [`docs/release-manifest.md`](docs/release-manifest.md): the signed manifest
  and what the app checks.
- [`docs/releasing.md`](docs/releasing.md): making a release, the CI workflows
  and the one-time key setup.
- [`SECURITY.md`](SECURITY.md): the security model and how to report a
  problem.

`scripts/keys/dev-signing-key.pub` is a development key for local testing
only. It never signs release firmware.

## Repository layout

| Path | Contents |
|---|---|
| `firmware/` | ESP-IDF project: startup, board configs, survey pipeline components |
| `proto/` | `ns_board.proto` and nanopb options |
| `flasher/` | Web installer page |
| `scripts/` | Build, codegen, signing and check scripts |
| `test/` | Host tests and wire fixtures |
| `docs/` | Protocol, hardware, release manifest and release process |

## Contributing

Bug reports, board ports and fixes are welcome. Read
[`CONTRIBUTING.md`](CONTRIBUTING.md) first.

## License

Apache License 2.0. See [`LICENSE`](LICENSE) and [`NOTICE`](NOTICE).
