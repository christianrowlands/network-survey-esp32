// Renders the installer's steps. The flow itself lives in app.js.

import { BOARDS, SUPPORTED_CHIPS, ledColor, resetButton } from "./boards.js";
import { boardArt } from "./art.js";

const $ = (id) => document.getElementById(id);

const PANELS = ["gate", "connect", "choose", "install", "error", "done"];
const RAIL = ["connect", "choose", "install", "pair"];
const RAIL_STEP = { gate: "connect", connect: "connect", choose: "choose", install: "install", done: "pair" };
const PHASES = ["preparing", "erasing", "writing", "restarting"];
const PHASE_TEXT = {
  preparing: "Downloading the firmware",
  erasing: "Erasing the board",
  writing: "Writing the firmware",
  restarting: "Restarting the board",
};

let railStep = "connect";

function el(tag, className, text) {
  const node = document.createElement(tag);
  if (className) node.className = className;
  if (text !== undefined) node.textContent = text;
  return node;
}

function listText(items) {
  return items.length < 2 ? items.join("") : `${items.slice(0, -1).join(", ")} or ${items.at(-1)}`;
}

export function announce(text) {
  $("announce").textContent = text;
}

export function setStatus(id, text) {
  $(id).textContent = text;
}

// Shows one panel and moves focus to its heading. An error keeps the rail on
// the step that failed.
export function showPanel(name, { focus = true } = {}) {
  for (const panel of PANELS) $(`panel-${panel}`).hidden = panel !== name;
  $("rail").hidden = name === "gate";
  if (RAIL_STEP[name]) railStep = RAIL_STEP[name];
  const index = RAIL.indexOf(railStep);
  for (const item of $("rail").children) {
    const at = RAIL.indexOf(item.dataset.step);
    const state = at < index ? "done" : at > index ? "todo" : name === "error" ? "failed" : "current";
    item.dataset.state = state;
    if (at === index) item.setAttribute("aria-current", "step");
    else item.removeAttribute("aria-current");
    item.querySelector(".dot").textContent = state === "done" ? "✓" : String(at + 1);
  }
  if (!focus) return;
  const card = $("wizard");
  if (card.getBoundingClientRect().top < 0) card.scrollIntoView({ block: "start" });
  $(`panel-${name}`).querySelector("h2")?.focus({ preventScroll: true });
}

export function setVersion(version) {
  if (!version) return;
  $("fw-version").textContent = `v${version}`;
  $("fw-version").hidden = false;
}

const GATES = {
  mobile: ["Open this page on a computer",
    "Installing needs a USB connection from Chrome or Edge on a Windows, Mac, Linux or ChromeOS computer. Send yourself the link:"],
  browser: ["Open this page in Chrome or Edge",
    "This browser cannot talk to USB devices. Chrome and Edge on a computer can. Copy the link and open it there:"],
  insecure: ["Open this page over HTTPS",
    "Browsers only allow USB access on secure pages. Use the https address:"],
};

export function showGate(kind, url) {
  const [title, text] = GATES[kind];
  $("gate-title").textContent = title;
  $("gate-text").textContent = text;
  $("page-url").textContent = url;
  $("mail-link").href = `mailto:?subject=${encodeURIComponent("NS Scout installer")}&body=${encodeURIComponent(url)}`;
  showPanel("gate", { focus: false });
}

export function renderConnectArt() {
  $("connect-art").innerHTML = boardArt("devkitc", { withCable: true, name: "A DevKit board with a USB cable plugged into its USB port" });
}

// "Espressif" or "Seeed Studio": the full name without the model.
function maker(board) {
  return board.name.replace(board.shortName, "").trim();
}

function badges(board) {
  const row = el("span", "badges");
  if (board.recommended) row.append(el("span", "badge rec", "Recommended"));
  row.append(board.tested ? el("span", "badge ok", "Tested") : el("span", "badge warn", "Not yet tested on hardware"));
  return row;
}

function option(board, disabledReason) {
  const label = el("label", "option");
  const input = el("input");
  input.type = "radio";
  input.name = "board";
  input.value = board.id;
  input.disabled = Boolean(disabledReason);
  const art = el("span", "option-art");
  art.innerHTML = boardArt(board.art, { name: board.name });
  const body = el("span", "option-body");
  body.append(el("span", "option-name", board.shortName), el("span", "option-meta", `${maker(board)} · ${board.bands}`), badges(board));
  if (disabledReason) body.append(el("span", "option-note blocked", disabledReason));
  else if (board.note) body.append(el("span", "option-note", board.note));
  label.append(input, art, body);
  return label;
}

// published: ids whose firmware is on this site. Returns false when no board
// can take the install (unsupported chip, too little flash).
export function showChoose({ chip, flashMB, published }) {
  const matches = BOARDS.filter((board) => board.chipFamily === chip);
  const fieldset = $("choose-options");
  fieldset.querySelectorAll(".option").forEach((node) => node.remove());
  const found = flashMB ? `Found an ${chip} with ${flashMB} MB of flash.` : `Found an ${chip}.`;
  let usable = 0;
  for (const board of matches) {
    let reason = null;
    if (!published.has(board.id)) reason = "Not available on this site yet.";
    else if (flashMB && flashMB < board.flashMB) reason = `Needs ${board.flashMB} MB of flash; this board has ${flashMB} MB.`;
    fieldset.append(option(board, reason));
    if (!reason) usable += 1;
  }
  const first = fieldset.querySelector("input:not(:disabled)");
  if (first) first.checked = true;

  let title = matches.length === 1 ? "Is this your board?" : "Which board is this?";
  let text = matches.length > 1 ? `${found} Pick the board that matches yours.` : found;
  if (!matches.length) {
    title = "This board can't be a Scout";
    text = `It has an ${chip} chip. The Scout software runs on the ${listText(SUPPORTED_CHIPS)}. The ESP32-C5 is the best choice, as it also hears 5 GHz.`;
  } else if (!usable) {
    title = "This board can't take the install";
  }
  $("choose-title").textContent = title;
  $("choose-found").textContent = text;
  $("choose-found").classList.toggle("found", matches.length > 0);
  fieldset.hidden = !matches.length;
  $("erase-note").hidden = !usable;
  $("install").hidden = !usable;
  $("choose-back").textContent = usable ? "Back" : "Try another board";
  showPanel("choose");
  return usable > 0;
}

export function selectedBoard() {
  const id = new FormData($("choose-form")).get("board");
  return BOARDS.find((board) => board.id === id) ?? null;
}

export function showInstall(board) {
  $("install-on").textContent = `On your ${board.shortName}.`;
  setPhase("preparing");
  showPanel("install");
}

let lastAnnounced = "";

export function setPhase(phase, percent) {
  const at = PHASES.indexOf(phase);
  for (const item of $("phases").children) {
    const index = PHASES.indexOf(item.dataset.phase);
    item.dataset.state = index < at ? "done" : index === at ? "current" : "todo";
  }
  const meter = $("meter");
  const determinate = phase === "writing" && percent !== undefined;
  meter.classList.toggle("busy", !determinate && phase !== "restarting");
  meter.style.setProperty("--pct", phase === "restarting" ? "100%" : determinate ? `${percent}%` : "0%");
  if (determinate) meter.setAttribute("aria-valuenow", String(percent));
  else meter.removeAttribute("aria-valuenow");
  meter.setAttribute("aria-valuetext", determinate ? `${PHASE_TEXT[phase]}, ${percent}%` : PHASE_TEXT[phase]);
  $("percent").textContent = determinate ? `${percent}%` : "";
  // Announce each phase, and the write in quarters.
  const spoken = determinate ? `${PHASE_TEXT[phase]}, ${Math.floor(percent / 25) * 25}%` : PHASE_TEXT[phase];
  if (spoken !== lastAnnounced) announce((lastAnnounced = spoken));
}

export function showError({ title, text, tips = [], log = [] }) {
  $("error-title").textContent = title;
  $("error-text").textContent = text;
  const list = $("error-tips");
  list.replaceChildren(...tips.map((tip) => el("li", "", tip)));
  list.hidden = !tips.length;
  $("log").textContent = log.join("\n");
  $("log-box").hidden = !log.length;
  showPanel("error");
}

// name: the Scout's NS-XXXX name, or null. reading: still listening for it.
// downloadMode: the board started in download mode instead of the firmware.
export function showDone(board, { name = null, reading = false, downloadMode = false, focus = true } = {}) {
  const colour = ledColor(board);
  $("done-art").innerHTML = boardArt(board.art, { led: true, name: `${board.name} with its status light on` });
  $("done-art").style.setProperty("--led", `var(--led-${colour})`);
  $("led-dot").style.setProperty("--led", `var(--led-${colour})`);
  $("led-colour").textContent = colour;
  $("name-card").hidden = !name;
  $("done-name").textContent = name ?? "";
  $("pair-name").textContent = name ?? "your Scout (NS- and four characters)";
  $("name-missing").hidden = Boolean(name);
  $("reread").hidden = reading;
  $("name-missing-text").textContent = reading
    ? "Reading the Scout's name…"
    : downloadMode
      ? "Installed, but the board restarted in download mode instead of the Scout software. Unplug it, plug it back in, then read the name again."
      : `Installed. Its name did not show up: press ${resetButton(board)} on the board, then read it again.`;
  if (focus) showPanel("done");
  announce(name ? `Your Scout is named ${name}.` : $("name-missing-text").textContent);
}

// published: Set of ids, or null while the manifests load.
export function renderBoardList(published) {
  const list = $("board-list");
  list.replaceChildren(...BOARDS.map((board) => {
    const item = el("li", "board-card");
    const art = el("div", "board-art");
    art.innerHTML = boardArt(board.art, { name: board.name });
    const body = el("div", "board-body");
    body.append(el("h3", "", board.name), el("p", "board-meta", `${board.chipFamily} · Wi-Fi ${board.bands}, Bluetooth LE`), badges(board));
    if (board.note) body.append(el("p", "board-note", board.note));
    if (published && !published.has(board.id)) body.append(el("p", "board-note blocked", "Not available on this site yet."));
    if (board.buyUrl) {
      const link = el("a", "board-buy", `Where to buy (${board.buyLabel})`);
      link.href = board.buyUrl;
      body.append(link);
    }
    item.append(art, body);
    return item;
  }));
}
