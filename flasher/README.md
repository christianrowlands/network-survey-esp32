# Web installer

A static page, served at <https://install.networksurvey.app/>, that turns an
ESP32 board into an NS Scout from the browser. It uses Web Serial, so it needs
Chrome or Edge on a computer. Later updates go over Bluetooth from the iOS app,
not through this page.

## The flow

1. **Connect.** The user plugs the board in and picks its port. The page puts
   the board in download mode and reads its chip and flash size.
2. **Choose.** Only the boards with that chip are offered (two share the
   ESP32-C5). A chip the firmware does not support, or too little flash, stops
   here with an explanation.
3. **Install.** The page downloads the image first, then erases the whole flash
   (old pairings included), writes the image and restarts the board.
4. **Pair.** The page reopens the same port, reads the Scout's `NS-XXXX` name
   from the boot log, and shows how to pair it in the app.

Phones, other browsers and plain HTTP get a page that explains where to open
the installer instead. Errors explain what to try next, with the esptool log
under "Technical details".

## Files

| File | Purpose |
|---|---|
| `index.html` | The page: header, the four-step card, supported boards, help, name tool. |
| `style.css` | Colors (light and dark, from the system setting), base styles, the sections below the card. |
| `wizard.css` | The step rail, the card and its panels, board choice, progress. |
| `js/boards.js` | The board catalog: one entry per board, recommended first. |
| `js/flash.js` | Connects, installs and restarts with esptool-js. |
| `js/app.js` | The flow: browser checks, the steps, error handling. |
| `js/ui.js` | Renders each step. |
| `js/errors.js` | Error titles and tips. |
| `js/board-name.js` | Reads `NS-XXXX` from the serial boot log. |
| `js/art.js` | Line drawings of the boards. |
| `js/demo.js` | Shows one step without a board, for design review (see below). |
| `fonts/` | The heading font: Mona Sans cut to Latin at width 112 and renamed Scout Display, as its licence (`fonts/OFL.txt`) asks of modified versions. |
| `img/` | Network Survey icon and lockups. |
| `stage.sh` | Assembles the page and the firmware into a folder ready to host. |
| `stage-release.sh` | The same, with the images of a published release instead of a local build. |

The page loads esptool-js from unpkg.com at an exact version, through an import
map with an integrity hash, and only on browsers that can install. It has no
analytics, cookies or other third-party requests.

## Layout when hosted

```
index.html  *.css  js/  fonts/  img/  .nojekyll
firmware/<board>/manifest.json
firmware/<board>/network-survey-board-<board>.bin
```

`manifest.json` and the merged image come unchanged from `dist/<board>/`,
written by `scripts/build-all-targets.sh`. The manifest keeps the ESP Web Tools
format (`builds[].chipFamily`, `parts[].path` and `offset`) and refers to the
image by a relative path, so any static host works. The page also reads the
version from it, and marks a board whose manifest is missing as not available.
The `-app.bin` OTA images are not part of the installer.

## Build and try it locally

```sh
scripts/build-all-targets.sh        # needs an activated ESP-IDF v6.0.1
flasher/stage.sh                    # writes dist/flasher/
cd dist/flasher && python3 -m http.server 8000
```

Open `http://localhost:8000/`. Web Serial needs a secure context, and
`localhost` counts as one, so installing works there; a LAN address over plain
HTTP does not.

To review a step without a board, add `?demo=<step>`: `connect`, `choose`,
`choose-one`, `small`, `unsupported`, `erasing`, `install`, `error-connect`,
`error-write`, `error-busy`, `done`, `done-xiao`, `reading`, `noname`,
`gate-mobile`, `gate-browser` or `gate-insecure`.

## Publish on GitHub Pages

`.github/workflows/release.yml` stages this folder with the images of each full
release and deploys it to GitHub Pages, so the page always offers the latest
firmware. To publish a change to the page between releases, run
`.github/workflows/installer.yml` by hand on `main` (Actions > installer > Run
workflow): it stages the page with the latest full release's images through
`stage-release.sh` and deploys it. Pages serves it at the custom domain `install.networksurvey.app`, over
HTTPS, which Web Serial requires. The repository and DNS settings it needs are
in `docs/releasing.md`. The same folder works on any other static HTTPS host.

## Updating esptool-js

`js/flash.js` follows the install sequence of ESP Web Tools 10.4.0, which uses
esptool-js 0.6. To move to a new version, pick a release that has been out for a
couple of weeks and supports every chip in the board list, change the version in
both URLs of the import map in `index.html`, and replace the `integrity` value
with the hash of the new file:

```sh
curl -s https://unpkg.com/esptool-js@<version>/bundle.js \
  | openssl dgst -sha384 -binary | base64
```

Prefix the result with `sha384-`. Then install on a real board through both of
a DevKit's USB ports before publishing.

## Adding a board

Add the board to `scripts/build-all-targets.sh`, to `BOARDS` in `stage.sh`, and
an entry to `js/boards.js` with the same id and the `chipFamily` that
esptool-js reports for its chip. Give it a drawing: reuse one from
`js/art.js` (`devkitc`, `devkitc-s3`, `devkitm`, `xiao`) if the USB ports, buttons
and LED are in the same places, or add one traced from the maker's photo. A board with a different reset button label, download
mode steps or LED color sets `resetButton`, `downloadMode` or `ledColor`.
