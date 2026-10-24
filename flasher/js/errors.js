// Turns a failed step into plain words and next steps for the error panel.

const BUSY_NAMES = new Set(["InvalidStateError", "NetworkError"]);

const CLOSE_OTHERS = "Close anything else using the board: a serial monitor, the Arduino IDE, or this page in another tab.";
const CABLE = "Try another USB cable or port, plugged straight into the computer rather than a hub.";
const DEVKIT_DOWNLOAD = "On a DevKit: hold BOOT, press and release RESET (RST on the C3 board), then release BOOT and press Connect board.";
const XIAO_DOWNLOAD = "On the XIAO: unplug it, hold B while you plug it back in, then press Connect board.";

function isBusy(error) {
  const cause = error?.cause ?? error;
  return BUSY_NAMES.has(cause?.name) || /failed to open|already open/i.test(cause?.message ?? "");
}

function isUnplugged(error) {
  return /device has been lost|disconnected|unplugged/i.test(error?.cause?.message ?? error?.message ?? "");
}

// code: an InstallError code ("connect", "download", "write", "chip"),
// "load", "unplugged", or anything else. board: the chosen board, if any.
export function describeError(code, error, board) {
  if (code === "load") {
    return {
      title: "The installer did not load",
      text: "Part of this page comes from unpkg.com and could not be loaded or checked.",
      tips: ["Check your internet connection, then reload the page."],
    };
  }
  if (isBusy(error)) {
    return {
      title: "The board is busy",
      text: "Another program has the board's port open, so this page cannot use it.",
      tips: [CLOSE_OTHERS, "Unplug the board, plug it back in, and try again."],
    };
  }
  if (code === "unplugged" || isUnplugged(error)) {
    return {
      title: "The board was unplugged",
      text: code === "write"
        ? "The connection dropped during the install. The board is not damaged, but it needs the install again."
        : "The connection to the board dropped.",
      tips: ["Plug the board back in and try again.", CABLE],
    };
  }
  if (code === "connect") {
    return {
      title: "Couldn't talk to the board",
      text: "The board was found, but it did not answer. Putting it in download mode by hand usually fixes this.",
      tips: [DEVKIT_DOWNLOAD, XIAO_DOWNLOAD, CLOSE_OTHERS],
    };
  }
  if (code === "download") {
    return {
      title: "Couldn't download the firmware",
      text: "Nothing on the board was changed.",
      tips: ["Check your internet connection, then try again."],
    };
  }
  if (code === "write") {
    const tips = ["Keep the board plugged in until the install finishes.", CABLE];
    if (board?.downloadMode) tips.push(`If it keeps failing: ${board.downloadMode}`);
    else tips.push(DEVKIT_DOWNLOAD);
    return {
      title: "The install stopped partway",
      text: "The board is not damaged, but it needs the install again.",
      tips,
    };
  }
  return {
    title: "Something went wrong",
    text: error?.message ?? "The install did not finish.",
    tips: ["Unplug the board, plug it back in, and try again.", CABLE],
  };
}
