// The installer's flow: check the browser, connect to the board and identify
// its chip, let the user confirm the board, install, then read the Scout's
// NS-XXXX name and show how to pair it. flash.js does the installing.

import { BOARDS, manifestUrl } from "./boards.js";
import { readBoardName } from "./board-name.js";
import { describeError } from "./errors.js";
import * as ui from "./ui.js";

const $ = (id) => document.getElementById(id);
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

// Espressif's built-in USB, WCH CH34x, Silicon Labs CP210x and FTDI bridges.
const PORT_FILTERS = [0x303a, 0x1a86, 0x10c4, 0x0403].map((usbVendorId) => ({ usbVendorId }));
const LISTENING = "Restarting the board and listening. If nothing appears in a few seconds, press its reset button.";

const state = { port: null, session: null, board: null, log: [], published: new Set(), busy: false, read: null };

// Loads esptool-js only on browsers that can use it, and retries after a failure.
let flashModule = null;
function loadFlash() {
  flashModule ??= import("./flash.js").catch((error) => {
    flashModule = null;
    throw error;
  });
  return flashModule;
}

function pageUrl() {
  const url = `${location.origin}${location.pathname}`;
  return window.isSecureContext ? url : url.replace(/^http:/, "https:");
}

function environment() {
  if (!window.isSecureContext) return "insecure";
  if ("serial" in navigator) return null;
  const ua = navigator.userAgent;
  const iPad = /Macintosh/.test(ua) && navigator.maxTouchPoints > 1;
  const mobile = navigator.userAgentData?.mobile || iPad || /Android|iPhone|iPad|iPod|Mobile/i.test(ua);
  return mobile ? "mobile" : "browser";
}

// Which boards have firmware on this site, and the version it is.
async function checkManifests() {
  const results = await Promise.all(BOARDS.map(async (board) => {
    try {
      const response = await fetch(manifestUrl(board), { cache: "no-cache" });
      if (!response.ok) return null;
      return { id: board.id, version: (await response.json()).version };
    } catch {
      return null;
    }
  }));
  const found = results.filter(Boolean);
  state.published = new Set(found.map((result) => result.id));
  ui.setVersion(found[0]?.version);
  ui.renderBoardList(state.published);
}

// Wake lock and leave-page warning while the flash is being written. The
// browser drops a wake lock when the tab is hidden, so take it again on return.
let wakeLock = null;
let guarding = false;
function onBeforeUnload(event) {
  event.preventDefault();
  event.returnValue = "";
}
async function lockScreen() {
  wakeLock = await navigator.wakeLock?.request("screen").catch(() => null);
}
document.addEventListener("visibilitychange", () => {
  if (guarding && document.visibilityState === "visible") lockScreen();
});
async function guard(on) {
  guarding = on;
  if (on) {
    addEventListener("beforeunload", onBeforeUnload);
    await lockScreen();
  } else {
    removeEventListener("beforeunload", onBeforeUnload);
    await wakeLock?.release().catch(() => {});
    wakeLock = null;
  }
}

// Runs one port action at a time. Its buttons stay disabled until it ends, so
// a second click cannot open the port twice.
const ACTION_BUTTONS = ["connect", "connect-all", "install", "choose-back", "retry", "again", "read-name"];
async function exclusive(task) {
  if (state.busy) return;
  state.busy = true;
  for (const id of ACTION_BUTTONS) $(id).disabled = true;
  try {
    await task();
  } finally {
    state.busy = false;
    for (const id of ACTION_BUTTONS) $(id).disabled = false;
  }
}

// InstallError codes are strings; a DOMException's numeric code is not one.
function codeOf(error, fallback) {
  return typeof error?.code === "string" ? error.code : fallback;
}

async function releaseSession() {
  const session = state.session;
  state.session = null;
  if (!session) return;
  const { restart } = await loadFlash();
  await restart(session).catch(() => {});
}

function fail(code, error) {
  if (error) console.error(error);
  if (error?.cause) console.error(error.cause);
  ui.showError({ ...describeError(code, error, state.board), log: state.log });
}

function onConnect(filtered) {
  return exclusive(async () => {
    ui.setStatus("connect-status", "");
    let port;
    try {
      port = await navigator.serial.requestPort(filtered ? { filters: PORT_FILTERS } : {});
    } catch (error) {
      ui.setStatus("connect-status", error.name === "NotFoundError"
        ? "No board chosen. Press Connect board and pick it from the list. Not there? See below."
        : `The browser would not open the port list: ${error.message}`);
      return;
    }
    state.port = port;
    state.log = [];
    state.board = null;
    ui.setStatus("connect-status", "Connecting to the board\u2026");
    try {
      const flash = await loadFlash().catch((error) => {
        throw Object.assign(error, { code: "load" });
      });
      state.session = await flash.connect(port, (line) => state.log.push(line));
      ui.showChoose({ ...state.session, published: state.published });
    } catch (error) {
      await releaseSession();
      fail(codeOf(error, "connect"), error);
    } finally {
      ui.setStatus("connect-status", "");
    }
  });
}

async function onInstall(event) {
  event.preventDefault();
  let installed = false;
  await exclusive(async () => {
    const board = ui.selectedBoard();
    if (!board || !state.session) return;
    state.board = board;
    ui.showInstall(board);
    await guard(true);
    try {
      const { install } = await loadFlash();
      await install(state.session, manifestUrl(board), ({ phase, percent }) => ui.setPhase(phase, percent));
      state.session = null;
      installed = true;
    } catch (error) {
      await releaseSession();
      fail(codeOf(error, "write"), error);
    } finally {
      await guard(false);
    }
  });
  if (!installed) return;
  ui.showDone(state.board, { reading: true });
  await readName(state.port);
}

// Stops a name read that is still listening and waits for it to close the port.
async function cancelRead() {
  const read = state.read;
  if (!read) return;
  read.abort.abort();
  await read.done;
}

async function readName(port) {
  await cancelRead();
  const abort = new AbortController();
  const board = state.board;
  const done = (async () => {
    let name = null;
    try {
      await sleep(300);
      if (!abort.signal.aborted) name = await readBoardName(port, null, abort.signal);
    } catch (error) {
      console.error(error);
    }
    // A read that was cancelled belongs to a Scout the user has moved on from.
    if (!abort.signal.aborted) ui.showDone(board, { name, focus: false });
  })();
  state.read = { abort, done };
  await done;
  if (state.read?.done === done) state.read = null;
}

async function onReread() {
  if (state.busy) return;
  ui.showDone(state.board, { reading: true, focus: false });
  let port = state.port;
  try {
    // A board that restarted may come back as a new port.
    const ports = await navigator.serial.getPorts();
    if (!ports.includes(port)) port = await navigator.serial.requestPort({ filters: PORT_FILTERS });
  } catch {
    ui.showDone(state.board, { focus: false });
    return;
  }
  state.port = port;
  await readName(port);
}

function startOver() {
  return exclusive(async () => {
    await cancelRead();
    await releaseSession();
    state.board = null;
    ui.showPanel("connect");
  });
}

// The standalone "Find a Scout's name" tool.
function setUpNameTool() {
  const button = $("read-name");
  const output = $("board-name");
  if (!("serial" in navigator)) {
    button.disabled = true;
    ui.setStatus("name-status", "Needs Chrome or Edge on a computer.");
    return;
  }
  button.addEventListener("click", () => exclusive(async () => {
    output.textContent = "";
    ui.setStatus("name-status", "");
    let port;
    try {
      port = await navigator.serial.requestPort({ filters: PORT_FILTERS });
    } catch {
      ui.setStatus("name-status", "No Scout chosen. Press Show Scout name again and pick it from the list.");
      return;
    }
    await cancelRead();
    try {
      const name = await readBoardName(port, () => ui.setStatus("name-status", LISTENING));
      output.textContent = name ?? "";
      ui.setStatus("name-status", name
        ? "This is the Scout's name. Write it on the board or its case."
        : "No name seen. Press the reset button on the board, then try again. If it still does not show, install the Scout software again.");
    } catch (error) {
      ui.setStatus("name-status", error.name === "InvalidStateError"
        ? "The port is busy. Finish or go back from the setup above, close any other serial monitor, and try again."
        : `Could not read the Scout: ${error.message}. Unplug it, plug it back in and try again.`);
    }
  }));
}

function setUpGate() {
  $("copy-link").addEventListener("click", async (event) => {
    const button = event.currentTarget;
    await navigator.clipboard?.writeText(pageUrl()).catch(() => {});
    button.textContent = "Copied";
    setTimeout(() => { button.textContent = "Copy link"; }, 2000);
  });
}

async function copyName(event) {
  const button = event.currentTarget;
  await navigator.clipboard?.writeText($("done-name").textContent).catch(() => {});
  button.textContent = "Copied";
  setTimeout(() => { button.textContent = "Copy"; }, 2000);
}

function start() {
  ui.renderConnectArt();
  ui.renderBoardList(null);
  setUpGate();
  setUpNameTool();
  const manifests = checkManifests();

  const params = new URLSearchParams(location.search);
  if (params.has("demo")) {
    manifests.then(() => import("./demo.js")).then((demo) => demo.show(params.get("demo"), state.published));
    return;
  }

  const gate = environment();
  if (gate) {
    ui.showGate(gate, pageUrl());
    return;
  }
  ui.showPanel("connect", { focus: false });
  loadFlash().catch(() => {});

  $("connect").addEventListener("click", () => onConnect(true));
  $("connect-all").addEventListener("click", () => onConnect(false));
  $("choose-form").addEventListener("submit", onInstall);
  $("choose-back").addEventListener("click", startOver);
  $("retry").addEventListener("click", startOver);
  $("again").addEventListener("click", startOver);
  $("reread").addEventListener("click", onReread);
  $("copy-name").addEventListener("click", copyName);
  navigator.serial.addEventListener("disconnect", (event) => {
    if (event.target === state.port && state.session && !$("panel-choose").hidden) {
      releaseSession();
      fail("unplugged", null);
    }
  });
}

start();
