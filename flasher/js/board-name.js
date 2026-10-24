// Reads a Scout's NS-XXXX name from its serial boot log.

// The firmware logs "BLE address ..., name NS-1A2B" and "advertising as NS-1A2B"
// at boot.
const NAME_PATTERN = /(?:name|advertising as) (NS-[0-9A-F]{4})\b/;
const LISTEN_MS = 15000;

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

// Resets the board into its firmware and watches the log for its name.
// DTR low keeps BOOT released; an RTS pulse drives EN (on a USB to UART
// bridge) or asks the built-in USB serial to reset the chip. Returns null on
// timeout or when signal aborts; the port is closed either way.
export async function readBoardName(port, onStatus, signal) {
  await port.open({ baudRate: 115200 });
  const decoder = new TextDecoder();
  let reader;
  let text = "";
  let timer;
  let onAbort;
  try {
    reader = port.readable.getReader();
    await port.setSignals({ dataTerminalReady: false, requestToSend: true });
    await sleep(100);
    await port.setSignals({ dataTerminalReady: false, requestToSend: false });
    onStatus?.("listening");
    const stop = new Promise((resolve) => {
      timer = setTimeout(() => resolve({ done: true }), LISTEN_MS);
      onAbort = () => resolve({ done: true });
      signal?.addEventListener("abort", onAbort);
      if (signal?.aborted) onAbort();
    });
    for (;;) {
      const { value, done } = await Promise.race([reader.read(), stop]);
      if (done) return null;
      text += decoder.decode(value, { stream: true });
      const match = NAME_PATTERN.exec(text);
      if (match) return match[1];
      // Keep only the tail so a partial line can still complete.
      if (text.length > 4096) text = text.slice(-256);
    }
  } finally {
    clearTimeout(timer);
    signal?.removeEventListener("abort", onAbort);
    if (reader) {
      await reader.cancel().catch(() => {});
      reader.releaseLock();
    }
    await port.close().catch(() => {});
  }
}
