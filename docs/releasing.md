# Releasing firmware

Three GitHub Actions workflows live in `.github/workflows/`:

- `firmware.yml` runs on every branch push and pull request: the passive-only
  check, the text check, the host tests, a check that the generated nanopb
  code matches `proto/ns_board.proto`, and a build of every board in the
  `espressif/idf:v6.0.1` container. Each board's `dist/<board>/` is kept as a
  build artifact.
- `release.yml` runs when a version tag is pushed. It runs `firmware.yml`,
  signs the OTA images, publishes a GitHub Release and deploys the web
  installer to GitHub Pages.
- `installer.yml` runs only by hand. It deploys the web installer from the
  branch it runs on, with the images of a release that is already published,
  so a change to the page goes live without a firmware release.

## One-time setup

1. **Signing key.** Generate an Ed25519 key pair on a trusted machine, offline
   if possible. Keep the private key out of every repository.

   ```sh
   python3 - <<'PY'
   import base64
   from cryptography.hazmat.primitives import serialization
   from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
   key = Ed25519PrivateKey.generate()
   raw = serialization.Encoding.Raw
   print("private:", base64.b64encode(key.private_bytes(raw, serialization.PrivateFormat.Raw,
                                                         serialization.NoEncryption())).decode())
   print("public: ", base64.b64encode(key.public_key().public_bytes(raw, serialization.PublicFormat.Raw)).decode())
   PY
   ```

2. **Secret.** In Settings > Secrets and variables > Actions, add a repository
   secret `NS_FIRMWARE_SIGNING_KEY` holding the private value (the one-line
   base64 form above; a PEM private key also works).
3. **Public key.** Commit the public value as
   `scripts/keys/release-signing-key.pub` (one line of base64; lines starting
   with `#` are comments). The release workflow verifies its own output
   against this file and stops before publishing if the secret does not match.
   The iOS app must embed the same public key to accept official updates.
4. **Pages.** In Settings > Pages, set the source to "GitHub Actions" and the
   custom domain to `install.networksurvey.app`, then tick Enforce HTTPS once
   the certificate is issued. In Settings > Environments > `github-pages`,
   allow deployments from tags matching `v*` for `release.yml`, and keep
   `main` allowed for `installer.yml` (by default only the default branch may
   deploy). In Cloudflare DNS for `networksurvey.app`, the record
   is `CNAME install -> christianrowlands.github.io`, DNS only (grey cloud):
   GitHub cannot issue the certificate through the Cloudflare proxy.
5. **Optional variable.** `NS_FIRMWARE_MIN_VERSION` (Settings > Secrets and
   variables > Actions > Variables) sets `min_version` in the manifest, the
   oldest release the app still installs. Leave it unset for `0.0.0`.

Nothing else is needed: the release step uses the workflow's own
`GITHUB_TOKEN`.

## Making a release

1. Set `PROJECT_VER` in `firmware/CMakeLists.txt` to the new version and
   commit.
2. Tag that commit with the same version and a leading `v`, then push the
   tag:

   ```sh
   git tag v0.2.0
   git push origin v0.2.0
   ```

The workflow refuses a tag that does not match `PROJECT_VER`. A version with a
pre-release part (`v0.2.0-beta.1`) is published as a GitHub pre-release and
does not update the web installer. The app looks for updates in the latest
full release.

## What a release contains

| Asset | Use |
|---|---|
| `firmware-manifest.json`, `firmware-manifest.json.sig` | Signed manifest the app reads (format: `release-manifest.md`). |
| `network-survey-board-<board>-<version>.bin` and `.bin.sig` | Signed OTA app image per board, installed by the app over Bluetooth. |
| `network-survey-board-<board>-<version>-full-flash.bin` | Complete flash image per board for `esptool write-flash 0x0`, an alternative to the web installer. Not for OTA. |

Image URLs in the manifest point at this release's download URLs, so the app
can fetch the manifest through `releases/latest/download/` and still get the
images of the release that manifest belongs to.

## Web installer

For a full release the workflow runs `flasher/stage.sh` on the freshly built
images and deploys the result to GitHub Pages
(`https://install.networksurvey.app/`), replacing the previous
version. See `flasher/README.md` for the page itself.

To publish a change to the page without a firmware release, merge it to
`main`, then in Actions > installer > Run workflow, run it on `main`. It
stages the page with the full flash images of the latest full release (or the
tag you enter) and deploys it. The firmware the page installs and the
firmware the app offers stay the same. `flasher/stage-release.sh` does the
staging and works locally too, with the `gh` CLI signed in.

## Adding a board

Besides the steps in `flasher/README.md`, add the board to `BOARDS` in
`scripts/sign_release.py`. The release workflow stops if a board it built has
no signed image.
