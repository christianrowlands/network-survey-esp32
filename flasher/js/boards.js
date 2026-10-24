// The boards the installer offers, recommended first. Each id matches a board
// in scripts/build-all-targets.sh and a folder in firmware/<id>/ when hosted.
// chipFamily is the name esptool-js reports for the chip.

export const BOARDS = [
  {
    id: "esp32c5-devkitc1",
    name: "Espressif ESP32-C5-DevKitC-1",
    shortName: "ESP32-C5-DevKitC-1",
    chipFamily: "ESP32-C5",
    bands: "2.4 and 5 GHz",
    flashMB: 8,
    tested: true,
    recommended: true,
    art: "devkitc",
    note: "Get the N8R8 version, with 8 MB of flash.",
    buyUrl: "https://www.espressif.com/en/products/devkits#socs-cat-ESP32-C5",
    buyLabel: "Espressif",
  },
  {
    id: "xiao-esp32c5",
    name: "Seeed Studio XIAO ESP32-C5",
    shortName: "XIAO ESP32-C5",
    chipFamily: "ESP32-C5",
    bands: "2.4 and 5 GHz",
    flashMB: 8,
    tested: false,
    art: "xiao",
    note: "Tiny, with a LiPo battery charger. Takes an optional GPS module.",
    buyUrl: "https://www.seeedstudio.com/Seeed-Studio-XIAO-ESP32C5-p-6609.html",
    buyLabel: "Seeed Studio",
    resetButton: "R",
    // Download mode, for the error panel. The DevKits' steps are in errors.js.
    downloadMode: "Unplug the board, hold the B button while you plug it back in, then release it.",
    ledColor: "yellow",
  },
  {
    id: "esp32c6-devkitc1",
    name: "Espressif ESP32-C6-DevKitC-1",
    shortName: "ESP32-C6-DevKitC-1",
    chipFamily: "ESP32-C6",
    bands: "2.4 GHz only",
    flashMB: 8,
    tested: false,
    art: "devkitc",
  },
  {
    id: "esp32s3-devkitc1",
    name: "Espressif ESP32-S3-DevKitC-1",
    shortName: "ESP32-S3-DevKitC-1",
    chipFamily: "ESP32-S3",
    bands: "2.4 GHz only",
    flashMB: 8,
    tested: false,
    art: "devkitc-s3",
  },
  {
    id: "esp32c3-devkitm1",
    name: "Espressif ESP32-C3-DevKitM-1",
    shortName: "ESP32-C3-DevKitM-1",
    chipFamily: "ESP32-C3",
    bands: "2.4 GHz only",
    flashMB: 4,
    tested: false,
    art: "devkitm",
    note: "Has one Micro-USB port.",
    resetButton: "RST",
  },
];

export const SUPPORTED_CHIPS = [...new Set(BOARDS.map((board) => board.chipFamily))];

export function resetButton(board) {
  return board?.resetButton ?? "RESET";
}

// The pairing cue: a fast blink, cyan on the RGB LED of the DevKits.
export function ledColor(board) {
  return board?.ledColor ?? "cyan";
}

export function manifestUrl(board) {
  return `firmware/${board.id}/manifest.json`;
}
