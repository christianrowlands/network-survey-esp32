# NS Scout protocol

Version 1.0

This document describes how an NS Scout (the board) talks to the Network Survey
phone app over Bluetooth Low Energy. Anyone can build a compatible board: follow
this document and [`proto/ns_board.proto`](../proto/ns_board.proto), which is the
source of truth for every message. Where this document and the proto file
disagree, the proto file wins and this document is the bug.

The board is a passive sensor. It listens for 802.11 frames and BLE
advertisements, summarizes what it hears, and streams the summaries to the
phone. The phone owns everything else: location, storage, files, streaming and
upload. A board never transmits 802.11 frames: no probe requests, no active
scans, no deauthentication. BLE scanning on the board is passive too.

Key words: "must", "should" and "may" have their usual specification meaning.

## Contents

1. [Advertising](#1-advertising)
2. [GATT service](#2-gatt-service)
3. [Pairing and security](#3-pairing-and-security)
4. [Framing](#4-framing)
5. [Connection flow](#5-connection-flow)
6. [Control commands](#6-control-commands)
7. [Status](#7-status)
8. [Records](#8-records)
9. [Raw frames](#9-raw-frames)
10. [Firmware update (OTA)](#10-firmware-update-ota)
11. [Versioning](#11-versioning)
12. [Size limits](#12-size-limits)
13. [How the app maps records to NS Messaging](#13-how-the-app-maps-records-to-ns-messaging)
14. [Examples](#14-examples)

## 1. Advertising

A board advertises whenever no bonded phone is connected (up to two
connections). While a bonded phone is connected it stops advertising, except
during an explicit pairing window (section 3), when it keeps connectable
advertising so a second phone can connect and bond.

**Advertising data** (28 bytes, fits a legacy advertisement):

| AD type | Content |
|---|---|
| `0x01` Flags | `0x06` (LE General Discoverable, BR/EDR not supported) |
| `0x07` Complete List of 128-bit Service UUIDs | The NS Board Service UUID (section 2) |
| `0xFF` Manufacturer Specific Data | 5 bytes, layout below |

**Manufacturer specific data:**

| Offset | Size | Field | Meaning |
|---|---|---|---|
| 0 | 2 | Company ID | Little-endian. `0xFFFF` (the Bluetooth SIG value reserved for testing) until a registered ID is assigned. |
| 2 | 1 | Protocol version | Protocol major version (currently 1). |
| 3 | 1 | Flags | Bit 0: pairable (no bonds yet, or an explicit pairing window is open). Bit 1: has at least one bond. Bit 2: has GPS. Bit 3: 5 GHz capable. Bits 4 to 7: reserved, send 0, ignore on receive. |
| 4 | 1 | Board model | A `BoardModel` value. |

The app finds boards by the service UUID, not by the company ID, so a later
change of company ID is not a breaking change.

**Scan response:** Complete Local Name (`0x09`) `NS-XXXX`, where `XXXX` is the
last two bytes of the board's BLE identity address in uppercase hex. For the
address `02:00:5E:00:AB:CD` the name is `NS-ABCD`. `XXXX` is the board's short
ID: it is printed on stickers, shown by the web flasher, and repeated in
`BoardInfo.short_id`.

## 2. GATT service

All UUIDs share one random base. Only the second 16-bit group changes.

| Name | UUID | Properties | Security | Content |
|---|---|---|---|---|
| NS Board Service | `72AA0001-0D8E-4D18-8877-63C24ABE28D2` | | | Primary service |
| Info | `72AA0002-0D8E-4D18-8877-63C24ABE28D2` | Read | None | `BoardInfo`, not framed |
| Control | `72AA0003-0D8E-4D18-8877-63C24ABE28D2` | Write (with response), Indicate | Encrypted, bonded | Writes: one `ControlRequest`, not framed. Indications: framed `ControlResponse` |
| Status | `72AA0004-0D8E-4D18-8877-63C24ABE28D2` | Notify | Encrypted, bonded | Framed `BoardStatus` |
| Records | `72AA0005-0D8E-4D18-8877-63C24ABE28D2` | Notify | Encrypted, bonded | Framed `RecordBatch` |
| Raw | `72AA0006-0D8E-4D18-8877-63C24ABE28D2` | Notify | Encrypted, bonded | Framed `RawFrame` |
| OTA | `72AA0007-0D8E-4D18-8877-63C24ABE28D2` | Write without response, Notify | Encrypted, bonded | Writes: image chunks (section 10). Notifications: framed `OtaAck` |

"Encrypted, bonded" means the board must reject reads, writes and Client
Characteristic Configuration Descriptor (CCCD) writes unless the link is
encrypted with keys from a stored bond. Reject with Insufficient Encryption
(`0x0F`) or Insufficient Authentication (`0x05`) on an unencrypted link, which
makes iOS start pairing, and with Insufficient Authorization (`0x08`) when the
link is encrypted but the peer is not bonded. Never send notifications or indications on a link that does not
meet this rule.

Info is readable before pairing so the app can show the board's version and
model, and can tell whether it needs a firmware update, before the user pairs.

**Link parameters.** On connect the board should request the 2M PHY, Data
Length Extension (251-byte PDUs) and an ATT MTU of `BoardInfo.preferred_mtu`
(517 recommended). The phone decides. Boards must work at any negotiated ATT
MTU of 185 or more. The usable notification payload is `ATT_MTU - 3` bytes.

The board also exposes the standard Generic Attribute service (with Service
Changed) and the Device Information service. The app does not depend on them.

## 3. Pairing and security

**Pairing method:** LE Secure Connections, Just Works (IO capability
NoInputNoOutput), bonding on, distributing encryption and identity keys in
both directions. iOS shows its system "Pair" prompt.

**Pairing mode:** a board accepts a new pairing only while it is pairable.
Otherwise it rejects pairing requests with SMP Pairing Failed (reason 0x07
Command Not Supported, or 0x08 Unspecified Reason when the bond store is full;
not 0x05) and never lets a stranger replace a stored bond. A bonded phone
that reconnects re-encrypts with its stored keys and needs no pairing mode.

- A board with no bonds is always pairable, with no timeout.
- A board with a bond opens an explicit pairing window when the BOOT button is
  held for about 3 seconds (the LED blinks fast) or when its bonded phone
  sends `EnterPairingMode`. The window ends when a new bond is created or
  after 60 seconds.
- During an explicit window the board keeps connectable advertising (pairing
  flag set) even while its bonded phone is connected, and accepts a second
  connection. When the window times out while a phone without a bond is still
  connected (it may be pairing), it is extended once by 30 seconds. When it
  ends, unbonded connections are dropped.
- Connections that are not encrypted with a stored bond cannot hold the board:
  while no bonded phone is connected the board keeps advertising and accepts
  up to two connections; when a bonded phone encrypts, the others are dropped
  (outside a window); and an unbonded connection is dropped after 30 seconds
  (90 seconds while the board is pairable, to leave time for the pairing
  prompt).
- The board stores bonds in non-volatile memory. The default maximum is one
  bond (a build option may allow more). A new bond beyond the maximum replaces
  the oldest, and the board disconnects the phone whose bond was replaced.
- Pressing BOOT within 5 seconds after power-up and holding it for about 10
  seconds wipes all bonds (the LED flashes white). Holding BOOT during the
  power-up itself starts the chip's download mode instead.

**Stale bonds on the phone:** when a board has wiped its bonds (button hold, or
`ForgetBonds`), iOS still holds its half of the bond, and reconnecting fails
with `CBError.peerRemovedPairingInformation` (code 14). The app should tell the
user to remove the board in Settings > Bluetooth (Forget This Device) and pair
again.

**Threat model in short:**

- Bonding and encrypted characteristics stop a bystander from reading data,
  controlling the board or pushing firmware. The pairing window limits when a
  new phone can bond.
- Info is readable before pairing. It includes `bt_address`, which is no secret:
  the board sends its address in every advertisement.
- Firmware authenticity is checked by the app, not the board. Official images
  are signed with Ed25519 and the app verifies the signature before sending an
  image. The board's SHA-256 check (section 10) only guards against corruption
  in transfer: the board installs any image with a matching hash and a valid
  header for its chip. A bonded phone is therefore trusted to install any
  firmware. That is the accepted model; secure boot on sold units is a possible
  future addition.

## 4. Framing

### Notifications and indications

Every notification and indication (Control, Status, Records, Raw, OTA) carries
one frame:

```
+----------------+----------+-----------+----------+-----------+-----
| sequence (u16, | length 1 | message 1 | length 2 | message 2 | ...
| little-endian) | (varint) |           | (varint) |           |
+----------------+----------+-----------+----------+-----------+-----
```

- **Sequence number:** counts frames per characteristic. It starts at 0 when the
  phone enables notifications or indications on that characteristic (CCCD
  write), goes up by one for every frame the board tries to send, and wraps
  from 65535 to 0. A frame the board gives up on (for example, the BLE stack
  refuses it) still uses its number, so the phone sees the loss as a gap. A
  frame the board keeps and sends again later (the stack was briefly out of
  buffers) keeps its number, since nothing was lost.
- **Messages:** one or more protobuf messages, each preceded by its length as a
  base-128 varint (the standard protobuf length-delimited form). The message
  type is fixed per characteristic (table in section 2).
- A message never spans frames. The largest message a frame can hold is
  `ATT_MTU - 3 - 2 - (size of its length varint)` bytes.
- The board packs as many messages into a frame as fit. It should not hold a
  partly filled frame for more than about 100 ms waiting for more messages.
- Every record must fit in a frame on its own (see "Fitting records" below).
  Other messages are always small enough at an MTU of 185 or more.

**Gap detection on the phone:** if the phone received sequence `a` and then
`b`, the number of lost frames is `(b - a - 1) mod 65536`. A result of 32768 or
more means the board restarted its counter (or sent a duplicate) and is not
counted as loss.

### Fitting records

A `Record` sent alone in a Records frame may use `ATT_MTU - 12` bytes: 3 for
the ATT header, 2 for the sequence, 2 for the `RecordBatch` length, 2 for
`time_synced`, 1 for the records tag and 2 for the `Record` length. At an MTU
of 185 that is 173 bytes.

Typical records are far smaller. Only an `ApObservation` with long rate and
suite lists can be larger. In that case the board drops optional detail in this
order, checking the size after each step, and sets `detail_trimmed`:

1. `extended_supported_rates`
2. `station_count` and `channel_utilization` (BSS Load)
3. `pairwise_ciphers` and `akm_suites` beyond the first two entries each
4. `supported_rates`
5. `pairwise_ciphers` and `akm_suites` beyond the first entry each

The board never drops identity fields (BSSID, SSID, channel) or the `security`
summary. The largest `ApObservation` the size limits allow fits at an MTU of
185 after these steps. Only below that MTU can a record still be too large; the
board then drops it and counts it in `BoardStatus.dropped_records`. The
reference firmware implements this in
`firmware/components/ns_proto/ns_proto_fit.c`, and a host test checks the
worst case.

### Writes

Writes are not framed:

- Control: the whole write is one `ControlRequest` (up to 512 bytes; iOS uses a
  long write when it exceeds the MTU).
- OTA: a 4-byte little-endian offset followed by image bytes (section 10).

### Buffering

The board keeps a bounded ring buffer of encoded records. When it is full the
board drops the oldest records and adds them to `BoardStatus.dropped_records`.
Capture never waits for BLE. Status and Control traffic take priority over
Records, and Records take priority over Raw.

## 5. Connection flow

1. Scan for the NS Board Service UUID. Read the manufacturer data to show the
   short ID, pairing state and model.
2. Connect. Read Info (no pairing needed). If `protocol_major` differs from the
   app's major, the app shows that the board needs a firmware update.
3. Subscribe to Control indications. On the first connection this triggers
   pairing (section 3). Then subscribe to Status, Records, OTA and, when raw
   capture is wanted, Raw.
4. Send `Hello`, then `SetTime`, then `Configure`, then `Start`.
5. Receive Status about once a second and Records continuously.
6. Send `Stop` to end the survey.

Until the board has answered a compatible `Hello` on this connection it accepts
only the commands that never change between major versions (`Hello`, `Ping`,
`ForgetBonds`, `OtaBegin`, `OtaEnd`, `OtaAbort`) and answers anything else with
`RESULT_CODE_INVALID_STATE`.

**Disconnects:** a board that loses its phone while surveying keeps surveying
with the same configuration and keeps filling the ring buffer, so a short
dropout loses little if the ring holds: only the few frames already handed to
the BLE stack when the link dropped are lost, and since sequence numbers
restart on the new subscription the phone cannot count them. When the phone
reconnects it repeats step 3 and 4; `Start` on a surveying board is a no-op
and buffered records then flow. A board stops surveying if no bonded phone
reconnects within 5 minutes; it then returns to idle (it does not restart).

## 6. Control commands

The phone writes a `ControlRequest` with a `request_id` of its choice. The ATT
write response only confirms delivery. The result arrives as an indication on
Control: a `ControlResponse` with the same `request_id`.

- The phone should keep one request outstanding at a time and wait for its
  response (5 seconds is a reasonable timeout; allow 30 seconds for `OtaEnd`).
- The board handles requests in order and answers every one.
- A request with no command set, or with a command the board does not know
  (from a newer app), gets `RESULT_CODE_UNSUPPORTED`.
- A write the board cannot decode (malformed, or over a limit in section 12,
  such as an `app_version` longer than 31 bytes or more than 64 channels) gets
  `RESULT_CODE_INVALID_ARGUMENT`. The board echoes `request_id` when it can
  read field 1, else it sends 0. The app must keep requests within the limits,
  for example by cutting `app_version` to 31 bytes.

### ControlRequest

| # | Field | Type | Meaning |
|---|---|---|---|
| 1 | `request_id` | uint32 | Chosen by the phone and echoed in the response. |
| 2 to 12 | `command` | oneof | Exactly one of the commands below. |

### ControlResponse

| # | Field | Type | Meaning |
|---|---|---|---|
| 1 | `request_id` | uint32 | Copied from the request. |
| 2 | `result` | ResultCode | Outcome. `RESULT_CODE_UNSPECIFIED` (0) is never sent; treat it as an error. |
| 3 | `message` | string | Optional detail for logs (up to 63 bytes). Not for display. |
| 4 to 7 | `payload` | oneof | `HelloResult`, `ConfigureResult`, `OtaBeginResult` or `PingResult`, for the commands that return data. |

**Result codes:** `OK`; `ERROR` (unexpected failure); `INVALID_ARGUMENT`;
`INVALID_STATE` (not allowed now); `UNSUPPORTED`; `INCOMPATIBLE` (Hello with a
different major); `OTA_VERIFY_FAILED`; `BUSY`. All carry the `RESULT_CODE_`
prefix in the proto.

### Hello (field 2)

The first command on every connection. It exchanges protocol versions.

| # | Field | Type | Meaning |
|---|---|---|---|
| 1 | `protocol_major` | uint32 | App's protocol major version. |
| 2 | `protocol_minor` | uint32 | App's protocol minor version. |
| 3 | `app_version` | string | App version, for board logs (up to 31 bytes). |

Response: `RESULT_CODE_OK` when the majors match, else
`RESULT_CODE_INCOMPATIBLE`. Both carry `HelloResult`:

| # | Field | Type | Meaning |
|---|---|---|---|
| 1 | `protocol_major` | uint32 | Board's protocol major version. |
| 2 | `protocol_minor` | uint32 | Board's protocol minor version. |
| 3 | `compatible` | bool | Majors match. |
| 4 | `firmware_version` | string | Board firmware version, semantic versioning. |
| 5 | `ota_image_confirmed` | bool | True on the first Hello after a firmware update: the new image is confirmed (section 10). |

When incompatible, the app blocks surveying and offers a firmware update.

### SetTime (field 3)

| # | Field | Type | Meaning |
|---|---|---|---|
| 1 | `unix_time_ms` | uint64 | Phone time, Unix epoch milliseconds. |

The board stores the offset between this time and its monotonic clock. From
then on every `board_time_ms` is Unix epoch milliseconds and `time_synced` is
true. Send it after every Hello. Response: `OK`. A time before
1000000000000 (September 2001) or after 4102444800000 (2100-01-01) is
answered with `INVALID_ARGUMENT` and leaves the board's clock unchanged.

The write takes about one connection interval to arrive, so board time trails
phone time by tens of milliseconds. The app may add half of a measured `Ping`
round trip to `unix_time_ms`; record times are no more accurate than this.

### Configure (field 4)

Replaces the whole capture configuration. Allowed while idle or surveying;
while surveying it takes effect at the next channel dwell boundary.

| # | Field | Type | Meaning |
|---|---|---|---|
| 1 | `wifi_ap` | bool | Produce `ApObservation`. |
| 2 | `wifi_client` | bool | Produce `ClientObservation`. |
| 3 | `wifi_probes` | bool | Produce `ProbeObservation`. |
| 4 | `ble` | bool | Produce `BleObservation`. In the Wi-Fi roles the board interleaves short BLE scan windows with Wi-Fi reception (the reference firmware listens for BLE 20 ms of every 100 ms). |
| 5 | `raw_frames` | bool | Stream `RawFrame` on the Raw characteristic. |
| 6 | `role` | Role | What the board does (below). |
| 7 | `channels` | repeated uint32 | Channels to hop. Empty means every supported channel for the role. Up to 64. |
| 8 | `dwell_24_ms` | uint32 | Base dwell per 2.4 GHz channel. 0 means the board default (250 ms). |
| 9 | `dwell_5_ms` | uint32 | Base dwell per 5 GHz channel. 0 means the board default (150 ms). |
| 10 | `raw_snaplen` | uint32 | Maximum captured bytes per raw frame. 0 means as many as fit (section 9). |
| 11 | `raw_max_frames_per_sec` | uint32 | Raw frame rate limit. 0 means the board default (50). |
| 12 | `raw_type_mask` | uint32 | Raw frame types: bit 0 management, bit 1 control, bit 2 data. 0 means management only. |

**Roles:**

| Role | Behavior |
|---|---|
| `ROLE_AUTO` (0) | Hop `channels`. If `ble` is set, interleave BLE scan windows. |
| `ROLE_BAND_24` | Hop the 2.4 GHz channels in `channels` (all 2.4 GHz channels if none). |
| `ROLE_BAND_5` | Hop the 5 GHz channels in `channels` (all 5 GHz channels if none). |
| `ROLE_FIXED_CHANNEL` | Stay on `channels[0]`. Needs at least one channel. |
| `ROLE_BLE_SNIFFER` | Continuous passive BLE scan; no Wi-Fi reception. Wi-Fi flags and raw frames are ignored. |

The board may lengthen the dwell on busy channels (up to twice the base
dwell). Channels the board cannot receive on are dropped. If no channel is
left for a Wi-Fi role the board answers `INVALID_ARGUMENT`.

Response: `OK` with `ConfigureResult`:

| # | Field | Type | Meaning |
|---|---|---|---|
| 1 | `channels` | repeated uint32 | Channels the board will hop, after dropping unsupported ones. |
| 2 | `role` | Role | Role in effect. |
| 3 | `raw_snaplen` | uint32 | Effective raw snaplen at the current MTU. |

Until the first Configure a board uses: `wifi_ap` and `wifi_client` on,
everything else off, `ROLE_AUTO`, all supported channels, default dwells.

### Start (field 5) and Stop (field 6)

No fields. `Start` begins capture with the current configuration. `Stop`
flushes the current dwell's observations as a last batch and stops. Both are
idempotent and answer `OK`. `Start` during an OTA transfer answers
`INVALID_STATE`.

### EnterPairingMode (field 7)

No fields. Opens the 60-second explicit pairing window (section 3) so the user
can hand the board to another phone. Answers `OK`. The current connection
stays up, and the board resumes connectable advertising with the pairing flag
set, so the new phone can find the board, connect and bond. The window ends
when the new phone bonds or after 60 seconds. With the default single bond, the
new bond replaces the current phone's bond and the board disconnects the
current phone.

### ForgetBonds (field 8)

No fields. The board answers `OK`, deletes every bond, disconnects, and is then
pairable because it has no bonds.

### OtaBegin, OtaEnd, OtaAbort (fields 9 to 11)

See section 10.

### Ping (field 12)

| # | Field | Type | Meaning |
|---|---|---|---|
| 1 | `nonce` | uint32 | Any value; echoed back. |

Response: `OK` with `PingResult`:

| # | Field | Type | Meaning |
|---|---|---|---|
| 1 | `nonce` | uint32 | Copied from the request. |
| 2 | `board_time_ms` | uint64 | Board time when it answered. |
| 3 | `time_synced` | bool | Time base of `board_time_ms`. |

## 7. Status

The board notifies `BoardStatus` on the Status characteristic about once a
second and right away when `state`, `role`, `pairing_mode` or `gps_state`
changes.

| # | Field | Type | Meaning |
|---|---|---|---|
| 1 | `board_time_ms` | uint64 | Board time (section 8, "Time"). |
| 2 | `time_synced` | bool | Time base of `board_time_ms`. |
| 3 | `state` | BoardState | `IDLE`, `SURVEYING`, `OTA` or `ERROR`. |
| 4 | `role` | Role | Role in effect. |
| 5 | `current_channel` | uint32 | Wi-Fi channel being received on, 0 when none (idle, or in `ROLE_BLE_SNIFFER`). |
| 6 | `records_per_sec` | uint32 | Records produced over the last second. |
| 7 | `ring_fill_percent` | uint32 | Records ring buffer fill, 0 to 100. |
| 8 | `dropped_records` | uint32 | Records dropped since boot (ring full, or too large for the MTU). |
| 9 | `dropped_raw_frames` | uint32 | Raw frames dropped since boot (rate limit or buffer). |
| 10 | `dropped_rx_frames` | uint32 | 802.11 frames lost before parsing since boot (receive queue or aggregation table full), plus BLE advertising reports lost the same way. |
| 11 | `rx_frames_per_sec` | uint32 | 802.11 frames received over the last second. |
| 12 | `uptime_ms` | uint64 | Time since boot. |
| 13 | `free_heap_bytes` | uint32 | Free heap now. |
| 14 | `min_free_heap_bytes` | uint32 | Lowest free heap since boot. |
| 15 | `gps_state` | GpsState | `NOT_PRESENT`, `NO_FIX` or `FIX`. |
| 16 | `battery_mv` | optional uint32 | Battery voltage, when the board can measure it. |
| 17 | `pairing_mode` | bool | Pairable: no bonds yet, or an explicit pairing window is open. |
| 18 | `bond_count` | uint32 | Stored bonds. |
| 19 | `mtu` | uint32 | Negotiated ATT MTU of this connection. |

Drop counters only grow (until reboot). The app works out new drops from the
difference between two status messages.

## 8. Records

The Records characteristic carries `RecordBatch` messages.

| # | Field | Type | Meaning |
|---|---|---|---|
| 1 | `time_synced` | bool | True: every `board_time_ms` in this batch is Unix epoch ms. False: milliseconds since boot. |
| 2 | `records` | repeated Record | Records in the order the board produced them. Up to 12. |

`Record` is a oneof: `ap` (1), `client` (2), `probe` (3), `ble` (4), `gps` (5).

**Building batches.** The board should encode record by record: measure each
`Record` (with nanopb, `pb_get_encoded_size`), trim it if needed (section 4),
and add records to the batch until the next one would overflow the frame. A
static encode buffer avoids holding a full `RecordBatch` struct on a task
stack.

**Time.** Capture stamps records on the board's monotonic clock. Once `SetTime`
has arrived, the board converts each record to Unix epoch milliseconds as it
queues it. Records queued before that keep milliseconds since boot. A batch
never mixes the two, so all records in one batch share the time base in
`time_synced`.

**Aggregation.** Wi-Fi observations are summaries over one channel dwell: the
board collects frames while it listens on a channel and emits one observation
per key when it leaves the channel. BLE observations are summaries over one
report window (1 second in the reference firmware, ended early when its table
of advertisers is full). "Last" fields come from the most recent frame or
advertisement.
Static details (SSID, rates, suites) repeat in every dwell; in very dense areas
watch the drop counters. A later minor version may send such details once per
BSSID.

**Common conventions.** MAC addresses are 6 bytes in transmission order. RSSI,
noise floor and TX power are dBm. Channel numbers are 1 to 14 for 2.4 GHz and
32 to 177 for 5 GHz. `frequency_mhz` is the centre frequency of the 20 MHz
primary channel.

### ApObservation

One per BSSID per dwell, built from beacons and probe responses.

| # | Field | Type | Meaning |
|---|---|---|---|
| 1 | `board_time_ms` | uint64 | Time of the last frame. |
| 2 | `channel` | uint32 | AP's primary channel: from the DS Parameter Set or HT Operation element, else the channel the frame was received on. |
| 3 | `frequency_mhz` | uint32 | Frequency of `channel`. |
| 4 | `band` | Band | Band of `channel`. |
| 5 | `bssid` | bytes(6) | BSSID (address 3). |
| 6 | `ssid` | bytes | Raw SSID octets, up to 32. Not necessarily UTF-8. |
| 7 | `ssid_hidden` | bool | The SSID element was empty or all zero bytes. |
| 8 | `rssi_max` | sint32 | Strongest RSSI in the dwell. |
| 9 | `rssi_last` | sint32 | RSSI of the last frame. |
| 10 | `noise_floor_dbm` | optional sint32 | Receiver noise floor, when the radio reports it. The app uses it for SNR. |
| 11 | `beacon_count` | uint32 | Beacons received in the dwell. |
| 12 | `probe_response_count` | uint32 | Probe responses received in the dwell. |
| 13 | `beacon_interval_tu` | optional uint32 | Beacon Interval field, in time units (1.024 ms). |
| 14 | `capability_info` | uint32 | Raw 16-bit Capability Information field (ESS, IBSS, Privacy and so on). |
| 15 | `security` | SecurityType | Summary, derived as described below. |
| 16 | `group_cipher` | uint32 | Group cipher suite selector, 0 if none. |
| 17 | `pairwise_ciphers` | repeated uint32 | Pairwise cipher suite selectors. Up to 8. |
| 18 | `akm_suites` | repeated uint32 | AKM suite selectors. Up to 8. |
| 19 | `phy_flags` | uint32 | `PhyFlag` bits: HT (1), VHT (2), HE (4), EHT (8) capability elements present. |
| 20 | `channel_width` | ChannelWidth | Operating width from the HT, VHT, HE or EHT Operation elements, `UNKNOWN` if none. |
| 21 | `wps` | bool | WPS vendor element present. |
| 22 | `passpoint` | bool | Hotspot 2.0 Indication element present. |
| 23 | `supported_rates` | bytes | Raw Supported Rates octets. Each is a rate in 500 kbps units; bit 7 marks a basic rate. Up to 16. |
| 24 | `extended_supported_rates` | bytes | Raw Extended Supported Rates octets, same encoding. Up to 32. |
| 25 | `station_count` | optional uint32 | Station Count from the BSS Load element. |
| 26 | `channel_utilization` | optional uint32 | Channel Utilization from the BSS Load element, 0 to 255. |
| 27 | `detail_trimmed` | bool | The board dropped optional detail so the record fits the MTU (section 4). Lists and rates may be incomplete. |

**Suite selectors** (fields 16 to 18) are the 4-byte selector from the RSN or
WPA element as a number: `(OUI << 8) | type`. `0x000FAC04` is RSN CCMP-128,
`0x000FAC08` is RSN SAE, `0x0050F202` is WPA TKIP. An AP with both RSN and WPA
elements lists the suites of both; the OUI tells them apart. The board passes
selectors through without interpreting them, so new suites need no protocol
change.

**Deriving `security`** (the board applies these in order):

1. No RSN and no WPA element: `WEP` if the Privacy capability bit is set, else
   `OPEN`.
2. WPA element only: `WPA`.
3. RSN element, where "SAE" means RSN AKM types 8, 9, 24 or 25 (or the
   192-bit Suite B types 12 and 13), "OWE" means type 18, and "legacy" means
   types 1 to 6:
   - only OWE: `OWE`
   - SAE and legacy: `WPA2_WPA3`
   - SAE: `WPA3`
   - otherwise, with a WPA element as well: `WPA_WPA2`
   - otherwise: `WPA2`

### ClientObservation

One per client (non-AP station) MAC per dwell. Only frames the client
transmitted count (client MAC in address 2), so RSSI describes the client, not
the AP.

| # | Field | Type | Meaning |
|---|---|---|---|
| 1 | `board_time_ms` | uint64 | Time of the last frame. |
| 2 | `channel` | uint32 | Channel the frames were received on. |
| 3 | `frequency_mhz` | uint32 | Frequency of `channel`. |
| 4 | `band` | Band | Band of `channel`. |
| 5 | `client_mac` | bytes(6) | Client address. |
| 6 | `associated_bssid` | bytes(0 or 6) | BSSID the client talks to, from ToDS/FromDS addressing of data frames or the target of an association, reassociation or authentication request. Empty if unknown. |
| 7 | `rssi_max` | sint32 | Strongest RSSI in the dwell. |
| 8 | `rssi_last` | sint32 | RSSI of the last frame. |
| 9 | `noise_floor_dbm` | optional sint32 | Receiver noise floor, when known. |
| 10 | `frame_count` | uint32 | Frames from this client in the dwell. |
| 11 | `source_flags` | uint32 | `ClientSource` bits for the frame types seen: data (1), null data (2), probe request (4), association request (8), reassociation request (16), authentication (32), action (64). |
| 12 | `locally_administered` | bool | Bit `0x02` of the first address byte is set, which usually means a randomized MAC. |

### ProbeObservation

One per (client MAC, SSID) per dwell.

| # | Field | Type | Meaning |
|---|---|---|---|
| 1 | `board_time_ms` | uint64 | Time of the last request. |
| 2 | `channel` | uint32 | Channel the requests were received on. |
| 3 | `frequency_mhz` | uint32 | Frequency of `channel`. |
| 4 | `band` | Band | Band of `channel`. |
| 5 | `client_mac` | bytes(6) | Source address (address 2). |
| 6 | `ssid` | bytes | Requested SSID octets. Empty means a wildcard request. |
| 7 | `rssi_max` | sint32 | Strongest RSSI in the dwell. |
| 8 | `rssi_last` | sint32 | RSSI of the last request. |
| 9 | `noise_floor_dbm` | optional sint32 | Receiver noise floor, when known. |
| 10 | `count` | uint32 | Requests in the dwell. |
| 11 | `locally_administered` | bool | As in ClientObservation. |
| 12 | `destination_address` | bytes(0 or 6) | Address 1 of the last request: broadcast or a directed AP. |
| 13 | `bssid` | bytes(0 or 6) | Address 3 of the last request. |
| 14 | `phy_flags` | uint32 | `PhyFlag` bits from the request's capability elements. |

### BleObservation

One per advertiser address per report window. The board scans passively, so it
never sends scan requests and receives no scan responses. It uses extended
scanning, so BLE 5 extended advertisements on the 1M PHY are included
(`adv_type` `EXTENDED`); one sent in several parts counts once, and `adv_data`
holds what fits from its first part. Local name, company ID and TX power come
from the latest advertisement in the window that carried them, so a device
that alternates between advertisements still reports its name.

| # | Field | Type | Meaning |
|---|---|---|---|
| 1 | `board_time_ms` | uint64 | Time of the last advertisement. |
| 2 | `channel` | uint32 | Advertising channel (37, 38 or 39) if the radio reports it, else 0. |
| 3 | `address` | bytes(6) | Advertiser address, in the order it is normally printed. |
| 4 | `address_type` | BleAddressType | `PUBLIC`, `RANDOM` or `ANONYMOUS`. For random addresses the top two bits of the first byte give the subtype (11 static, 01 resolvable, 00 non-resolvable). |
| 5 | `rssi_max` | sint32 | Strongest RSSI in the window. |
| 6 | `rssi_last` | sint32 | RSSI of the last advertisement. |
| 7 | `count` | uint32 | Advertisements received in the window. |
| 8 | `adv_type` | BleAdvType | PDU type of the last advertisement. |
| 9 | `adv_data` | bytes | Raw AD structures of the last advertisement, up to 31 bytes. |
| 10 | `adv_data_truncated` | bool | The advertisement (extended advertising) was longer than 31 bytes. The board cut `adv_data` at the last whole AD structure that fits. |
| 11 | `local_name` | bytes | Raw octets of the Complete or Shortened Local Name, if present (up to 31). Usually UTF-8, but not guaranteed: the app decodes leniently. |
| 12 | `company_id` | optional uint32 | Company ID of the first Manufacturer Specific Data structure, if present. |
| 13 | `tx_power` | optional sint32 | TX Power Level, if present. |

Service UUIDs, the full manufacturer data and the Flags structure are in
`adv_data`; the app parses them.

### GpsFix

Sent while surveying when the board has a GPS receiver with a fix, about once
a second. The reference firmware reads NMEA 0183 from a UART and pairs each GGA
sentence (position, quality, satellites, HDOP, altitude) with the RMC sentence
of the same second (date, speed, course); without a matching RMC the fix goes
out without date, speed and course. Sentences with a missing or wrong checksum
are ignored.

| # | Field | Type | Meaning |
|---|---|---|---|
| 1 | `board_time_ms` | uint64 | When the board received the fix. |
| 2 | `latitude` | double | Degrees, WGS84. |
| 3 | `longitude` | double | Degrees, WGS84. |
| 4 | `altitude_m` | optional float | Metres above mean sea level. |
| 5 | `hdop` | optional float | Horizontal dilution of precision. |
| 6 | `satellites` | uint32 | Satellites used. |
| 7 | `fix_time_ms` | uint64 | UTC time of the fix, Unix epoch ms. 0 if the receiver has not reported the date. |
| 8 | `fix_quality` | uint32 | NMEA GGA quality indicator: 0 invalid, 1 GPS, 2 DGPS, 4 RTK fixed, 5 RTK float, 6 estimated. |
| 9 | `speed_mps` | optional float | Ground speed, m/s. |
| 10 | `course_deg` | optional float | Course over ground, degrees true. |

## 9. Raw frames

When `Configure.raw_frames` is set and the phone has subscribed to Raw, the
board sends received 802.11 frames as `RawFrame` messages, for pcap files on
the phone. Raw capture is rate limited and lower priority than Records.

| # | Field | Type | Meaning |
|---|---|---|---|
| 1 | `board_time_us` | uint64 | Receive time in microseconds, same time base as `board_time_ms`. |
| 2 | `time_synced` | bool | Time base of `board_time_us`. |
| 3 | `channel` | uint32 | Receive channel. |
| 4 | `frequency_mhz` | uint32 | Frequency of `channel`. |
| 5 | `band` | Band | Band of `channel`. |
| 6 | `rssi` | sint32 | Frame RSSI. |
| 7 | `noise_floor_dbm` | optional sint32 | Noise floor, when known. |
| 8 | `phy` | FramePhy | `LEGACY`, `HT`, `VHT` or `HE`. |
| 9 | `rate_500kbps` | uint32 | Legacy data rate in 500 kbps units, 0 for HT and later. |
| 10 | `mcs` | optional uint32 | MCS index for HT and later. |
| 11 | `orig_len` | uint32 | Frame length on air. More than `data`'s length when the frame was cut. |
| 12 | `fcs_included` | bool | `data` ends with the 4-byte FCS. |
| 13 | `data` | bytes | The 802.11 frame from Frame Control onward, up to the snaplen. |

**Snaplen.** The effective snaplen is the smallest of `Configure.raw_snaplen`
(when set), 512, and what fits in one frame at the current MTU. The board
reports it in `ConfigureResult.raw_snaplen`.

**Data frames.** A data frame is never sent with payload: the board keeps at
most its MAC header (including address 4, QoS control and HT control) plus the
8-byte LLC/SNAP header, so `data` shows the ethertype but nothing after it.
Protected frames, A-MSDUs and null frames keep the MAC header only. EAPOL
frames (ethertype `0x888E`) and RSN pre-authentication frames (`0x88C7`) are
never sent at all, so no key handshake or PMKID leaves the board. These frames
are not counted as drops. `orig_len` still gives the length on air.

**Rate limit.** The board sends at most `Configure.raw_max_frames_per_sec`
frames per second and also caps raw bytes per second (12 KB/s of captured
bytes plus about 32 bytes per frame in the reference firmware), so raw capture
leaves room for Records on a link of about 25 KB/s. Both limits allow a burst
of up to one second's budget. Frames over either limit, and frames lost in the
board's buffers, are counted in `BoardStatus.dropped_raw_frames`. Raw frames
are sent only while the board is surveying in a Wi-Fi role and the phone is
subscribed to Raw.

**FCS.** The reference firmware never keeps the FCS: `fcs_included` is false
and `orig_len` is the frame length without it.

**Privacy.** Raw frames carry frame bodies up to the snaplen: SSIDs and vendor
elements of beacons and probes, and the MAC addresses of every device in range.
With the data bit of `raw_type_mask`, data frames add their MAC and LLC/SNAP
headers (addresses and ethertype), never payload and never EAPOL (see "Data
frames"). Raw capture is off by default and covers management frames only
unless `raw_type_mask` asks for control or data frames; keep the snaplen low.
Apps should warn before turning it on and mark files that contain raw frames.

## 10. Firmware update (OTA)

The app verifies the image signature before it starts. The board checks the
SHA-256 and the image format, writes the image to the inactive partition, and
switches to it.

1. **OtaBegin** (Control, field 9). The board stops surveying and enters the
   `OTA` state.

   | # | Field | Type | Meaning |
   |---|---|---|---|
   | 1 | `image_size` | uint32 | Image size in bytes. |
   | 2 | `sha256` | bytes(32) | SHA-256 of the whole image. |

   Response `OK` with `OtaBeginResult`, or `INVALID_ARGUMENT` (too large for
   the partition, or a bad hash length):

   | # | Field | Type | Meaning |
   |---|---|---|---|
   | 1 | `max_chunk_size` | uint32 | Most image bytes per OTA write. Normally `ATT_MTU - 3 - 4`. |
   | 2 | `window_bytes` | uint32 | Most image bytes the phone may send beyond the last acknowledged offset. |
   | 3 | `start_offset` | uint32 | Where to start. Non-zero when the board still holds part of the same image (same size and SHA-256) from an interrupted transfer since it booted. |

   To discard a partial image and start from 0, send `OtaAbort` before
   `OtaBegin`. An `OtaBegin` for a different image (size or SHA-256) also
   discards the partial image and starts at 0.

   A board still running a new image that is not yet confirmed (step 5)
   confirms it on `OtaBegin`: an update cannot start from an unconfirmed
   image, and a board that can take a new image is working.

2. **Chunks.** The phone writes without response to the OTA characteristic:

   ```
   +---------------------------+------------------------------+
   | offset (u32 little-endian)| image bytes (1..max_chunk)   |
   +---------------------------+------------------------------+
   ```

   The board accepts a chunk only when `offset` equals its next expected
   offset and the chunk does not run past `image_size`. A chunk that starts
   below that offset (a resend) is handled so that a lost ack never stalls the
   transfer:

   - one that ends exactly at `next_offset` is answered with an `OK` ack
     carrying `next_offset`;
   - one that runs past `next_offset` (the phone changed its chunk size) is
     trimmed: the board writes the part from `next_offset` on;
   - any other is skipped, and the board answers `OK` with `next_offset` at
     most once a second.

3. **Acks.** The board notifies `OtaAck` on the OTA characteristic:

   | # | Field | Type | Meaning |
   |---|---|---|---|
   | 1 | `next_offset` | uint32 | Bytes received and written, counted from 0 without gaps. |
   | 2 | `status` | OtaAckStatus | See below. |

   - `OK`: sent at least every `window_bytes / 2` bytes and when the last byte
     arrives.
   - `OUT_OF_ORDER`: a chunk started past `next_offset` (one was lost). The
     board drops it and every following chunk until one starts at
     `next_offset`, and sends this ack only once until that happens. The phone
     resends from `next_offset`.
   - `WRITE_FAILED` or `OVERFLOW`: the transfer is aborted; start again with
     OtaBegin.
   - `NOT_ACTIVE`: a chunk arrived without a transfer in progress. Sent
     once; further chunks are dropped quietly. Chunks still in flight after
     an `OtaAbort`, `OVERFLOW` or `WRITE_FAILED` are dropped without it.

   The phone keeps at most `window_bytes` unacknowledged bytes in flight. If no
   ack arrives for 5 seconds it resends from the last acknowledged offset.

4. **OtaEnd** (Control, field 10, no fields). The board checks that it has
   `image_size` bytes, that their SHA-256 matches, and that the image is valid
   for this chip. It answers `OK` and restarts into the new image about one
   second later, or answers `OTA_VERIFY_FAILED` and discards the image.
   `OtaEnd` before the last byte answers `INVALID_STATE`. The answer can take
   several seconds (the board first writes the chunks it still holds, then
   reads the whole image back to check it); wait up to 30 seconds.

5. **Confirm.** The new image starts unconfirmed. Its self-test passes when
   capture and link are initialised, the sniffer task is running and the BLE
   host is up. The image is confirmed by whichever comes first: a compatible
   `Hello` while the self-test passes, or the self-test after 60 seconds of
   uptime. The self-test path keeps a working image when the board is power
   cycled before the phone reconnects. The first Hello after the image is
   confirmed reports `HelloResult.ota_image_confirmed = true`. If the new
   image restarts before it is confirmed (for example it crashes), the
   bootloader returns to the previous image. A new image whose self-test still
   fails 180 seconds after boot returns to the previous image on its own.

   Once confirmed, an image stays: a crash later on (for example only after
   `Start`) does not roll back, and recovery is another update or a USB
   reflash.

   After `OtaEnd` answers `OK`, the app reconnects and compares
   `firmware_version` (in `Info` or `HelloResult`) with the version it
   installed. A different version means the update did not take (the board
   returned to the previous image), even if `OtaEnd` succeeded. Do not rely on
   `ota_image_confirmed` alone: it is kept in memory only, so a board power
   cycled after confirming and before the next Hello does not report it.

**OtaAbort** (Control, field 11, no fields) discards a transfer and returns to
idle. It answers `OK` even when no transfer is in progress.

A disconnect during a transfer keeps the partial image, and the `OTA` state,
until the board restarts or receives OtaAbort, so the app can resume with the
same OtaBegin. A transfer with no chunk and no OtaBegin for 5 minutes is
discarded and the board returns to idle.

**Authenticity and downgrades.** The board's SHA-256 check only proves the
image arrived intact (section 3, threat model). Authenticity is the app's job:
it verifies the Ed25519 signature over the image and over the signed release
manifest, which names the firmware version, chip and board model. The app
refuses an image built for another chip or model, and refuses to install an
older version than the board runs unless the user explicitly asks (for example
to recover from a bad release). Unsigned do-it-yourself images are installed
only after an explicit warning. After the restart the app checks the version as
step 5 describes; only when the version cannot tell (the same version
reinstalled, or an image without one) does it wait for
`HelloResult.ota_image_confirmed`. A validly signed but
old manifest replayed by a release host is only partly detected (see the
`min_version` field and the limitations in release-manifest.md).

## 11. Versioning

The protocol version is `MAJOR.MINOR`, defined by the `ProtocolVersion` enum in
the proto file. The board reports it in the advertising data (major only),
`BoardInfo` and `HelloResult`; the app sends its own in `Hello`.

- **Minor** changes are additive: new fields, new messages, new enum values, new
  commands, new record kinds. Old and new peers keep working. Receivers ignore
  unknown fields, and treat unknown enum values as the default (0) value or
  ignore them. A board answers
  an unknown command with `RESULT_CODE_UNSUPPORTED`. The app checks the board's
  minor version before it uses a feature added in a later minor.
- **Major** changes are anything else: changing or removing a field, changing a
  field's meaning, type or number, or changing the GATT layout or framing. The
  app and board only work together when their majors match.
- Field and enum numbers are never reused. A removed field becomes `reserved`.
- **Frozen across majors:** everything an app needs to identify, version-check
  and update any board, whatever their versions:
  - the advertised NS Board Service UUID and the manufacturer data layout
    (including the protocol version byte at offset 2)
  - the Info, Control and OTA characteristic UUIDs, and the framing of Control
    and OTA
  - `BoardInfo` fields 1 to 5
  - `ControlRequest` (`request_id` and the oneof numbers of the frozen
    commands) and `ControlResponse` (`request_id`, `result`, `message` and the
    oneof numbers of the frozen payloads)
  - `ResultCode` and `OtaAckStatus` values
  - `Hello`, `HelloResult`, `Ping`, `PingResult`, `ForgetBonds`, `OtaBegin`,
    `OtaBeginResult`, `OtaEnd`, `OtaAbort`, `OtaAck` and the OTA chunk format

## 12. Size limits

Senders must stay within these limits so a board can decode and encode every
message without dynamic memory (they are the nanopb options in
`proto/ns_board.options`). Receivers on the phone accept larger values.

| Field | Limit |
|---|---|
| MAC address fields | 6 bytes (empty allowed where marked 0 or 6) |
| `ApObservation.ssid`, `ProbeObservation.ssid` | 32 bytes |
| `ApObservation.supported_rates` | 16 bytes |
| `ApObservation.extended_supported_rates` | 32 bytes |
| `ApObservation.pairwise_ciphers`, `akm_suites` | 8 entries each |
| `BleObservation.adv_data` | 31 bytes |
| `BleObservation.local_name` | 31 bytes (raw octets) |
| `BoardInfo.firmware_version`, `model_name`, `Hello.app_version`, `HelloResult.firmware_version` | 31 bytes |
| `BoardInfo.short_id` | 7 bytes |
| `BoardInfo.bands` | 4 entries |
| `ControlResponse.message` | 63 bytes |
| `Configure.channels`, `ConfigureResult.channels` | 64 entries |
| `OtaBegin.sha256` | exactly 32 bytes |
| `RecordBatch.records` | 12 entries |
| `RawFrame.data` | 512 bytes |
| `ControlRequest` (whole write) | 512 bytes |

Typical encoded sizes: an `ApObservation` is about 60 to 120 bytes, a
`BleObservation` about 40 to 80, a `ClientObservation` about 40.

## 13. How the app maps records to NS Messaging

This section is informative. It shows why the records carry what they carry.
The app combines each record with the phone's location and survey details to
build NS Messaging API 2.6.0 messages.

| NS Messaging field | Source |
|---|---|
| **WifiBeaconRecord** | from `ApObservation` |
| `bssid`, `sourceAddress` | `bssid` |
| `destinationAddress` | broadcast (`FF:FF:FF:FF:FF:FF`) |
| `ssid` | `ssid` decoded as UTF-8 (empty when hidden) |
| `beaconInterval` | `beacon_interval_tu` |
| `serviceSetType` | `capability_info` ESS and IBSS bits |
| `supportedRates`, `extendedSupportedRates` | rate octets, bit 7 cleared, times 0.5 Mbps |
| `cipherSuites` | `group_cipher` and `pairwise_ciphers` |
| `akmSuites` | `akm_suites` |
| `encryptionType` | `security` |
| `wps`, `passpoint` | `wps`, `passpoint` |
| `bandwidth` | `channel_width` |
| `channel`, `frequencyMhz` | `channel`, `frequency_mhz` |
| `signalStrength`, `snr` | `rssi_max`, and `rssi_max - noise_floor_dbm` |
| `standard` | `phy_flags`, `band` and the rates |
| **WifiProbeRequestRecord** | from `ProbeObservation` |
| `sourceAddress`, `destinationAddress`, `bssid` | `client_mac`, `destination_address`, `bssid` |
| `ssid`, `channel`, `frequencyMhz` | `ssid`, `channel`, `frequency_mhz` |
| `signalStrength`, `snr` | `rssi_max`, `noise_floor_dbm` |
| `standard` | `phy_flags` and `band` |
| **BluetoothRecord** | from `BleObservation` |
| `sourceAddress`, `addressType` | `address`, `address_type` |
| `signalStrength`, `txPower` | `rssi_max`, `tx_power` |
| `otaDeviceName` | `local_name`, decoded leniently as UTF-8 |
| `technology`, `supportedTechnologies` | LE, and the Flags structure in `adv_data` |
| `serviceUuids`, `companyId`, `mfgData` | parsed from `adv_data` |
| `channel` | `channel` |

`ClientObservation` has no NS Messaging 2.6.0 message; the app stores it
locally.

## 14. Examples

Bytes are hex.

**Ping request** (Control write, not framed): `request_id` 5, nonce 1.

```
08 05 62 02 08 01
```

**Ping response** (Control indication, sequence 0): one 17-byte
`ControlResponse` with `request_id` 5, `RESULT_CODE_OK` and a `PingResult`
(nonce 1, `board_time_ms` 1767225600123, `time_synced` true).

```
00 00  11  08 05 10 01 3a 0b 08 01 10 fb d0 ea b6 b7 33 18 01
seq    len ControlResponse
```

**OTA ack** (OTA notification, sequence 258): `next_offset` 4096, status `OK`.

```
02 01  05  08 80 20 10 01
```

**OTA chunk** (OTA write): 4 bytes at offset 4096.

```
00 10 00 00  e9 03 02 20
offset       image bytes
```

The firmware repository has one encoded example of every record type and of
the main control messages in `test/fixtures/proto/`, produced by the nanopb
host test in `test/host/proto_roundtrip/`.
