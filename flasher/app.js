// Page logic for the board installer: checks that each board's firmware is
// published, shows its version, and reads the board's NS-XXXX name from the
// serial boot log after an install. ESP Web Tools does the installing.

const hasSerial = "serial" in navigator;

// The firmware logs "BLE address ..., name NS-1A2B" and "advertising as NS-1A2B"
// at boot.
const NAME_PATTERN = /(?:name|advertising as) (NS-[0-9A-F]{4})\b/;
const LISTEN_MS = 15000;

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

async function checkManifests() {
  const versionText = document.getElementById("fw-version");
  let version = null;
  for (const card of document.querySelectorAll(".board")) {
    const button = card.querySelector("esp-web-install-button");
    const url = button.getAttribute("manifest");
    try {
      const response = await fetch(url, { cache: "no-cache" });
      if (!response.ok) throw new Error(`HTTP ${response.status}`);
      const manifest = await response.json();
      version ??= manifest.version;
    } catch {
      const note = document.createElement("p");
      note.className = "missing";
      note.textContent = "Firmware for this board is not published here yet.";
      button.replaceWith(note);
    }
  }
  if (version) versionText.textContent = `Firmware version ${version}.`;
}

// Resets the board into its firmware and watches the log for its name.
// DTR low keeps BOOT released; an RTS pulse drives EN (on a USB to UART
// bridge) or asks the built-in USB serial to reset the chip.
async function readBoardName(port, onStatus) {
  await port.open({ baudRate: 115200 });
  const reader = port.readable.getReader();
  const decoder = new TextDecoder();
  let text = "";
  let timer;
  try {
    await port.setSignals({ dataTerminalReady: false, requestToSend: true });
    await sleep(100);
    await port.setSignals({ dataTerminalReady: false, requestToSend: false });
    onStatus("Restarting the board and listening. If nothing appears in a few seconds, press RESET on the board.");
    const timeout = new Promise((resolve) => {
      timer = setTimeout(() => resolve({ done: true, timedOut: true }), LISTEN_MS);
    });
    for (;;) {
      const { value, done } = await Promise.race([reader.read(), timeout]);
      if (done) return null;
      text += decoder.decode(value, { stream: true });
      const match = NAME_PATTERN.exec(text);
      if (match) return match[1];
      // Keep only the tail so a partial line can still complete.
      if (text.length > 4096) text = text.slice(-256);
    }
  } finally {
    clearTimeout(timer);
    await reader.cancel().catch(() => {});
    reader.releaseLock();
    await port.close().catch(() => {});
  }
}

function setUpNameReader() {
  const button = document.getElementById("read-name");
  const output = document.getElementById("board-name");
  const status = document.getElementById("name-status");
  if (!hasSerial) {
    button.disabled = true;
    status.textContent = "Needs Chrome or Edge on a computer.";
    return;
  }
  button.addEventListener("click", async () => {
    output.textContent = "";
    status.textContent = "";
    let port;
    try {
      port = await navigator.serial.requestPort();
    } catch {
      status.textContent = "No port chosen. Press Show board name again and pick the board's port.";
      return;
    }
    button.disabled = true;
    try {
      const name = await readBoardName(port, (message) => { status.textContent = message; });
      if (name) {
        output.textContent = name;
        status.textContent = "This is your board's name. Write it on the board or its case.";
      } else {
        status.textContent = "No name seen. Press RESET on the board, then try again. If it still does not show, install the firmware again.";
      }
    } catch (error) {
      if (error.name === "InvalidStateError") {
        status.textContent = "The port is busy. Close the installer window (or any other serial monitor) and try again.";
      } else {
        status.textContent = `Could not read the board: ${error.message}. Unplug it, plug it back in and try again.`;
      }
    } finally {
      button.disabled = false;
    }
  });
}

document.getElementById("unsupported").hidden = hasSerial;
setUpNameReader();
checkManifests();
