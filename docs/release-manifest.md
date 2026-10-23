# Firmware release manifest

Each firmware release publishes the OTA app images, one per supported board,
and a signed manifest that the app reads to offer updates (protocol.md
section 10). `scripts/sign-release.sh` produces all of it from the output of
`scripts/build-all-targets.sh`.

## Files

| File | Content |
|---|---|
| `firmware-manifest.json` | The manifest below. |
| `firmware-manifest.json.sig` | Ed25519 signature over the exact bytes of `firmware-manifest.json`, base64, one line. |
| `network-survey-board-<board>-<version>.bin` | ESP-IDF app image for OTA (not the merged flash image). |
| `network-survey-board-<board>-<version>.bin.sig` | Ed25519 signature over the image bytes, base64, one line. |

The app downloads the manifest and its `.sig` from the same location (the
manifest URL with `.sig` appended).

## Manifest

```json
{
  "format": 1,
  "name": "Network Survey Board firmware",
  "version": "0.2.0",
  "min_version": "0.1.0",
  "protocol_major": 1,
  "protocol_minor": 0,
  "released": "2026-10-22T12:00:00Z",
  "images": [
    {
      "board": "esp32c5-devkitc1",
      "board_model": "ESP32_C5_DEVKITC_1",
      "chip": "ESP32_C5",
      "file": "network-survey-board-esp32c5-devkitc1-0.2.0.bin",
      "url": "network-survey-board-esp32c5-devkitc1-0.2.0.bin",
      "size": 651360,
      "sha256": "71b6b464...",
      "signature": "B5G3u3rg..."
    }
  ]
}
```

| Field | Meaning |
|---|---|
| `format` | Manifest format, 1. An app ignores a manifest with a format it does not know. |
| `version` | Firmware version, semantic versioning (`PROJECT_VER`). |
| `min_version` | Oldest version still considered safe to install (`--min-version`, default `0.0.0`). Raise it when older releases must never be installed again, for example after a security fix. |
| `protocol_major`, `protocol_minor` | Board protocol version this firmware speaks. |
| `released` | UTC time the manifest was signed (`SOURCE_DATE_EPOCH` when set). |
| `images[].board` | Build name from `build-all-targets.sh`, for people. |
| `images[].board_model` | `BoardModel` enum name without the `BOARD_MODEL_` prefix, as the board reports it in `BoardInfo.board_model`. |
| `images[].chip` | `Chip` enum name without the `CHIP_` prefix, as in `BoardInfo.chip`. |
| `images[].url` | Absolute URL, or a path relative to the manifest URL. |
| `images[].size` | Image size in bytes (`OtaBegin.image_size`), at most 8 MiB. |
| `images[].sha256` | SHA-256 of the image, lowercase hex (`OtaBegin.sha256`). |
| `images[].signature` | Ed25519 signature over the image bytes, base64 (same as the `.bin.sig` file). |

Unknown fields are ignored, so later releases can add some within format 1.

## What the app checks

1. The manifest and image URLs use https, and the downloads stay within the
   size limits (256 KiB for the manifest and signatures, the listed size for
   an image).
2. The manifest signature, against the public keys built into the app.
3. That `version` is not below the highest `min_version` this phone has
   verified, and whether it has verified a higher `version` before (then it
   warns that the release host may be out of date and asks before installing).
4. That an image exists for the board's `board_model` and `chip`. Boards with
   `BOARD_MODEL_OTHER` have no official image and use "Install from file".
5. After download: size, SHA-256, the image signature, and that the image
   header names the board's chip and the manifest's `version`.
6. That `version` is newer than the board's firmware. The same or an older
   version is installed only when the user explicitly asks for it.
7. After the restart: `Hello` reports the expected version; the previous
   version means the board went back to its old image. When the version cannot
   tell (the same version reinstalled, or a file without a version), the app
   keeps saying `Hello` for up to 20 seconds until the board reports
   `ota_image_confirmed`.

The board checks the SHA-256 again before it switches partitions.

"Install from file" takes an app image and, optionally, its `.sig`. A valid
signature from a trusted key marks the file as signed; an invalid one is
refused. An image does not name its board model (only the manifest does), so
for any file the user must confirm the model, and an unsigned file, or one
that is not newer than the board's firmware, needs a further explicit
confirmation.

## Limitations

- **Stale or replayed manifests.** A manifest has no expiry. A release host
  that serves an old but validly signed manifest (a stale cache, or a
  compromised host) can keep boards on an older release or offer a release
  that is newer than the board's but not the newest. `min_version` and the
  app's memory of the highest release it has verified narrow this, but a phone
  that never saw a newer manifest cannot tell.
- **One key, two kinds of data.** The same key signs manifests and images,
  with no context prefix. This is safe because the two cannot be confused: a
  manifest is JSON, an app image starts with the byte `0xE9` and the app
  parses each only in its own place.

## Signing

```sh
NS_SIGNING_KEY_FILE=path/to/key.pem scripts/sign-release.sh --public-key path/to/key.pub
```

CI passes the key in `NS_SIGNING_KEY` (PEM text, or base64 of the raw 32-byte
private key) from a repository secret, and must pass `--public-key` with the
release public key (the script refuses to run without it when `CI` is set), so
a wrong secret is caught before anything is uploaded. `--base-url` writes
absolute image URLs; without it the URLs are relative to the manifest.
`--verify DIR` checks an existing release folder against a public key. The
output folder is emptied of earlier images, signatures and manifests first.

Public key files hold the raw 32-byte key in base64; lines starting with `#`
are comments. `scripts/keys/dev-signing-key.pub` is a development key for
local testing only. Release firmware is signed with a separate key that is
never stored in the repository.

Secure boot (the chip verifying signatures itself) is not used yet; it is a
possible future option for sold units.
