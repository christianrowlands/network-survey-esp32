// Installs the firmware with esptool-js. The sequence follows flash.js in
// ESP Web Tools 10.4.0 (Apache-2.0), split in two so the page can show the
// detected chip and ask which board it is before anything is erased:
// connect() enters download mode and identifies the chip, install() downloads
// the image, erases the flash, writes it and restarts the board.

import { ESPLoader, Transport } from "esptool-js";

// esptool-js connects at 115200 and switches to this rate once its stub runs.
// UART bridges handle 460800 (esptool's own default); the chips' built-in USB
// serial ignores the baud rate.
const BAUD = 460800;

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

export class InstallError extends Error {
  constructor(code, message, cause) {
    super(message, { cause });
    this.code = code;
  }
}

// Sends esptool's log lines to onLine, for the page's technical details.
function terminalFor(onLine) {
  let line = "";
  return {
    clean() {},
    write(text) { line += text; },
    writeLine(text) { onLine(line + text); line = ""; },
  };
}

export async function connect(port, onLog = () => {}) {
  const transport = new Transport(port, false);
  const esploader = new ESPLoader({
    transport,
    baudrate: BAUD,
    terminal: terminalFor(onLog),
    enableTracing: false,
  });
  try {
    await esploader.main();
    await esploader.flashId();
  } catch (error) {
    // A port that would not open (in use elsewhere) has nothing to reset.
    if (error?.name !== "InvalidStateError") await restart({ transport, esploader }).catch(() => {});
    throw new InstallError("connect", "Could not connect to the board.", error);
  }
  return { transport, esploader, chip: esploader.chip.CHIP_NAME, flashMB: await flashSizeMB(esploader) };
}

// The flash size from the chip's JEDEC ID, or null when the ID is not one
// esptool-js knows (its detectFlashSize() would guess 4 MB).
async function flashSizeMB(esploader) {
  try {
    const id = await esploader.readFlashId();
    const size = esploader.DETECTED_FLASH_SIZES[(id >> 16) & 0xff];
    return size?.endsWith("MB") ? Number.parseInt(size, 10) : null;
  } catch {
    return null;
  }
}

async function download(manifestUrl, chipFamily) {
  let manifest;
  try {
    const response = await fetch(manifestUrl, { cache: "no-cache" });
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    manifest = await response.json();
  } catch (error) {
    throw new InstallError("download", "Could not download the firmware.", error);
  }
  const build = manifest?.builds?.find((b) => b.chipFamily === chipFamily);
  if (!build) throw new InstallError("chip", `This firmware is not for an ${chipFamily}.`);
  if (!build.parts?.length) throw new InstallError("download", "The firmware manifest has no images.");
  const base = new URL(manifestUrl, location.href);
  const fileArray = [];
  for (const part of build.parts) {
    try {
      const response = await fetch(new URL(part.path, base), { cache: "no-cache" });
      if (!response.ok) throw new Error(`HTTP ${response.status}`);
      const data = new Uint8Array(await response.arrayBuffer());
      if (!data.length || !Number.isInteger(part.offset)) throw new Error(`Bad image ${part.path}`);
      fileArray.push({ data, address: part.offset });
    } catch (error) {
      throw new InstallError("download", "Could not download the firmware.", error);
    }
  }
  return { manifest, fileArray };
}

// onState gets { phase: "preparing" | "erasing" | "writing" | "restarting",
// percent? }. The image is downloaded before the erase, so a network failure
// leaves the board as it was.
export async function install(session, manifestUrl, onState) {
  const { esploader } = session;
  onState({ phase: "preparing" });
  const { manifest, fileArray } = await download(manifestUrl, session.chip);
  const totalSize = fileArray.reduce((sum, file) => sum + file.data.length, 0);

  onState({ phase: "erasing" });
  try {
    await esploader.eraseFlash();
  } catch (error) {
    throw new InstallError("write", "Erasing the board failed.", error);
  }

  onState({ phase: "writing", percent: 0 });
  let totalWritten = 0;
  try {
    await esploader.writeFlash({
      fileArray,
      flashSize: "keep",
      flashMode: "keep",
      flashFreq: "keep",
      eraseAll: false,
      compress: true,
      reportProgress: (fileIndex, written, total) => {
        const uncompressed = (written / total) * fileArray[fileIndex].data.length;
        if (written === total) {
          totalWritten += uncompressed;
          return;
        }
        const percent = Math.floor(((totalWritten + uncompressed) / totalSize) * 100);
        onState({ phase: "writing", percent });
      },
    });
  } catch (error) {
    throw new InstallError("write", "Writing the firmware failed.", error);
  }
  onState({ phase: "writing", percent: 100 });
  onState({ phase: "restarting" });
  // The image is in place. A board on its built-in USB can drop off the bus
  // as it resets, so a failure here does not fail the install.
  await restart(session).catch((error) => console.warn("Restart after install:", error));
  return manifest.version;
}

// Restarts the board into its firmware and releases the port.
export async function restart({ transport, esploader }) {
  try {
    await transport.setRTS(true);
    await sleep(100);
    await esploader.after();
  } finally {
    await transport.disconnect().catch(() => {});
  }
}
