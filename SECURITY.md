# Security policy

## Supported versions

Only the latest firmware release is supported. Fixes go into the next release,
and the iPhone app offers it as an update over Bluetooth. Older releases are
not patched.

## Reporting a vulnerability

If you find a security problem in the firmware, the board protocol or the web
installer, please report it privately:

- Use GitHub's private vulnerability reporting: the **Report a vulnerability**
  button on this repository's **Security** tab
  (`https://github.com/christianrowlands/network-survey-esp32/security/advisories/new`).
- Include a description of the problem, the board and firmware version, steps
  to reproduce, and any logs or captures (with private data removed).
- Do **not** disclose the problem publicly until a fixed release is out.

What to expect:

- Acknowledgment of the report within **96 hours**.
- Updates on the investigation and the plan for a fix.
- A fix in the next available release, and credit in the release notes if
  you want it.

Problems in the iPhone app go to the app repository's security policy.

## Security model

What the board protects, and what it does not.

**Pairing and data.** The board uses Bluetooth LE Secure Connections with
bonding. In the board's GATT service, everything except the read-only Info
characteristic requires an encrypted link with a stored bond: a bystander cannot read survey data,
control the board or send firmware. A new phone can pair only while the board
is pairable: when it has no bonds, or for 60 seconds after its BOOT button is
held for about 3 seconds (or its paired phone asks). Outside that window a
stranger cannot replace the stored bond. Pairing uses Just Works (the board
has no display), so a nearby attacker who pairs during an open window gets
in; keep pairing windows short and in places you trust. The full rules are in
[`docs/protocol.md`](docs/protocol.md) section 3.

**Firmware signing.**

- Official releases are signed with an Ed25519 key. The private key is held
  only as a CI secret and never stored in a repository.
- Each release has a signed manifest and a signature for every image
  ([`docs/release-manifest.md`](docs/release-manifest.md)). The iPhone app
  verifies both against the public keys built into it before it sends an
  image to the board. It refuses a release below the `min_version` of any
  release it has verified, and warns before installing when it has seen a
  newer release.
- The board checks the image's SHA-256 and header, which guards against
  corruption in transfer, but it does not check signatures. A phone that is
  paired with the board can therefore install any firmware, including an
  unsigned file the user chooses with "Install firmware from file" after a
  clear warning. A paired phone is trusted.
- `scripts/keys/dev-signing-key.pub` is a development key for local testing.
  Release builds of the app do not trust it.

**Physical access.** Secure boot and flash encryption are not enabled. Anyone
with the board in hand can reflash it over USB, read its flash (including the
stored bond keys) and run any firmware. Enabling secure boot on sold units is
possible future work. Holding BOOT for about 10 seconds right after power-up
deletes all bonds.

**Radio behavior.** The board is receive-only for Wi-Fi. Anything that makes
it transmit 802.11 frames, or makes its BLE scanning active, is a bug we want
to hear about.

## Out of scope

- Attacks that need physical access to the board, given the model above.
- A user who pairs their own phone and installs their own firmware.
- Denial of service by radio jamming.
