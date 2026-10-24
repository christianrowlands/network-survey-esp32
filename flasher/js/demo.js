// Shows one installer state without a board, for design review:
// index.html?demo=<state>. Nothing here touches a serial port.

import { BOARDS } from "./boards.js";
import { describeError } from "./errors.js";
import * as ui from "./ui.js";

const LOG = [
  "esptool.js",
  "Serial port WebSerial VendorID 0x303a ProductID 0x1001",
  "Connecting....",
  "Chip is ESP32-C5 (revision v1.0)",
  "Features: Wi-Fi 6 (dual-band), BT 5 (LE), IEEE802.15.4",
  "MAC: 3c:dc:75:aa:bb:cc",
];

const board = (id) => BOARDS.find((b) => b.id === id);

export function show(name, published) {
  const all = published.size ? published : new Set(BOARDS.map((b) => b.id));
  const url = "https://install.networksurvey.app/";
  const states = {
    "gate-mobile": () => ui.showGate("mobile", url),
    "gate-browser": () => ui.showGate("browser", url),
    "gate-insecure": () => ui.showGate("insecure", url),
    connect: () => ui.showPanel("connect", { focus: false }),
    choose: () => ui.showChoose({ chip: "ESP32-C5", flashMB: 8, published: all }),
    "choose-one": () => ui.showChoose({ chip: "ESP32-S3", flashMB: 8, published: all }),
    small: () => ui.showChoose({ chip: "ESP32-C6", flashMB: 4, published: all }),
    unsupported: () => ui.showChoose({ chip: "ESP32-S2", flashMB: 4, published: all }),
    erasing: () => { ui.showInstall(board("esp32c5-devkitc1")); ui.setPhase("erasing"); },
    install: () => {
      ui.showInstall(board("esp32c5-devkitc1"));
      let percent = 0;
      ui.setPhase("writing", 0);
      const timer = setInterval(() => {
        percent = Math.min(percent + 2, 100);
        ui.setPhase("writing", percent);
        if (percent === 100) clearInterval(timer);
      }, 150);
    },
    "error-connect": () => ui.showError({ ...describeError("connect", null, null), log: LOG }),
    "error-write": () => ui.showError({ ...describeError("write", null, board("xiao-esp32c5")), log: LOG }),
    "error-busy": () => ui.showError(describeError("connect", { name: "InvalidStateError" }, null)),
    done: () => ui.showDone(board("esp32c5-devkitc1"), { name: "NS-1A2B" }),
    "done-xiao": () => ui.showDone(board("xiao-esp32c5"), { name: "NS-7F3C" }),
    reading: () => ui.showDone(board("esp32c5-devkitc1"), { reading: true }),
    noname: () => ui.showDone(board("esp32c5-devkitc1"), {}),
  };
  (states[name] ?? states.connect)();
}
