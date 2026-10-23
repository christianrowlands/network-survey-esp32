# Hardware guide

What you need to build a Network Survey board, how the supported boards are
wired, and how to add a GPS receiver and a battery.

## Status

Only the **Espressif ESP32-C5-DevKitC-1** has been tested on hardware so far.
The other boards build from this repository, but their pin maps come from the
vendors' documentation and have not been checked on a real board. Treat them
as unverified: if the LED, button or battery reading does not behave as
described, the pin settings below are the first thing to check. Reports and
fixes are welcome (see `CONTRIBUTING.md`).

## Choosing a board

| Board | Chip | Bands | Flash / PSRAM | Battery input | Status |
|---|---|---|---|---|---|
| Espressif ESP32-C5-DevKitC-1 (N8R8) | ESP32-C5 | 2.4 and 5 GHz | 8 MB / 8 MB | No | Tested |
| Seeed Studio XIAO ESP32-C5 | ESP32-C5 | 2.4 and 5 GHz | 8 MB / 8 MB | Yes | Unverified |
| Espressif ESP32-C6-DevKitC-1 | ESP32-C6 | 2.4 GHz | 8 MB / none | No | Unverified |
| Espressif ESP32-S3-DevKitC-1 | ESP32-S3 | 2.4 GHz | 8 MB / not used | No | Unverified |
| Espressif ESP32-C3-DevKitM-1 | ESP32-C3 | 2.4 GHz | 4 MB / none | No | Unverified |

- **Pick an ESP32-C5** if you can. It is the only chip here that receives on
  5 GHz, where most modern access points do their work. The C6, S3 and C3
  hear 2.4 GHz only.
- **XIAO ESP32-C5** is the small board meant for a pocket unit: it has a
  LiPo charger and battery input, and an external antenna connector.
- **DevKits** are easy to find and good for a desk or a car with USB power.
- PSRAM gives the board larger buffers. It is optional; boards without it
  use internal RAM.
- Other ESP32-C5, C6, S3 or C3 boards work too with a custom board
  configuration (see [Custom boards](#custom-boards)).

The app uses up to four boards at once. With several boards it splits the channels between them, so each board hops fewer channels and
misses less.

## Pin maps

These are the defaults in the "Network Survey board" Kconfig menu
(`firmware/components/board/Kconfig`). Each board's
`firmware/boards/<board>.defaults` selects its entry.

| Board | BOOT button | Status LED | Battery sense | GPS RX (default) |
|---|---|---|---|---|
| ESP32-C5-DevKitC-1 | GPIO28 | Addressable RGB, GPIO27 | none | none |
| XIAO ESP32-C5 | GPIO28 | Yellow, GPIO27, on when low | ADC GPIO6, enable GPIO26, divider x2 | D7 (GPIO12), detect at boot |
| ESP32-C6-DevKitC-1 | GPIO9 | Addressable RGB, GPIO8 | none | none |
| ESP32-S3-DevKitC-1 | GPIO0 | Addressable RGB, GPIO48 (v1.0) or GPIO38 (v1.1) | none | none |
| ESP32-C3-DevKitM-1 | GPIO9 | Addressable RGB, GPIO8 | none | none |

Verified on hardware: the ESP32-C5-DevKitC-1 row only. Everything else in
this table is unverified.

Notes:

- The BOOT button is a strapping pin on every board. Holding it while the
  board powers up or resets starts the chip's download mode instead of the
  firmware. The firmware's button actions (pairing window, deleting bonds)
  start after power-up; see `firmware/README.md`.
- GPIO27 on the C5 boards is a strapping pin as well. It is fine as an LED
  output, but do not add strong external pull resistors to it.
- ESP32-S3-DevKitC-1 v1.1 boards have the LED on GPIO38. Build with
  `CONFIG_NS_BOARD_LED_GPIO_NUM=38` for those.
- On the ESP32-C5-DevKitC-1 the serial console uses GPIO11 (TX) and GPIO12
  (RX). Do not wire a GPS to them.
- The XIAO build puts the console on the USB port so that the header's UART
  pins (D6 and D7, GPIO11 and GPIO12) stay free for a GPS module.

## GPS receiver (optional)

The phone's location is normally used for every record. A GPS receiver on the
board is useful when the board is mounted away from the phone (a roof box, a
backpack) or when the phone's fix is poor. In the app, Settings > Location
chooses between them:

- **Phone** (default): records use the phone's location. The board GPS fills
  in when the phone's fix is missing, older than 10 seconds or worse than
  50 m.
- **Board GPS**: records use the board GPS when it has a fix, and the phone
  otherwise.

**Modules.** Any GPS or GNSS receiver that sends NMEA 0183 at 9600 baud works,
as long as it outputs GGA and RMC sentences (any talker ID, so GPS-only and
multi-constellation receivers are both fine). Most low-cost modules do this
out of the box, for example ones based on u-blox NEO-6M or NEO-M8N, or
Quectel L76K. Pick a 3.3 V module, or one with its own regulator.

**Wiring.** The board only listens, so three wires are enough:

| GPS module | Board |
|---|---|
| TX | The GPS RX GPIO (XIAO ESP32-C5: D7, GPIO12) |
| VCC | 3V3 (or 5V if the module needs it and has a regulator) |
| GND | GND |

Leave the module's RX pin unconnected.

**Configuration.** The XIAO build looks for a GPS at boot by default. On the
other boards, build with the GPS options set:

| Option | Meaning |
|---|---|
| `CONFIG_NS_BOARD_GPS_NONE` / `_FITTED` / `_AUTO` | No GPS, always fitted, or detect at boot |
| `CONFIG_NS_BOARD_GPS_RX_GPIO` | The GPIO wired to the module's TX |
| `CONFIG_NS_BOARD_GPS_BAUD` | Baud rate, 9600 by default |
| `CONFIG_NS_BOARD_GPS_DETECT_S` | With detect: seconds to wait for valid NMEA after boot, 10 by default |

Choose a free GPIO that is not a strapping pin and not the console UART. With
"detect at boot", the board turns the GPS off and frees the pin when no valid
NMEA sentence arrives in time, so the same image works with and without a
module.

A board with a GPS sets the "has GPS" flag in its advertising and reports it
to the app, which shows it on the board's page. Fixes go to the phone about once a
second while surveying.

The GPS code has been tested with synthetic NMEA only, not yet with a real
module. The XIAO's D7 default is from the vendor pinout and is unverified.

## Battery

The XIAO ESP32-C5 has a LiPo charger: solder a single-cell 3.7 V LiPo to its
battery pads and it charges from USB. The firmware reads the battery voltage
through a divider on GPIO6 (enabled by driving GPIO26 high while measuring)
and reports it in the board's status. The app turns the voltage into a
percentage with a typical LiPo curve (4.20 V is 100%, 3.80 V about 50%,
3.30 V is 0%) and warns when it gets low. These pins come from the vendor's
documentation and are not yet verified on hardware.

The DevKits have no battery input. Power them from a USB power bank or a car
USB port. They then report no battery level, which the app shows as "No
battery info".

For a custom board with a battery divider, set `CONFIG_NS_BOARD_BATTERY_GPIO`
(an ADC pin), `CONFIG_NS_BOARD_BATTERY_ENABLE_GPIO` (or -1) and
`CONFIG_NS_BOARD_BATTERY_DIVIDER` (battery voltage divided by the voltage at
the pin).

## Antennas

- The DevKits use the PCB antenna on their module. Keep the antenna end of
  the board clear of metal, cables and the battery.
- The XIAO ESP32-C5 has a U.FL connector for an external antenna. Always
  attach the antenna that came with the board (or another 2.4/5 GHz antenna)
  before surveying. Reception is much weaker without it.
- The same antenna serves Wi-Fi and Bluetooth: the board's link to the phone
  depends on it too.
- Signal levels depend on the antenna and its placement. When you compare
  surveys, use the same board, antenna and mounting.

## Enclosure

- Use plastic. A metal case blocks the radio.
- Keep the antenna (or the PCB antenna end) away from the battery and any
  metal parts, and close to the case wall.
- Make the status LED visible, through a light pipe or a clear window. It
  shows pairing, surveying and update states.
- Keep the BOOT button reachable, for example with a small hole and a
  plunger. It is needed to pair a new phone and to delete pairings.
- Keep the USB port reachable for charging and for the first install.
  Updates after that go over Bluetooth.
- The board draws little power and needs no ventilation, but avoid sealing a
  charging LiPo in a closed case in direct sun.
- Add a label with the board's short name (`NS-XXXX`, shown by the web
  installer and in the app), so you can tell boards apart.

## Custom boards

Any ESP32-C5, C6, S3 or C3 board can run the firmware:

1. Copy the closest `firmware/boards/<board>.defaults` to a new file.
2. Set `CONFIG_NS_BOARD_CUSTOM=y` and the pins in the "Network Survey board"
   menu: BOOT button, LED type and GPIO, battery sense and GPS.
3. Build it as described in `firmware/README.md`.

A custom board reports the model "Custom board" (`BOARD_MODEL_OTHER`). The app
cannot offer official updates for it, because a release has no image for an
unknown model, so update it with "Install firmware from file" in the app or
reflash it over USB. To add a board to the supported list, see "Adding a
board" in `docs/releasing.md` and `flasher/README.md`.
