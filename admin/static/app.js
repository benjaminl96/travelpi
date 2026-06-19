let state = { trips: [], jobs: [] };
let selected = 0;
let importFiles = [];
let uploadFiles = [];
let editorDirty = false;

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

function editorHasFocus() {
  return tripForm.contains(document.activeElement);
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
  renderJobs();
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

  if (photos.dirty) reasons.push(`Photos: ${reasonText(photos.reasons)}`);
  if (map.dirty) reasons.push(`Map: ${reasonText(map.reasons)}`);

  prepStatus.textContent = reasons.length ? reasons.join(" · ") : "Assets are current";
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
  (trip.photos || []).forEach((photo, index) => {
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
  const form = new FormData();
  [...files].forEach(file => form.append("photos", file));
  const result = await api(`/api/trips/${selected}/photos`, { method: "POST", body: form });
  showToast(`Uploaded ${result.photo_count} photos. Prepare photos is now available.`);
  editorDirty = false;
  await loadState({ forceEditor: true });
}

async function importTrip() {
  const form = new FormData(importForm);
  importFiles.forEach(file => form.append("photos", file));
  const result = await api("/api/import", { method: "POST", body: form });
  selected = result.index;
  showToast(`Imported ${result.photo_count} photos. Prepare photos is now available.`);
  importFiles = [];
  $("#importUpload").value = "";
  editorDirty = false;
  await loadState({ forceEditor: true });
  showTab("edit");
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
  document.querySelectorAll(".tab").forEach(tab => tab.classList.toggle("active", tab.dataset.tab === name));
  document.querySelectorAll(".panel").forEach(panel => panel.classList.remove("active"));
  $(`#${name}Panel`).classList.add("active");
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
  selected = state.trips.length;
  editorDirty = false;
  renderTrips();
  renderEditor();
};
$("#importTrip").onclick = () => importTrip().catch(err => showToast(err.message));
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
wireDrop("#photoDrop", "#photoUpload", files => uploadToTrip(files).catch(err => showToast(err.message)));
wireDrop("#importDrop", "#importUpload", files => {
  importFiles = files;
  showToast(`${files.length} files ready to import`);
});

loadState().catch(err => showToast(err.message));
setInterval(() => loadState().catch(() => {}), 4000);
