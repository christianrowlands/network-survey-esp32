# Network Survey board firmware

ESP-IDF firmware that turns an ESP32 board into a passive Wi-Fi and BLE
survey sensor for the Network Survey app. The board talks to the phone over
BLE as described in [`docs/protocol.md`](../docs/protocol.md).

## Supported boards

| Board id | Board | Chip | 5 GHz | Status LED | BOOT button | Flash | Tested on hardware |
|---|---|---|---|---|---|---|---|
| `esp32c5-devkitc1` | Espressif ESP32-C5-DevKitC-1 | ESP32-C5 | yes | RGB, GPIO27 | GPIO28 | 8 MB | yes |
| `xiao-esp32c5` | Seeed Studio XIAO ESP32-C5 | ESP32-C5 | yes | yellow, GPIO27 | GPIO28 | 8 MB | no, pins unverified |
| `esp32c6-devkitc1` | Espressif ESP32-C6-DevKitC-1 | ESP32-C6 | no | RGB, GPIO8 | GPIO9 | 8 MB | no, pins unverified |
| `esp32s3-devkitc1` | Espressif ESP32-S3-DevKitC-1 | ESP32-S3 | no | RGB, GPIO48 | GPIO0 | 8 MB | no, pins unverified |
| `esp32c3-devkitm1` | Espressif ESP32-C3-DevKitM-1 | ESP32-C3 | no | RGB, GPIO8 | GPIO9 | 4 MB | no, pins unverified |

All five build. Only the ESP32-C5-DevKitC-1 has been run on hardware; the
other pin maps (LED, button, the XIAO's battery and GPS pins) come from vendor
documentation. [`docs/hardware.md`](../docs/hardware.md) has the full pin maps,
GPS wiring, battery and enclosure notes.

Each board has a `boards/<board id>.defaults` file (target, flash size,
partition table, board choice). Pins and the board model live in the "Network
Survey board" Kconfig menu (`components/board/Kconfig`), so a custom board only
needs a new defaults file. ESP32-S3-DevKitC-1 v1.1 boards have the LED on
GPIO38: set `CONFIG_NS_BOARD_LED_GPIO_NUM=38`.

## Build

Install [ESP-IDF v6.0.1](https://docs.espressif.com/projects/esp-idf/en/v6.0.1/)
and activate it in your shell. Then, from the repository root:

```sh
scripts/build-all-targets.sh                     # every board
scripts/build-all-targets.sh esp32c5-devkitc1    # one board
```

Output goes to `dist/<board id>/`:

- `network-survey-board-<board id>.bin`: merged image, flash at offset 0.
- `network-survey-board-<board id>-app.bin`: app image for OTA updates.
- `manifest.json`: manifest for the esp-web-tools web flasher.

To work on one board with `idf.py` directly (from `firmware/`):

```sh
B=esp32c5-devkitc1
idf.py -B build/$B -DSDKCONFIG=build/$B/sdkconfig \
       -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;boards/$B.defaults" build
idf.py -B build/$B -p PORT flash monitor
```

## Flash a merged image

```sh
python -m esptool --chip esp32c5 -p PORT write-flash 0x0 \
    dist/esp32c5-devkitc1/network-survey-board-esp32c5-devkitc1.bin
```

Use the chip of your board (`esp32c5`, `esp32c6`, `esp32s3`, `esp32c3`).

## LED and button

| LED | Meaning |
|---|---|
| Short blue blink every 2 s | Idle, waiting for a phone |
| Solid blue | Phone connected |
| Green, 1 Hz | Surveying |
| Cyan, fast blink | Pairable: no bonds yet, or the pairing window is open |
| Magenta, 2.5 Hz | Firmware update |
| Red double blink | Error |
| White, rapid flash for 2 s | All bonds deleted |

On boards with a single-color LED the same on/off patterns are used.

| BOOT button | Action |
|---|---|
| Hold about 3 s | Open the 60 s pairing window |
| Press within 5 s after power-up and hold about 10 s | Delete all bonds |

The board keeps one bond by default: pairing a new phone in the window
replaces the old phone's bond. To allow more phones, raise
`CONFIG_BT_NIMBLE_MAX_BONDS` (and `CONFIG_BT_NIMBLE_MAX_CCCDS`, about 8 per
bond). Pairing rules are in `docs/protocol.md` section 3.

Holding BOOT while the board powers up or resets starts the chip's ROM
download mode instead of the firmware, so press it just after power-up.

## GPS (optional)

A GPS receiver that sends NMEA 0183 (GGA and RMC, any talker) at 9600 baud can
be wired to one GPIO: the receiver's TX to the board's GPS RX pin, plus power
and ground. The board only listens. Set it in the "Network Survey board"
Kconfig menu:

| Option | Meaning |
|---|---|
| `CONFIG_NS_BOARD_GPS_NONE` / `_FITTED` / `_AUTO` | No GPS, always fitted, or detect at boot (default on the XIAO ESP32-C5) |
| `CONFIG_NS_BOARD_GPS_RX_GPIO` | GPIO for the receiver's TX line (XIAO ESP32-C5: D7, GPIO12) |
| `CONFIG_NS_BOARD_GPS_BAUD` | Baud rate, 9600 by default |
| `CONFIG_NS_BOARD_GPS_DETECT_S` | With detect: seconds to wait for valid NMEA after boot, 10 by default |

Wire only the receiver's TX line to the board. The board's UART0 TX pin
(GPIO11, D6 on the XIAO) still prints boot messages.

With detect, the board turns the GPS off (and frees the pin) if no sentence
with a valid checksum arrives in time. A fitted or detected GPS sets the has
GPS advertising flag and `BoardInfo.gps_present`; `BoardStatus.gps_state`
shows whether it has a fix, and `GpsFix` records go to the phone about once a
second while surveying. The XIAO build uses the USB port as its console so
the header's UART pins stay free for a GPS module. The GPS path was tested
with synthetic NMEA, not yet with a real receiver.

## BLE sniffing

With `Configure.ble` in a Wi-Fi role the board scans for BLE advertisements
20 ms of every 100 ms while it hops Wi-Fi channels; in `ROLE_BLE_SNIFFER` it
stops Wi-Fi reception and scans continuously. Scanning is passive and uses
extended scanning, so BLE 5 extended advertisements are seen too. Scanning
continues while a phone is connected (measured with an idle connection; the
effect of a phone streaming Records at full rate is not measured yet). The
interleave timing is in `components/ble_sniffer/include/ble_sniffer.h`.

Raw frame capture (`Configure.raw_frames`) sends frame bodies to the phone,
including other people's SSIDs and MAC addresses. Data frames, when enabled,
are cut after their MAC and LLC/SNAP headers (no payload), and EAPOL frames
are never captured, so no key handshake or PMKID leaves the board. It is off
by default and rate limited (docs/protocol.md section 9).

## Layout

| Path | Contents |
|---|---|
| `main/` | Startup and wiring between modules |
| `components/board/` | Board Kconfig, model, chip, battery input |
| `components/gatt_server/` | NimBLE peripheral, NS Board Service, advertising |
| `components/ui_io/` | Status LED patterns and BOOT button events |
| `components/ns_proto/` | Generated nanopb code for `proto/ns_board.proto` |
| `components/sniffer/`, `dot11_parser/`, `aggregator/`, `channel_planner/`, `link/`, `ota/`, `gps/`, `ble_sniffer/` | Survey pipeline modules |
| `partitions/` | Partition tables (two OTA slots) for 4 MB and 8 MB flash |

## Checks

```sh
scripts/check-passive.sh   # no 802.11 transmit, active scan or deauthentication code
make -C test/host          # host tests, no ESP-IDF needed
```

The board never transmits 802.11 frames. Wi-Fi capture is receive-only and
BLE scanning is passive.
