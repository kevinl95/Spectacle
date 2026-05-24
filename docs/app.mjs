const manifestUrl = "./latest-release.json";
const esptoolModuleUrl = "https://cdn.jsdelivr.net/npm/esptool-js@0.6.0/+esm";

let ESPLoaderClass = null;
let TransportClass = null;

const state = {
  manifest: null,
  loader: null,
  transport: null,
  connectedChip: null,
  busy: false,
};

const elements = {
  manifestState: document.querySelector("#manifestState"),
  serialState: document.querySelector("#serialState"),
  connectionState: document.querySelector("#connectionState"),
  releaseTag: document.querySelector("#releaseTag"),
  releaseDate: document.querySelector("#releaseDate"),
  boardName: document.querySelector("#boardName"),
  chipName: document.querySelector("#chipName"),
  deviceCount: document.querySelector("#deviceCount"),
  configPath: document.querySelector("#configPath"),
  releaseLink: document.querySelector("#releaseLink"),
  updateNote: document.querySelector("#updateNote"),
  recoveryNote: document.querySelector("#recoveryNote"),
  connectButton: document.querySelector("#connectButton"),
  disconnectButton: document.querySelector("#disconnectButton"),
  flashButton: document.querySelector("#flashButton"),
  factoryFlashButton: document.querySelector("#factoryFlashButton"),
  imageList: document.querySelector("#imageList"),
  firmwareOffset: document.querySelector("#firmwareOffset"),
  spiffsOffset: document.querySelector("#spiffsOffset"),
  progressLabel: document.querySelector("#progressLabel"),
  progressBar: document.querySelector("#progressBar"),
  progressDetail: document.querySelector("#progressDetail"),
  serialLog: document.querySelector("#serialLog"),
};

const terminal = {
  clean() {
    elements.serialLog.textContent = "";
  },
  write(data) {
    appendLog(data);
  },
  writeLine(data) {
    appendLog(`${data}\n`);
  },
};


function appendLog(text) {
  elements.serialLog.textContent += text;
  elements.serialLog.scrollTop = elements.serialLog.scrollHeight;
}


function formatDate(isoString) {
  if (!isoString) {
    return "Pending";
  }

  const date = new Date(isoString);
  if (Number.isNaN(date.getTime())) {
    return "Pending";
  }

  return new Intl.DateTimeFormat(undefined, {
    year: "numeric",
    month: "short",
    day: "numeric",
    hour: "numeric",
    minute: "2-digit",
    timeZoneName: "short",
  }).format(date);
}


function parseAddress(address) {
  return Number.parseInt(address, 16);
}


function isSerialSupported() {
  return "serial" in navigator;
}


function setPill(element, text) {
  element.textContent = text;
}


async function ensureEsptoolLoaded() {
  if (ESPLoaderClass && TransportClass) {
    return;
  }

  try {
    const module = await import(esptoolModuleUrl);
    ESPLoaderClass = module.ESPLoader;
    TransportClass = module.Transport;
  } catch (error) {
    throw new Error(
      error instanceof Error
        ? `Failed to load flasher module: ${error.message}`
        : `Failed to load flasher module: ${String(error)}`
    );
  }
}


function updateButtonState() {
  const serialSupported = isSerialSupported();
  const ready = state.manifest?.status === "ready";
  const connected = Boolean(state.loader);

  elements.connectButton.disabled = !serialSupported || state.busy || connected;
  elements.disconnectButton.disabled = !connected || state.busy;
  elements.flashButton.disabled = !ready || !connected || state.busy;
  elements.factoryFlashButton.disabled = !ready || !connected || state.busy;
}


function setBusy(isBusy) {
  state.busy = isBusy;
  updateButtonState();
}


function renderManifest() {
  if (!state.manifest || state.manifest.status !== "ready") {
    const message = state.manifest?.message ?? "Run the release workflow to publish the first manifest.";
    setPill(elements.manifestState, message);
    elements.releaseTag.textContent = "Unpublished";
    elements.releaseDate.textContent = "Pending";
    elements.deviceCount.textContent = "Pending";
    elements.releaseLink.href = "#";
    elements.releaseLink.textContent = "Release assets will appear here";
    elements.imageList.innerHTML = "<li><div class=\"image-title\">No release images published yet.</div></li>";
    elements.progressLabel.textContent = "Waiting for a published release manifest.";
    elements.progressDetail.textContent = "Run the workflow dispatch or wait for the weekly release.";
    updateButtonState();
    return;
  }

  const { manifest } = state;
  setPill(elements.manifestState, `Loaded ${manifest.release.tag}`);
  elements.releaseTag.textContent = manifest.release.tag;
  elements.releaseDate.textContent = formatDate(manifest.release.publishedAt);
  elements.boardName.textContent = manifest.board;
  elements.chipName.textContent = manifest.chip;
  elements.deviceCount.textContent = `${manifest.config.deviceCount} signatures`;
  elements.configPath.textContent = manifest.config.runtimePath;
  elements.releaseLink.href = manifest.release.releaseUrl;
  elements.releaseLink.textContent = "Open release assets";
  elements.updateNote.textContent = manifest.update.note;
  elements.recoveryNote.textContent = manifest.recovery.note;
  elements.firmwareOffset.textContent = manifest.partitions.appOffset;
  elements.spiffsOffset.textContent = manifest.partitions.spiffsOffset;

  elements.imageList.innerHTML = manifest.images
    .map(
      (image) => `
        <li>
          <div class="image-line">
            <span class="image-title">${image.label}</span>
            ${image.default ? '<span class="image-badge">Routine</span>' : '<span class="image-badge">Recovery</span>'}
          </div>
          <div class="image-line image-meta">
            <span>${image.filename}</span>
            <span>${image.address}</span>
          </div>
          <div class="image-meta">${image.description}</div>
        </li>
      `
    )
    .join("");

  updateButtonState();
}


function renderConnectionState() {
  if (state.connectedChip) {
    setPill(elements.connectionState, `Connected to ${state.connectedChip}`);
    return;
  }

  setPill(elements.connectionState, "Not connected");
}


async function loadManifest() {
  try {
    const response = await fetch(`${manifestUrl}?t=${Date.now()}`, { cache: "no-store" });
    if (!response.ok) {
      throw new Error(`Manifest request failed with ${response.status}`);
    }

    state.manifest = await response.json();
  } catch (error) {
    state.manifest = {
      status: "placeholder",
      message: error instanceof Error ? error.message : "Manifest request failed.",
    };
  }

  renderManifest();
}


async function connectDevice() {
  if (!state.manifest || state.manifest.status !== "ready") {
    appendLog("Release manifest is not ready yet.\n");
    return;
  }

  if (!isSerialSupported()) {
    appendLog("Web Serial is not available in this browser. Use Chrome or Edge to flash.\n");
    return;
  }

  setBusy(true);
  appendLog("Requesting serial port access...\n");

  try {
    await ensureEsptoolLoaded();
    const port = await navigator.serial.requestPort();
    const transport = new TransportClass(port, false);
    const loader = new ESPLoaderClass({
      transport,
      baudrate: state.manifest.baudRate,
      terminal,
      debugLogging: false,
    });
    const chipName = await loader.main();

    state.transport = transport;
    state.loader = loader;
    state.connectedChip = chipName;
    renderConnectionState();
    appendLog(`Connected to ${chipName}.\n`);
  } catch (error) {
    appendLog(`Connection failed: ${error instanceof Error ? error.message : String(error)}\n`);
    await disconnectDevice({ silent: true });
  } finally {
    setBusy(false);
  }
}


async function disconnectDevice({ silent = false } = {}) {
  try {
    if (state.transport) {
      await state.transport.disconnect();
    }
  } catch (error) {
    if (!silent) {
      appendLog(`Disconnect warning: ${error instanceof Error ? error.message : String(error)}\n`);
    }
  } finally {
    state.transport = null;
    state.loader = null;
    state.connectedChip = null;
    renderConnectionState();
    updateButtonState();
  }
}


async function fetchImage(image) {
  const response = await fetch(image.path, { cache: "no-store" });
  if (!response.ok) {
    throw new Error(`Failed to fetch ${image.filename}`);
  }

  const data = new Uint8Array(await response.arrayBuffer());
  return {
    address: parseAddress(image.address),
    data,
  };
}


async function ensureConnected() {
  if (state.loader) {
    return;
  }
  await connectDevice();
}


function updateProgress(image, written, total) {
  const percent = total === 0 ? 0 : Math.round((written / total) * 100);
  elements.progressBar.value = percent;
  elements.progressLabel.textContent = `Writing ${image.label}`;
  elements.progressDetail.textContent = `${image.filename} · ${percent}%`;
}


async function flash(mode) {
  await ensureConnected();
  if (!state.loader || !state.manifest || state.manifest.status !== "ready") {
    return;
  }

  const isFactory = mode === "factory";
  if (
    isFactory &&
    !window.confirm(
      "Factory reflash will erase the entire chip and then restore bootloader, partitions, firmware, and SPIFFS. Continue?"
    )
  ) {
    return;
  }

  const images = state.manifest.images.filter((image) => (isFactory ? true : image.default));

  setBusy(true);
  elements.progressBar.value = 0;
  elements.progressLabel.textContent = isFactory ? "Preparing factory reflash" : "Preparing routine update";
  elements.progressDetail.textContent = "Downloading image bundle from the current Pages deployment.";
  appendLog(`${isFactory ? "Factory reflash" : "Routine update"} selected.\n`);

  try {
    const fileArray = [];
    for (const image of images) {
      appendLog(`Fetching ${image.filename}...\n`);
      const payload = await fetchImage(image);
      fileArray.push(payload);
    }

    await state.loader.writeFlash({
      fileArray,
      flashMode: state.manifest.flashMode,
      flashFreq: state.manifest.flashFreq,
      flashSize: state.manifest.flashSize,
      eraseAll: isFactory,
      compress: true,
      reportProgress: (fileIndex, written, total) => {
        updateProgress(images[fileIndex], written, total);
      },
    });

    await state.loader.after("hard_reset");
    elements.progressBar.value = 100;
    elements.progressLabel.textContent = isFactory ? "Factory reflash complete" : "Routine update complete";
    elements.progressDetail.textContent = "The StickS3 is rebooting into the flashed firmware.";
    appendLog("Flash completed successfully.\n");
  } catch (error) {
    elements.progressLabel.textContent = "Flash failed";
    elements.progressDetail.textContent = error instanceof Error ? error.message : String(error);
    appendLog(`Flash failed: ${error instanceof Error ? error.message : String(error)}\n`);
  } finally {
    setBusy(false);
    await disconnectDevice({ silent: true });
  }
}


function initialise() {
  if (isSerialSupported()) {
    setPill(elements.serialState, "Web Serial is available");
  } else {
    setPill(elements.serialState, "Use Chrome or Edge with Web Serial enabled");
  }

  renderConnectionState();
  updateButtonState();

  elements.connectButton.addEventListener("click", () => {
    void connectDevice();
  });

  elements.disconnectButton.addEventListener("click", () => {
    void disconnectDevice();
  });

  elements.flashButton.addEventListener("click", () => {
    void flash("update");
  });

  elements.factoryFlashButton.addEventListener("click", () => {
    void flash("factory");
  });

  void loadManifest();
}


initialise();