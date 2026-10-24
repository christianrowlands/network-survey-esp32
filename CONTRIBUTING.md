# Contributing

Thanks for helping with the NS Scout firmware. Bug reports, board
ports, test results on hardware and fixes are all welcome.

## Before you start

- **Bugs:** open an issue with the board, the firmware version (shown in the
  app and in the boot log), what you did, what you expected and what
  happened. A serial log from boot helps a lot.
- **Hardware reports:** results on a board marked unverified in
  [`docs/hardware.md`](docs/hardware.md) are especially useful, including
  "it works".
- **Larger changes** (new record types, protocol changes, new features):
  open an issue first so we can agree on the approach before you write the
  code.
- **Security problems:** do not open a public issue. See
  [`SECURITY.md`](SECURITY.md).

## Ground rules

- **Receive only.** The board must never transmit 802.11 frames: no probe
  requests, no active scans, no association, no deauthentication, no frame
  injection, and no handshake or PMKID capture. BLE scanning stays passive.
  `scripts/check-passive.sh` fails on such code, and pull requests that add
  it will not be accepted.
- **No real captures in the repository.** Test data must be synthetic. Use
  locally administered MAC addresses (for example `02:00:5E:xx:xx:xx`) and
  made-up network names such as `ExampleNet`.
- **Keep it small.** The project is maintained by a small team. Prefer
  simple code and few dependencies. Production source files should stay
  under about 400 lines.
- **Match the surrounding style:** four-space indents, `snake_case`,
  short comments that explain why rather than what.
- **Plain text in docs and comments:** direct sentences, no em dashes
  (`scripts/check-text.sh` checks this).

## Building and testing

See [`firmware/README.md`](firmware/README.md) for the ESP-IDF setup. Before
you open a pull request, run:

```sh
scripts/check-passive.sh
scripts/check-text.sh
make -C test/host
scripts/build-all-targets.sh        # or at least the boards you touched
```

Add or update host tests in `test/host/` for any logic that can run without
ESP-IDF (parsers, state machines, planners). Keep that logic in pure C files
with no ESP-IDF includes so the host tests can cover it.

If you changed behavior on the radio side, say in the pull request which board
you tested on and how.

## Changing the board protocol

`proto/ns_board.proto` is the source of truth for the protocol between board
and app, and the iOS app keeps a copy of it.

1. Change `proto/ns_board.proto`. Prefer additive changes (new fields,
   messages, enum values or commands), which need only a minor version
   bump; anything else is a new major version. Never renumber or reuse a
   field or enum number.
2. Run `scripts/gen-proto.sh` and commit the regenerated nanopb code.
3. Update the fixtures (`make -C test/host/proto_roundtrip fixtures`) when
   the change is deliberate.
4. Update [`docs/protocol.md`](docs/protocol.md), including section 11
   (versioning) when the version changes.
5. Open a matching change in the iOS app repository (its
   `scripts/sync-proto.sh` copies the proto and fixtures).

The GATT table must not change within a protocol major version, because the
board never sends Service Changed.

## Adding a board

Add a `firmware/boards/<board>.defaults` file and, if the board needs new pin
defaults, an entry in `firmware/components/board/Kconfig`. Then follow
"Adding a board" in [`flasher/README.md`](flasher/README.md) and
[`docs/releasing.md`](docs/releasing.md), and add the board to
[`docs/hardware.md`](docs/hardware.md) as unverified until someone has
tested it.

## Pull requests

- One topic per pull request, with a short description of what and why.
- CI must pass (checks, host tests, and a build of every board).
- By contributing you agree that your contribution is licensed under the
  Apache License 2.0, the license of this project.
- Source files carry no per-file license headers; LICENSE and NOTICE cover the
  repository.
