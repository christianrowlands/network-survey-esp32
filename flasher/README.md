# Web installer

A static page that installs the board firmware from the browser with
[ESP Web Tools](https://esphome.github.io/esp-web-tools/) (Web Serial, so
Chrome or Edge on a desktop). It has one Install button per board, because an
ESP Web Tools manifest holds one build per chip family and two boards share the
ESP32-C5. After an install, the page can reset the board over the same serial
port and read its `NS-XXXX` name from the boot log. Later updates go over
Bluetooth from the iOS app, not through this page.

## Files

| File | Purpose |
|---|---|
| `index.html` | The page: warnings, steps, one card per board. |
| `style.css` | Light and dark styles (follows the system setting). |
| `app.js` | Hides boards whose firmware is missing, shows the version, reads the board name. |
| `img/` | Network Survey icon and lockups. |
| `stage.sh` | Assembles the page and the firmware into a folder ready to host. |

The page loads `esp-web-tools` from unpkg.com at an exact version with a
Subresource Integrity hash. It has no analytics, cookies or other third-party
requests.

## Layout when hosted

```
index.html  style.css  app.js  img/  .nojekyll
firmware/<board>/manifest.json
firmware/<board>/network-survey-board-<board>.bin
```

`manifest.json` and the merged image come unchanged from
`dist/<board>/`, written by `scripts/build-all-targets.sh`. The manifest refers
to the image by a relative path, so any static host works. The `-app.bin` OTA
images are not part of the installer.

## Build and try it locally

```sh
scripts/build-all-targets.sh        # needs an activated ESP-IDF v6.0.1
flasher/stage.sh                    # writes dist/flasher/
cd dist/flasher && python3 -m http.server 8000
```

Open `http://localhost:8000/`. Web Serial needs a secure context, and
`localhost` counts as one, so installing works there; a LAN address over plain
HTTP does not. A board whose files are missing shows "not published here yet"
instead of an Install button.

## Publish on GitHub Pages

`.github/workflows/release.yml` stages this folder with the images of each full
release and deploys it to GitHub Pages, so the page always offers the latest
firmware. The repository settings it needs are in `docs/releasing.md`. Pages
serves over HTTPS, which Web Serial requires. The same folder works on any
other static HTTPS host.

## Updating ESP Web Tools

Change the version in the `<script>` tag of `index.html` (pick a release that
has been out for a couple of weeks and supports every chip in the board list),
then replace the `integrity` value with the hash of the new file:

```sh
curl -s https://unpkg.com/esp-web-tools@<version>/dist/web/install-button.js \
  | openssl dgst -sha256 -binary | base64
```

Prefix the result with `sha256-`.

## Adding a board

Add the board to `scripts/build-all-targets.sh`, to `BOARDS` in `stage.sh`, and
a card to `index.html` with
`<esp-web-install-button manifest="firmware/<board>/manifest.json">`.
