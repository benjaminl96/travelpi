let state = { trips: [], jobs: [] };
let selected = 0;
let importFiles = [];
let importMetadata = null;
let editorDirty = false;
let importBusy = false;
let uploadBusy = false;
let activePanel = "edit";

const PHOTO_LAYOUT = [
  { anchor: [0.30, 0.48], scale: 0.42, rotation: -3.0, drift: 0.4 },
  { anchor: [0.70, 0.48], scale: 0.42, rotation: 3.0, drift: 1.5 },
];
const PREPARED_PHOTO_LONG_EDGE = 1600;

const $ = (selector) => document.querySelector(selector);
const tripList = $("#tripList");
const tripForm = $("#tripForm");
const importForm = $("#importForm");
const photoGrid = $("#photoGrid");
const toast = $("#toast");

function showToast(message) {
  toast.textContent = message;
  toast.classList.add("show");
  setTimeout(() => toast.classList.remove("show"), 2800);
}

async function api(path, options = {}) {
  const response = await fetch(path, options);
  const data = await response.json().catch(() => ({}));
  if (!response.ok) throw new Error(data.error || response.statusText);
  return data;
}

function wait(ms) {
  return new Promise(resolve => setTimeout(resolve, ms));
}

async function retryStep(label, action, retries = 3) {
  let lastError = null;

  for (let attempt = 1; attempt <= retries; attempt++) {
    try {
      return await action(attempt);
    } catch (error) {
      lastError = error;
      if (attempt >= retries) break;
      await wait(700*attempt);
    }
  }

  throw new Error(`${label} failed after ${retries} attempts: ${lastError?.message || lastError}`);
}

function formatBytes(bytes) {
  if (!bytes) return "0 B";
  const units = ["B", "KB", "MB", "GB"];
  let size = bytes;
  let unit = 0;
  while (size >= 1024 && unit < units.length - 1) {
    size /= 1024;
    unit++;
  }
  return `${size >= 10 || unit === 0 ? Math.round(size) : size.toFixed(1)} ${units[unit]}`;
}

function describeFiles(files) {
  const list = [...files];
  const totalBytes = list.reduce((sum, file) => sum + (file.size || 0), 0);
  const plural = list.length === 1 ? "photo" : "photos";
  return `${list.length} ${plural} selected · ${formatBytes(totalBytes)}`;
}

function filePreview(files) {
  const list = [...files].slice(0, 4).map(file => `<li>${escapeHtml(file.name)}</li>`);
  if (files.length > 4) list.push(`<li>+ ${files.length - 4} more</li>`);
  return `<ul class="file-list">${list.join("")}</ul>`;
}

function readAscii(view, offset, length) {
  let text = "";
  for (let i = 0; i < length; i++) text += String.fromCharCode(view.getUint8(offset + i));
  return text.replace(/\0+$/, "");
}

function isTiffHeader(bytes, offset) {
  return (
    (bytes[offset] === 0x49 && bytes[offset + 1] === 0x49 && bytes[offset + 2] === 0x2a && bytes[offset + 3] === 0x00) ||
    (bytes[offset] === 0x4d && bytes[offset + 1] === 0x4d && bytes[offset + 2] === 0x00 && bytes[offset + 3] === 0x2a)
  );
}

function candidateExifViews(buffer) {
  const bytes = new Uint8Array(buffer);
  const views = [];

  if (bytes[0] === 0xff && bytes[1] === 0xd8) {
    let offset = 2;
    while (offset + 4 < bytes.length) {
      if (bytes[offset] !== 0xff) break;
      const marker = bytes[offset + 1];
      const length = (bytes[offset + 2] << 8) + bytes[offset + 3];
      if (marker === 0xe1 && readAscii(new DataView(buffer), offset + 4, 6) === "Exif") {
        views.push(new DataView(buffer, offset + 10, length - 8));
      }
      offset += 2 + length;
    }
  }

  for (let i = 0; i + 10 < bytes.length; i++) {
    if (
      bytes[i] === 0x45 && bytes[i + 1] === 0x78 && bytes[i + 2] === 0x69 &&
      bytes[i + 3] === 0x66 && bytes[i + 4] === 0x00 && bytes[i + 5] === 0x00
    ) {
      views.push(new DataView(buffer, i + 6));
    }

    if (isTiffHeader(bytes, i)) {
      views.push(new DataView(buffer, i));
    }
  }

  return views;
}

function parseExifDate(value) {
  const match = String(value || "").match(/^(\d{4}):(\d{2}):(\d{2})/);
  return match ? `${match[1]}-${match[2]}-${match[3]}` : null;
}

function parseExifView(view) {
  if (!view || view.byteLength < 12) return {};

  const little = readAscii(view, 0, 2) === "II";
  const read16 = offset => view.getUint16(offset, little);
  const read32 = offset => view.getUint32(offset, little);
  const typeSizes = { 1: 1, 2: 1, 3: 2, 4: 4, 5: 8, 7: 1, 9: 4, 10: 8 };

  if (read16(2) !== 42) return {};

  function entryValueOffset(entryOffset) {
    const type = read16(entryOffset + 2);
    const count = read32(entryOffset + 4);
    const bytes = (typeSizes[type] || 1)*count;
    return bytes <= 4 ? entryOffset + 8 : read32(entryOffset + 8);
  }

  function readIFD(offset) {
    if (!offset || offset + 2 > view.byteLength) return new Map();
    const count = read16(offset);
    if (count > 1024) return new Map();
    const tags = new Map();

    for (let i = 0; i < count; i++) {
      const entry = offset + 2 + i*12;
      if (entry + 12 > view.byteLength) break;
      tags.set(read16(entry), {
        type: read16(entry + 2),
        count: read32(entry + 4),
        offset: entryValueOffset(entry),
      });
    }

    return tags;
  }

  function readString(tag) {
    if (!tag || tag.offset + tag.count > view.byteLength) return null;
    return readAscii(view, tag.offset, tag.count);
  }

  function readRational(offset) {
    if (offset + 8 > view.byteLength) return null;
    const numerator = read32(offset);
    const denominator = read32(offset + 4);
    return denominator ? numerator/denominator : null;
  }

  function readRationalArray(tag) {
    if (!tag) return null;
    const values = [];
    for (let i = 0; i < tag.count; i++) {
      const value = readRational(tag.offset + i*8);
      if (value == null) return null;
      values.push(value);
    }
    return values;
  }

  const ifd0 = readIFD(read32(4));
  const exifIFD = readIFD(ifd0.get(0x8769)?.offset);
  const gpsIFD = readIFD(ifd0.get(0x8825)?.offset);
  const date = parseExifDate(
    readString(exifIFD.get(0x9003)) ||
    readString(exifIFD.get(0x9004)) ||
    readString(ifd0.get(0x0132))
  );
  const latRef = readString(gpsIFD.get(1));
  const latDms = readRationalArray(gpsIFD.get(2));
  const lonRef = readString(gpsIFD.get(3));
  const lonDms = readRationalArray(gpsIFD.get(4));
  let latitude = null;
  let longitude = null;

  if (latRef && latDms?.length >= 3 && lonRef && lonDms?.length >= 3) {
    latitude = latDms[0] + latDms[1]/60 + latDms[2]/3600;
    longitude = lonDms[0] + lonDms[1]/60 + lonDms[2]/3600;
    if (latRef.trim() === "S") latitude = -latitude;
    if (lonRef.trim() === "W") longitude = -longitude;
  }

  return { date, latitude, longitude };
}

function hasUsefulMetadata(metadata) {
  return Boolean(metadata.date) || (Number.isFinite(metadata.latitude) && Number.isFinite(metadata.longitude));
}

function formatDateLike(value) {
  if (!value) return null;

  if (value instanceof Date && !Number.isNaN(value.getTime())) {
    const year = value.getFullYear();
    const month = String(value.getMonth() + 1).padStart(2, "0");
    const day = String(value.getDate()).padStart(2, "0");
    return `${year}-${month}-${day}`;
  }

  const match = String(value).match(/^(\d{4})[:-](\d{2})[:-](\d{2})/);
  return match ? `${match[1]}-${match[2]}-${match[3]}` : null;
}

function rationalValue(value) {
  if (Array.isArray(value) && value.length >= 2) return Number(value[0]) / Number(value[1]);
  if (value && typeof value === "object" && "numerator" in value && "denominator" in value) {
    return Number(value.numerator) / Number(value.denominator);
  }
  return Number(value);
}

function dmsToDecimal(value, ref) {
  if (!Array.isArray(value) || value.length < 3) return null;
  const degrees = rationalValue(value[0]);
  const minutes = rationalValue(value[1]);
  const seconds = rationalValue(value[2]);
  if (![degrees, minutes, seconds].every(Number.isFinite)) return null;
  let decimal = degrees + minutes/60 + seconds/3600;
  if (String(ref || "").trim().toUpperCase().match(/^[SW]$/)) decimal = -decimal;
  return decimal;
}

function normalizeCoordinate(primary, dms, ref) {
  const value = Number(primary);
  if (Number.isFinite(value)) return value;
  return dmsToDecimal(dms, ref);
}

function normalizeLibraryMetadata(raw) {
  if (!raw || typeof raw !== "object") return {};
  const latitude = normalizeCoordinate(raw.latitude, raw.GPSLatitude, raw.GPSLatitudeRef);
  const longitude = normalizeCoordinate(raw.longitude, raw.GPSLongitude, raw.GPSLongitudeRef);
  const date = formatDateLike(
    raw.DateTimeOriginal ||
    raw.CreateDate ||
    raw.ModifyDate ||
    raw.DateCreated ||
    raw.DateTime ||
    raw.GPSDateStamp
  );

  return {
    date,
    latitude: Number.isFinite(latitude) ? latitude : null,
    longitude: Number.isFinite(longitude) ? longitude : null,
  };
}

function parseExifMetadata(buffer) {
  for (const view of candidateExifViews(buffer)) {
    try {
      const metadata = parseExifView(view);
      if (hasUsefulMetadata(metadata)) return metadata;
    } catch {
      // Keep trying; HEIC containers often contain Exif-looking bytes outside the metadata payload.
    }
  }

  return {};
}

async function extractPhotoMetadata(file) {
  if (globalThis.exifr?.parse) {
    try {
      const metadata = normalizeLibraryMetadata(await globalThis.exifr.parse(file, {
        tiff: true,
        ifd0: true,
        exif: true,
        gps: true,
        xmp: true,
        mergeOutput: true,
        reviveValues: true,
      }));
      if (hasUsefulMetadata(metadata)) return metadata;
    } catch (error) {
      console.warn(`Could not read metadata with exifr for ${file.name}`, error);
    }
  }

  try {
    return parseExifMetadata(await file.arrayBuffer());
  } catch {
    return {};
  }
}

function median(values) {
  const sorted = values.filter(value => Number.isFinite(value)).sort((a, b) => a - b);
  if (!sorted.length) return null;
  const middle = Math.floor(sorted.length/2);
  return sorted.length % 2 ? sorted[middle] : (sorted[middle - 1] + sorted[middle])/2;
}

async function extractTripMetadata(files) {
  const items = [];

  for (const file of files) {
    items.push({
      name: file.name,
      preparedName: preparedPhotoName(file),
      ...(await extractPhotoMetadata(file)),
    });
  }

  const dates = items.map(item => item.date).filter(Boolean).sort();
  const gpsItems = items.filter(item => Number.isFinite(item.latitude) && Number.isFinite(item.longitude));

  return {
    gpsCount: gpsItems.length,
    latitude: median(gpsItems.map(item => item.latitude)),
    longitude: median(gpsItems.map(item => item.longitude)),
    startDate: dates[0] || "",
    endDate: dates[dates.length - 1] || "",
    items,
  };
}

function loadImageElement(file) {
  return new Promise((resolve, reject) => {
    const url = URL.createObjectURL(file);
    const image = new Image();
    const name = file.name || "photo";

    image.onload = () => {
      URL.revokeObjectURL(url);
      resolve(image);
    };
    image.onerror = () => {
      URL.revokeObjectURL(url);
      reject(new Error(`Could not decode ${name}`));
    };
    image.src = url;
  });
}

function canvasToBlob(canvas, type, quality) {
  return new Promise((resolve, reject) => {
    canvas.toBlob(blob => {
      if (blob) resolve(blob);
      else reject(new Error("Could not prepare photo"));
    }, type, quality);
  });
}

function preparedPhotoName(file) {
  return file.name.replace(/\.[^.]+$/, "") + ".png";
}

function isHeicFile(file) {
  return /hei[cf]$/i.test(file.name || "") || /hei[cf]/i.test(file.type || "");
}

async function hasHeicSignature(file) {
  if (!isHeicFile(file)) return false;

  const header = new Uint8Array(await file.slice(0, 16).arrayBuffer());
  const brand = String.fromCharCode(
    header[4] || 0,
    header[5] || 0,
    header[6] || 0,
    header[7] || 0
  );

  return brand === "ftyp";
}

async function canvasSourceForFile(file, quality) {
  if (!(await hasHeicSignature(file))) return file;

  if (!globalThis.HeicTo) {
    throw new Error("HEIC decoding is not available in this browser session. Refresh the admin page and try again.");
  }

  try {
    const converted = await globalThis.HeicTo({
      blob: file,
      type: "image/jpeg",
      quality,
    });
    return Array.isArray(converted) ? converted[0] : converted;
  } catch (error) {
    throw new Error(`Could not decode HEIC ${file.name}: ${error?.message || error}`);
  }
}

function photoProgressLabel(action, file, index, total) {
  return `<strong>${escapeHtml(action)} photo ${index + 1} of ${total}</strong><span>${escapeHtml(file.name)} · originals stay on this device.</span>`;
}

function photoDefinition(index) {
  const layout = PHOTO_LAYOUT[index % PHOTO_LAYOUT.length];
  return {
    anchor: [...layout.anchor],
    scale: layout.scale,
    rotation: layout.rotation,
    drift: layout.drift,
  };
}

function numericFormValue(form, name, fallback) {
  const value = form.elements[name]?.value;
  return value === "" || value == null ? fallback : Number(value);
}

function browserTripDefinition(latitude, longitude, metadata) {
  const fd = new FormData(importForm);
  const name = String(fd.get("name") || "").trim();
  const caption = String(fd.get("caption") || "").trim() || name;
  const trip = {
    name,
    caption,
    geo: {
      latitude: Number(latitude.toFixed(6)),
      longitude: Number(longitude.toFixed(6)),
    },
    pixel_nudge: [0.0, 0.0],
    close_zoom: numericFormValue(importForm, "close_zoom", 10),
    zoom_in_seconds: 2.35,
    hold_seconds: numericFormValue(importForm, "hold_seconds", 15),
    fade_seconds: numericFormValue(importForm, "fade_seconds", 0.45),
    zoom_out_seconds: 2.05,
    photos: importFiles.map((_, index) => photoDefinition(index)),
  };

  if (metadata.startDate) {
    trip.start_date = metadata.startDate;
    trip.end_date = metadata.endDate || metadata.startDate;
  }

  return trip;
}

async function preparePhotoForUploadWithProgress(file, index, total, progress) {
  progress(photoProgressLabel("Checking", file, index, total));
  const isHeic = await hasHeicSignature(file);

  if (isHeic) {
    progress(photoProgressLabel("Converting HEIC", file, index, total));
  } else {
    progress(photoProgressLabel("Decoding", file, index, total));
  }

  const source = isHeic ? await canvasSourceForFile(file, 0.88) : file;
  const image = await loadImageElement(source);
  const sourceWidth = image.naturalWidth || image.width;
  const sourceHeight = image.naturalHeight || image.height;
  const currentLongEdge = Math.max(sourceWidth, sourceHeight);
  const ratio = currentLongEdge > PREPARED_PHOTO_LONG_EDGE ? PREPARED_PHOTO_LONG_EDGE/currentLongEdge : 1;
  const width = Math.max(1, Math.round(sourceWidth*ratio));
  const height = Math.max(1, Math.round(sourceHeight*ratio));
  const canvas = document.createElement("canvas");
  const context = canvas.getContext("2d");

  progress(photoProgressLabel("Resizing", file, index, total));
  canvas.width = width;
  canvas.height = height;
  context.drawImage(image, 0, 0, width, height);

  progress(photoProgressLabel("Saving display copy", file, index, total));
  const blob = await canvasToBlob(canvas, "image/png", 0.88);
  return new File([blob], preparedPhotoName(file), { type: "image/png", lastModified: file.lastModified });
}

async function preparePhotosForUpload(files, progress = () => {}) {
  const prepared = [];
  const list = [...files];

  for (const [index, file] of list.entries()) {
    prepared.push(await preparePhotoForUploadWithProgress(file, index, list.length, progress));
  }

  return prepared;
}

function editorHasFocus() {
  return tripForm.contains(document.activeElement) || importForm.contains(document.activeElement);
}

function shouldPreserveEditor() {
  return editorDirty || editorHasFocus();
}

async function loadState(options = {}) {
  const nextState = await api("/api/state");
  const preserveEditor = !options.forceEditor && shouldPreserveEditor();
  state = nextState;
  $("#manifestPath").textContent = state.manifest;
  if (selected >= state.trips.length && !preserveEditor) selected = Math.max(0, state.trips.length - 1);
  renderTrips();
  if (!preserveEditor) renderEditor();
  renderPrepStatus();
  renderWorkflowStatus();
  renderJobs();
  updateImportControls();
}

function reasonText(reasons) {
  if (!reasons || reasons.length === 0) return "";
  return reasons.length === 1 ? reasons[0] : `${reasons[0]} + ${reasons.length - 1} more`;
}

function renderPrepStatus() {
  const assets = state.assets || {};
  const photos = assets.photos || {};
  const map = assets.map || {};
  const preparePhotos = $("#preparePhotos");
  const prepareAll = $("#prepareAll");
  const prepStatus = $("#prepStatus");
  const reasons = [];

  preparePhotos.hidden = !photos.dirty;
  prepareAll.hidden = !map.dirty;
  preparePhotos.textContent = "Prepare photos now";
  prepareAll.textContent = "Prepare map now";

  if (photos.dirty) reasons.push(`Photos: ${reasonText(photos.reasons)}`);
  if (map.dirty) reasons.push(`Map: ${reasonText(map.reasons)}`);

  prepStatus.textContent = reasons.length ? reasons.join(" · ") : "Assets ready";
}

function renderWorkflowStatus() {
  const banner = $("#workflowStatus");
  const runningJob = (state.jobs || []).find(job => job.status === "running");
  const photosDirty = state.assets?.photos?.dirty;
  const mapDirty = state.assets?.map?.dirty;

  banner.hidden = false;
  banner.className = "workflow-status";

  if (runningJob) {
    banner.classList.add("running");
    banner.innerHTML = `<strong>${escapeHtml(runningJob.label)}</strong><span>Job #${runningJob.id} is running. Check Jobs for live output.</span>`;
    return;
  }

  if (photosDirty) {
    banner.classList.add("needs-work");
    banner.innerHTML = `<strong>Photos need preparing</strong><span>${escapeHtml(reasonText(state.assets.photos.reasons))}</span>`;
    return;
  }

  if (mapDirty) {
    banner.classList.add("needs-work");
    banner.innerHTML = `<strong>Map needs preparing</strong><span>${escapeHtml(reasonText(state.assets.map.reasons))}</span>`;
    return;
  }

  banner.hidden = true;
}

function tripDate(trip) {
  if (trip.start_date && trip.end_date && trip.start_date !== trip.end_date) return `${trip.start_date} - ${trip.end_date}`;
  return trip.start_date || "";
}

function renderTrips() {
  tripList.innerHTML = "";
  state.trips.forEach((trip, index) => {
    const button = document.createElement("button");
    button.className = `trip-item ${index === selected ? "active" : ""}`;
    button.innerHTML = `<strong>${escapeHtml(trip.name)}</strong><small>${trip.photos?.length || 0} photos ${escapeHtml(tripDate(trip))}</small>`;
    button.onclick = () => {
      selected = index;
      editorDirty = false;
      renderTrips();
      renderEditor();
      showTab("edit");
    };
    tripList.appendChild(button);
  });
}

function currentTrip() {
  return state.trips[selected] || blankTrip();
}

function blankTrip() {
  return {
    name: "",
    caption: "",
    geo: { latitude: "", longitude: "" },
    close_zoom: 10,
    hold_seconds: 15,
    fade_seconds: 0.45,
    zoom_in_seconds: 2.35,
    zoom_out_seconds: 2.05,
    photos: []
  };
}

function setFormValue(form, name, value) {
  form.elements[name].value = value ?? "";
}

function renderEditor() {
  const trip = currentTrip();
  setFormValue(tripForm, "name", trip.name);
  setFormValue(tripForm, "caption", trip.caption);
  setFormValue(tripForm, "latitude", trip.geo?.latitude);
  setFormValue(tripForm, "longitude", trip.geo?.longitude);
  setFormValue(tripForm, "start_date", trip.start_date);
  setFormValue(tripForm, "end_date", trip.end_date);
  setFormValue(tripForm, "date_label", trip.date_label);
  setFormValue(tripForm, "close_zoom", trip.close_zoom ?? 10);
  setFormValue(tripForm, "hold_seconds", trip.hold_seconds ?? 15);
  setFormValue(tripForm, "fade_seconds", trip.fade_seconds ?? 0.45);
  setFormValue(tripForm, "zoom_in_seconds", trip.zoom_in_seconds ?? 2.35);
  setFormValue(tripForm, "zoom_out_seconds", trip.zoom_out_seconds ?? 2.05);

  photoGrid.innerHTML = "";
  const photos = trip.photos || [];
  renderPhotoUploadStatus();

  if (photos.length === 0) {
    photoGrid.innerHTML = `<div class="empty-state">No photos yet.</div>`;
    return;
  }

  photos.forEach((photo, index) => {
    const name = photo.path.split("/").pop();
    const card = document.createElement("article");
    card.className = "photo-card";
    card.innerHTML = `
      <img src="/photos/${encodeURIComponent(name)}" alt="">
      <div class="meta">
        <span>${escapeHtml(photo.path)}</span>
        <div class="row">
          <button data-action="up">Up</button>
          <button data-action="down">Down</button>
          <button data-action="remove">Remove</button>
        </div>
      </div>`;
    card.querySelector('[data-action="up"]').onclick = () => movePhoto(index, -1);
    card.querySelector('[data-action="down"]').onclick = () => movePhoto(index, 1);
    card.querySelector('[data-action="remove"]').onclick = () => removePhoto(index);
    photoGrid.appendChild(card);
  });
}

function tripFromForm() {
  const fd = new FormData(tripForm);
  return {
    ...currentTrip(),
    name: fd.get("name"),
    caption: fd.get("caption"),
    geo: {
      latitude: fd.get("latitude"),
      longitude: fd.get("longitude")
    },
    start_date: fd.get("start_date"),
    end_date: fd.get("end_date"),
    date_label: fd.get("date_label"),
    close_zoom: fd.get("close_zoom"),
    hold_seconds: fd.get("hold_seconds"),
    fade_seconds: fd.get("fade_seconds"),
    zoom_in_seconds: fd.get("zoom_in_seconds"),
    zoom_out_seconds: fd.get("zoom_out_seconds"),
    photos: currentTrip().photos || []
  };
}

async function saveTrip() {
  const trip = tripFromForm();
  if (state.trips[selected]) {
    await api(`/api/trips/${selected}`, {
      method: "PUT",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ trip })
    });
  } else {
    const result = await api("/api/trips", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ trip })
    });
    selected = result.index;
  }
  editorDirty = false;
  showToast("Trip saved");
  await loadState({ forceEditor: true });
}

async function deleteTrip() {
  if (!state.trips[selected]) return;
  if (!confirm(`Delete ${state.trips[selected].name}?`)) return;
  await api(`/api/trips/${selected}`, { method: "DELETE" });
  selected = 0;
  editorDirty = false;
  showToast("Trip deleted");
  await loadState({ forceEditor: true });
}

async function saveCurrentPhotos(photos) {
  const trip = { ...tripFromForm(), photos };
  await api(`/api/trips/${selected}`, {
    method: "PUT",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ trip })
  });
  editorDirty = false;
  await loadState({ forceEditor: true });
}

async function startPreparePhotos(context) {
  const result = await api("/api/prepare", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ mode: "photos" })
  });
  showToast(`${context} Preparing photos in job #${result.job}.`);
  await loadState();
  return result;
}

async function finishPhotoChange(result, context) {
  editorDirty = false;
  await loadState({ forceEditor: true });
  showToast(`${context} Ready for display.`);
}

async function uploadPreparedPhoto(tripIndex, file, definition, progressLabel) {
  return retryStep(progressLabel, async attempt => {
    const form = new FormData();
    form.append("browser_prepared", "1");
    form.append("photo_definitions", JSON.stringify([definition]));
    form.append("photos", file);

    renderUploadProgress(`<strong>${escapeHtml(progressLabel)}</strong><span>Attempt ${attempt} of 3. Only this prepared display copy is being sent.</span>`);
    return api(`/api/trips/${tripIndex}/photos`, { method: "POST", body: form });
  });
}

async function uploadPreparedPhotosSequentially(tripIndex, preparedFiles, photoDefinitions, context) {
  let currentIndex = tripIndex;

  for (let index = 0; index < preparedFiles.length; index++) {
    const label = `${context} photo ${index + 1} of ${preparedFiles.length}`;
    const result = await uploadPreparedPhoto(currentIndex, preparedFiles[index], photoDefinitions[index], label);
    if (Number.isInteger(result.index)) currentIndex = result.index;
  }

  return currentIndex;
}

function renderPhotoUploadStatus(message) {
  const status = $("#photoUploadStatus");
  if (!status) return;

  if (message) {
    status.innerHTML = message;
    return;
  }

  const trip = currentTrip();
  if (!state.trips[selected]) {
    status.innerHTML = `<strong>No trip selected</strong><span>Save or select a trip before adding photos.</span>`;
    return;
  }

  const count = trip.photos?.length || 0;
  status.innerHTML = `<strong>${count} ${count === 1 ? "photo" : "photos"} in ${escapeHtml(trip.name)}</strong><span>Your browser prepares display copies before upload; originals stay on this device.</span>`;
}

function renderImportProgress(message) {
  const summary = $("#importSummary");
  const readiness = $("#importReadiness");
  if (!summary || !readiness) return;

  summary.innerHTML = message;
  readiness.textContent = "Keep this page open until the upload finishes.";
}

function renderUploadProgress(message) {
  renderPhotoUploadStatus(message);
  if (importBusy) renderImportProgress(message);
}

function movePhoto(index, direction) {
  const photos = [...(currentTrip().photos || [])];
  const next = index + direction;
  if (next < 0 || next >= photos.length) return;
  [photos[index], photos[next]] = [photos[next], photos[index]];
  saveCurrentPhotos(photos).then(() => showToast("Photo order saved")).catch(err => showToast(err.message));
}

function removePhoto(index) {
  const photos = [...(currentTrip().photos || [])];
  photos.splice(index, 1);
  saveCurrentPhotos(photos).then(() => showToast("Photo removed")).catch(err => showToast(err.message));
}

async function uploadToTrip(files) {
  if (!state.trips[selected]) throw new Error("Select a trip first.");
  if (!files.length) throw new Error("Choose at least one image.");

  uploadBusy = true;
  renderPhotoUploadStatus(`<strong>Preparing ${files.length} ${files.length === 1 ? "photo" : "photos"} in this browser...</strong><span>Keep this page open until the upload finishes.</span>`);
  const originalFiles = [...files];

  try {
    const preparedFiles = await preparePhotosForUpload(originalFiles, renderUploadProgress);
    const existingCount = currentTrip().photos?.length || 0;
    const photoDefinitions = originalFiles.map((_, index) => photoDefinition(existingCount + index));

    selected = await uploadPreparedPhotosSequentially(selected, preparedFiles, photoDefinitions, "Uploading");
    await finishPhotoChange({ photo_count: preparedFiles.length }, `Uploaded ${preparedFiles.length} ${preparedFiles.length === 1 ? "photo." : "photos."}`);
  } finally {
    uploadBusy = false;
    renderPhotoUploadStatus();
    $("#photoUpload").value = "";
  }
}

async function importTrip() {
  if (importBusy) return;
  if (!importFiles.length) throw new Error("Choose photos first.");

  const latInput = importForm.elements.lat.value;
  const lonInput = importForm.elements.lon.value;
  const metadata = importMetadata || await extractTripMetadata(importFiles);
  const latitude = latInput ? Number(latInput) : metadata.latitude;
  const longitude = lonInput ? Number(lonInput) : metadata.longitude;

  if (!Number.isFinite(latitude) || !Number.isFinite(longitude)) {
    throw new Error("No GPS found. Enter latitude and longitude to create this trip.");
  }

  importBusy = true;
  updateImportControls();

  try {
    renderUploadProgress(`<strong>Preparing ${importFiles.length} ${importFiles.length === 1 ? "photo" : "photos"} in this browser...</strong><span>Originals stay on this device.</span>`);
    const tripDefinition = browserTripDefinition(latitude, longitude, metadata);
    const preparedFiles = await preparePhotosForUpload(importFiles, renderUploadProgress);
    const photoDefinitions = tripDefinition.photos || [];
    const emptyTrip = { ...tripDefinition, photos: [] };

    renderUploadProgress(`<strong>Creating trip...</strong><span>The Pi gets the trip first, then each prepared photo uploads separately.</span>`);
    const result = await retryStep("Create trip", () => api("/api/trips", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ trip: emptyTrip })
    }));

    selected = result.index;
    selected = await uploadPreparedPhotosSequentially(selected, preparedFiles, photoDefinitions, "Uploading new trip");
    importFiles = [];
    $("#importUpload").value = "";
    editorDirty = false;
    await loadState({ forceEditor: true });
    showTab("edit");
    await finishPhotoChange(result, `Created trip with ${preparedFiles.length} ${preparedFiles.length === 1 ? "photo." : "photos."}`);
  } finally {
    importBusy = false;
    updateImportControls();
  }
}

function renderJobs() {
  const jobs = $("#jobs");
  jobs.innerHTML = "";
  state.jobs.forEach(job => {
    const item = document.createElement("article");
    item.className = "job";
    item.innerHTML = `
      <header><strong>#${job.id} ${escapeHtml(job.label)}</strong><span>${escapeHtml(job.status)}</span></header>
      <pre>${escapeHtml((job.log || []).join("\\n"))}</pre>`;
    jobs.appendChild(item);
  });
}

function showTab(name) {
  activePanel = name;
  document.querySelectorAll(".tab").forEach(tab => tab.classList.toggle("active", tab.dataset.tab === name));
  document.querySelectorAll(".panel").forEach(panel => panel.classList.remove("active"));
  $(`#${name}Panel`).classList.add("active");
  $("#newTrip").classList.toggle("active", name === "import");
  updateImportControls();
}

function wireDrop(zoneSelector, inputSelector, callback) {
  const zone = $(zoneSelector);
  const input = $(inputSelector);
  input.onchange = () => callback([...input.files]);
  ["dragenter", "dragover"].forEach(event => zone.addEventListener(event, e => {
    e.preventDefault();
    zone.classList.add("drag");
  }));
  ["dragleave", "drop"].forEach(event => zone.addEventListener(event, e => {
    e.preventDefault();
    zone.classList.remove("drag");
  }));
  zone.addEventListener("drop", e => callback([...e.dataTransfer.files]));
}

function setImportFiles(files) {
  importFiles = [...files];
  importMetadata = null;
  $("#importUpload").value = "";
  updateImportControls();
  if (importFiles.length) showToast(`${importFiles.length} ${importFiles.length === 1 ? "photo" : "photos"} selected`);
  extractTripMetadata(importFiles).then(metadata => {
    if (importFiles.length === files.length) {
      importMetadata = metadata;
      updateImportControls();
    }
  });
}

function resetNewTripWorkflow() {
  importFiles = [];
  importMetadata = null;
  importBusy = false;
  importForm.reset();
  setFormValue(importForm, "close_zoom", 10);
  setFormValue(importForm, "hold_seconds", 15);
  setFormValue(importForm, "fade_seconds", 0.45);
  $("#importUpload").value = "";
  updateImportControls();
}

function updateImportControls() {
  const summary = $("#importSummary");
  const readiness = $("#importReadiness");
  const importButton = $("#importTrip");
  const clearButton = $("#clearImport");
  if (!summary || !readiness || !importButton || !clearButton) return;

  const nameReady = Boolean(importForm.elements.name.value.trim());
  const filesReady = importFiles.length > 0;
  const hasManualCoordinates = importForm.elements.lat.value && importForm.elements.lon.value;
  const hasBrowserGps = Number.isFinite(importMetadata?.latitude) && Number.isFinite(importMetadata?.longitude);
  const coordinateHint = hasManualCoordinates
    ? "Coordinates entered."
    : hasBrowserGps
      ? `GPS found in ${importMetadata.gpsCount} ${importMetadata.gpsCount === 1 ? "photo" : "photos"}.`
      : "Photos need GPS EXIF, or enter latitude and longitude.";

  clearButton.hidden = !filesReady || importBusy;
  importButton.disabled = importBusy || !nameReady || !filesReady;

  if (importBusy) {
    readiness.textContent = "Preparing photos in this browser...";
    importButton.textContent = "Creating...";
    summary.innerHTML = `<strong>${describeFiles(importFiles)}</strong><span>Only browser-prepared display copies will be uploaded.</span>`;
    return;
  }

  if (!filesReady) {
    readiness.textContent = nameReady ? "Add photos to continue." : "Name the trip and add photos to continue.";
    importButton.textContent = "Choose photos first";
    summary.innerHTML = `<strong>No photos selected</strong><span>Choose the photos for this trip. Originals stay on this device.</span>`;
    return;
  }

  if (!nameReady) {
    readiness.textContent = "Name this trip to continue.";
    importButton.textContent = "Name trip first";
    summary.innerHTML = `<strong>${describeFiles(importFiles)}</strong><span>${coordinateHint}</span>${filePreview(importFiles)}`;
    return;
  }

  readiness.textContent = `Ready to create trip with ${importFiles.length} ${importFiles.length === 1 ? "photo" : "photos"}.`;
  importButton.textContent = `Create trip & prepare ${importFiles.length} ${importFiles.length === 1 ? "photo" : "photos"}`;
  summary.innerHTML = `<strong>${describeFiles(importFiles)}</strong><span>${coordinateHint} Originals stay on this device.</span>${filePreview(importFiles)}`;
}

function escapeHtml(value) {
  return String(value ?? "").replace(/[&<>"']/g, ch => ({
    "&": "&amp;",
    "<": "&lt;",
    ">": "&gt;",
    '"': "&quot;",
    "'": "&#39;"
  }[ch]));
}

$("#saveTrip").onclick = () => saveTrip().catch(err => showToast(err.message));
$("#deleteTrip").onclick = () => deleteTrip().catch(err => showToast(err.message));
$("#newTrip").onclick = () => {
  editorDirty = false;
  resetNewTripWorkflow();
  renderTrips();
  showTab("import");
};
$("#importTrip").onclick = () => importTrip().catch(err => showToast(err.message));
$("#clearImport").onclick = () => setImportFiles([]);
$("#preparePhotos").onclick = () => api("/api/prepare", {
  method: "POST",
  headers: { "Content-Type": "application/json" },
  body: JSON.stringify({ mode: "photos" })
}).then(result => { showToast(`Prep job #${result.job}`); loadState(); }).catch(err => showToast(err.message));
$("#prepareAll").onclick = () => api("/api/prepare", {
  method: "POST",
  headers: { "Content-Type": "application/json" },
  body: JSON.stringify({ mode: "all" })
}).then(result => { showToast(`Prep job #${result.job}`); loadState(); }).catch(err => showToast(err.message));
$("#restartDisplay").onclick = () => api("/api/restart", {
  method: "POST"
}).then(result => { showToast(`Restart job #${result.job}`); loadState(); }).catch(err => showToast(err.message));

document.querySelectorAll(".tab").forEach(tab => tab.onclick = () => showTab(tab.dataset.tab));
tripForm.addEventListener("input", () => { editorDirty = true; });
importForm.addEventListener("input", updateImportControls);
wireDrop("#photoDrop", "#photoUpload", files => uploadToTrip(files).catch(err => showToast(err.message)));
wireDrop("#importDrop", "#importUpload", setImportFiles);

loadState().catch(err => showToast(err.message));
setInterval(() => loadState().catch(() => {}), 4000);
