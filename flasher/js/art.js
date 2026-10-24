// Line drawings of the supported boards, as inline SVG so they follow the page
// colours. Traced from the makers' annotated photos (Espressif's user guides,
// Seeed's pinout): not to scale, but the USB ports, buttons and LED sit where
// they are on the real board, with the silkscreen labels the board carries.

const LABEL = 'font-size="9" font-weight="600" text-anchor="end"';
const CENTER = 'font-size="9" font-weight="600" text-anchor="middle"';

function port(x, y, w, h) {
  return `<rect x="${x}" y="${y}" width="${w}" height="${h}" rx="5" class="art-port"/>`;
}

function button(cx, cy, size = 14) {
  const h = size / 2;
  return `<rect x="${cx - h}" y="${cy - h}" width="${size}" height="${size}" rx="2" class="art-button"/>
    <circle cx="${cx}" cy="${cy}" r="${h - 3}"/>`;
}

function led(x, y, on) {
  const state = on ? " on" : "";
  return `<circle cx="${x}" cy="${y}" r="9" class="art-led-glow${state}"/>
    <rect x="${x - 5}" y="${y - 5}" width="10" height="10" rx="2" class="art-led${state}"/>`;
}

// A cable plugged into the port whose centre is (x, y), leaving towards dx.
function cable(x, y, dx) {
  const plugX = dx > 0 ? x : x - 30;
  const end = x + dx * 66;
  return `<g class="art-cable">
    <path d="M${x + dx * 32} ${y} H${end}"/>
    <rect x="${plugX}" y="${y - 9}" width="30" height="18" rx="5" class="art-plug"/>
  </g>`;
}

// The DevKit shape: module and antenna on the left, USB ports at the right end.
// ports: [{ y, label }], buttons: [{ y, label }] beside the ports, ledAt: [x, y].
function devkit({ name, ports, buttons, ledAt, withCable, on }) {
  const right = withCable ? 74 : 0;
  const cablePort = ports[0];
  return `<svg class="art" viewBox="0 0 ${280 + right} 124" role="img" aria-label="${name}">
    <rect x="22" y="14" width="236" height="96" rx="5" class="art-board"/>
    <path d="M40 21 H226 M40 103 H226" class="art-pins" stroke-dasharray="3 7"/>
    <rect x="4" y="30" width="26" height="64" rx="2" class="art-antenna-pcb"/>
    <path d="M10 38 h14 v10 h-14 v10 h14 v10 h-14 v10 h14" class="art-antenna"/>
    <rect x="30" y="32" width="84" height="60" rx="4" class="art-module"/>
    <rect x="38" y="40" width="68" height="44" rx="2"/>
    ${ports.map((p) => port(250, p.y - 10, 28, 20)).join("")}
    ${ports.map((p) => `<text x="244" y="${p.y + 3}" ${LABEL}>${p.label}</text>`).join("")}
    ${buttons.map((b) => button(b.x ?? 206, b.y)).join("")}
    ${buttons.map((b) => `<text x="${(b.x ?? 206) - 11}" y="${b.y + 3}" ${LABEL}>${b.label}</text>`).join("")}
    ${led(ledAt[0], ledAt[1], on)}
    ${withCable ? cable(278, cablePort.y, 1) : ""}
  </svg>`;
}

// The XIAO, upright as Seeed draws it: USB-C on the top edge with the charge
// (C) and user (L) LEDs beside it; R, the antenna socket and B along the bottom.
function xiao({ name, withCable, on }) {
  const top = withCable ? 74 : 0;
  return `<svg class="art" viewBox="0 ${-top} 160 ${150 + top}" role="img" aria-label="${name}">
    ${withCable ? `<g class="art-cable"><path d="M80 -40 V-74"/><rect x="71" y="-38" width="18" height="30" rx="5" class="art-plug"/></g>` : ""}
    <rect x="34" y="14" width="92" height="112" rx="12" class="art-board"/>
    <path d="M40 36 V112 M120 36 V112" class="art-pins" stroke-dasharray="6 6"/>
    ${port(64, 2, 32, 26)}
    <rect x="46" y="22" width="8" height="8" rx="2" class="art-led-off"/>
    <text x="50" y="44" ${CENTER}>C</text>
    ${led(110, 26, on)}
    <text x="110" y="44" ${CENTER}>L</text>
    <rect x="52" y="50" width="56" height="48" rx="3" class="art-module"/>
    ${button(56, 114, 12)}
    <text x="56" y="140" ${CENTER}>R</text>
    <circle cx="80" cy="113" r="7" class="art-antenna"/>
    <circle cx="80" cy="113" r="2.5"/>
    <text x="80" y="140" ${CENTER}>ANT</text>
    ${button(104, 114, 12)}
    <text x="104" y="140" ${CENTER}>B</text>
  </svg>`;
}

const DRAWINGS = {
  // ESP32-C5-DevKitC-1 and ESP32-C6-DevKitC-1.
  devkitc: (opts) => devkit({
    ...opts,
    ports: [{ y: 36, label: "USB" }, { y: 88, label: "UART" }],
    buttons: [{ y: 44, label: "BOOT" }, { y: 80, label: "RESET" }],
    ledAt: [150, 80],
  }),
  // ESP32-S3-DevKitC-1: RESET is the upper button.
  "devkitc-s3": (opts) => devkit({
    ...opts,
    ports: [{ y: 36, label: "USB" }, { y: 88, label: "UART" }],
    buttons: [{ y: 44, label: "RESET" }, { y: 80, label: "BOOT" }],
    ledAt: [140, 66],
  }),
  // ESP32-C3-DevKitM-1: one Micro-USB port between RST and BOOT.
  devkitm: (opts) => devkit({
    ...opts,
    ports: [{ y: 62, label: "USB" }],
    buttons: [{ x: 230, y: 34, label: "RST" }, { x: 230, y: 90, label: "BOOT" }],
    ledAt: [150, 36],
  }),
  xiao,
};

// kind is a board's art value.
export function boardArt(kind, { name = "", withCable = false, led: on = false } = {}) {
  return (DRAWINGS[kind] ?? DRAWINGS.devkitc)({ name, withCable, on });
}
