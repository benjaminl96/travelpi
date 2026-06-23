# travelpi

Bare-metal-style Raspberry Pi kiosk scaffold for a C + raylib global travel loop
on Raspberry Pi OS Lite. The Pi build targets raylib's DRM/KMS path, so the app
can render directly to the Linux graphics stack without X11 or Wayland.

## Layout

```text
.
├── Makefile
├── include/
│   ├── app_config.h
│   └── travel_config.h
├── src/
│   ├── main.c
│   ├── travel_config_loader.c
│   └── travel_config.c        # generated locally
├── admin/
│   ├── travelpi_admin.py
│   ├── static/
│   └── templates/
├── config/
│   └── trips.example.json
├── assets/
│   ├── fonts/
│   ├── maps/
│   ├── photos/
│   └── source/
├── scripts/
│   ├── build_raylib_drm.sh
│   ├── build_qoiconv.sh
│   ├── generate_travel_config.py
│   ├── import_trip.py
│   ├── prepare_assets.py
│   └── install_pi_deps.sh
└── systemd/
    ├── travelpi.service
    └── travelpi-admin.service
```

## Core Loop

- Runs a continuous `SetTargetFPS(60)` render loop with vsync requested.
- Loads one static equirectangular world map texture into VRAM and draws it
  through `Camera2D`.
- Tracks camera target and zoom with framerate-independent exponential LERP.
- Loads `config/trips.json` at runtime when present, with the generated
  `TRAVELPI_LOCATIONS` array kept as a build-safe fallback.
- Polls the local trip manifest every two seconds and hot-reloads valid changes
  without restarting the renderer.
- Per location: zoom in, fade a polaroid-style photo collage in, hold, fade the
  collage out, zoom back to the macro world map, advance to the next location.
- Keeps memory pressure low by expecting pre-sized photo assets and only keeping
  the current page plus next page's photos resident.

## Personal Data

This repository tracks code, map assets, and an example trip manifest. Your real
trip manifest and photos stay local:

```text
config/trips.json
src/travel_config.c
assets/photos/*
assets/source/photos/*
```

Those paths are ignored by Git so personal photos and location history do not
get pushed by accident. A fresh clone can still build because `make` generates
`src/travel_config.c` from `config/trips.example.json` when your local
`config/trips.json` does not exist. At runtime, the app prefers the local JSON
manifest and falls back to the generated C config only when the JSON is absent
or invalid.

To create an editable local manifest from the example:

```sh
python3 scripts/bootstrap_config.py
```

## Prepare Assets

The runtime is configured for 1080p. The easiest path from real photos to
runtime is:

```sh
python3 -m pip install -r requirements-dev.txt
./scripts/build_qoiconv.sh

python3 scripts/import_trip.py ~/Pictures/Iceland \
  --name "Iceland Ring Road" \
  --caption "June 2026" \
  --prepare
```

`import_trip.py` scans the folder, reads EXIF GPS/date metadata, computes a trip
center from the median GPS coordinates, copies every readable still image into
`assets/source/photos/`, appends `config/trips.json`, regenerates
`src/travel_config.c`, and optionally runs the 1080p preparation step. At
runtime, each trip is rendered as as many print-style gallery pages as needed,
with up to two photos resident per page. Pages hold for 15 seconds by default
and page-to-page fades are intentionally quick.

Trip names and captions are human-supplied because EXIF reliably gives capture
time and GPS, not a semantic trip title. If exported photos have no GPS, provide
the destination manually:

```sh
python3 scripts/import_trip.py ~/Pictures/Iceland \
  --name "Iceland Ring Road" \
  --caption "June 2026" \
  --lat 64.1466 \
  --lon -21.9426 \
  --prepare
```

Preview without writing:

```sh
python3 scripts/import_trip.py ~/Pictures/Iceland --name "Iceland Ring Road" --dry-run
```

The editable local source of truth is `config/trips.json`. Run this after manual
JSON edits:

```sh
python3 scripts/generate_travel_config.py
python3 scripts/prepare_assets.py --width 1920
```

You can also put original media here directly:

```text
assets/source/maps/world_map.png
assets/source/maps/world_map_highres.tif    # optional, used for close-zoom tiles
assets/source/photos/*.jpg
```

Then run:

```sh
python3 scripts/prepare_assets.py --width 1920
```

The script resizes each configured photo to the exact long-edge size implied by
its `PhotoSpec.scale` at 1920px, with a default minimum long edge of 1600px for
the non-overlapping gallery layout, and writes the prepared files into
`assets/photos/`. When `.asset-cache/bin/qoiconv` exists, it also writes sibling
QOI files for faster runtime decode. It prepares a Pi-friendly base map at
`assets/maps/world_map.png` and cuts `assets/maps/tiles/` into 512px close-zoom
tiles. The app draws the base map for macro views, then loads only visible tiles
near the camera for sharper close zooms. If `assets/source/maps/world_map_highres.*`
exists, tile prep uses it while keeping the base map capped. If Natural Earth
boundary zip files are present locally, map prep overlays country and
state/province borders onto both the base map and high-resolution tiles. QOI
files are generated when `qoiconv` is available; the app falls back to PNG/JPEG.

To convert existing trips to the current two-photo, larger-image layout, keep
your existing `config/trips.json` and source photos in place, then run:

```sh
python3 scripts/prepare_assets.py --photos-only
```

The runtime page split changes automatically because it is controlled by the app
constant, and the prep script regenerates any prepared photo whose long edge is
below the current 1600px target.

## Admin Web UI

Run the local admin surface from any machine on your LAN:

```sh
make admin
```

Then open `http://<pi-hostname-or-ip>:8080`. The admin UI intentionally has no
login; it is meant for a trusted home network, not internet exposure.

The web UI can:

- Create and edit trips in `config/trips.json`.
- Upload photos directly from the browser into `assets/source/photos/`.
- Read EXIF GPS/date metadata where available and compute a trip center.
- Reorder or remove photos from a trip.
- Run `scripts/prepare_assets.py` in a background job and stream job logs.
- Show prep buttons only when the corresponding photo or map assets are stale
  or missing.
- Trigger a best-effort systemd restart if you set `TRAVELPI_RESTART_COMMAND`
  or run the packaged service names.

The display process hot-reloads valid manifest changes automatically. Asset prep
still matters because the renderer expects Pi-friendly runtime images in
`assets/photos/` plus QOI siblings when `qoiconv` is available.

### High-Resolution Map Tiles

The tracked `assets/source/maps/world_map.png` is a modest 4096x2048 source map.
That is enough for macro world views, but close zooms need a larger local source.
For the sharp tile set used during regional zooms, download Natural Earth's
1:10m **Natural Earth II with Shaded Relief and Water** large raster:

```sh
mkdir -p .asset-cache/naturalearth assets/source/maps
curl -L --fail \
  -o .asset-cache/naturalearth/NE2_HR_LC_SR_W.zip \
  https://naciscdn.org/naturalearth/10m/raster/NE2_HR_LC_SR_W.zip

unzip -o .asset-cache/naturalearth/NE2_HR_LC_SR_W.zip \
  NE2_HR_LC_SR_W.tif \
  NE2_HR_LC_SR_W.VERSION.txt \
  NE2_HR_LC_SR_W.README.html \
  -d .asset-cache/naturalearth

mv .asset-cache/naturalearth/NE2_HR_LC_SR_W.tif \
  assets/source/maps/world_map_highres.tif
cp .asset-cache/naturalearth/NE2_HR_LC_SR_W.VERSION.txt \
  assets/source/maps/NATURAL_EARTH_II_HR_VERSION.txt
cp .asset-cache/naturalearth/NE2_HR_LC_SR_W.README.html \
  assets/source/maps/NATURAL_EARTH_II_HR_README.html

./scripts/build_qoiconv.sh
python3 scripts/prepare_assets.py --map-only
```

For country and state/province borders, fetch Natural Earth's 1:10m cultural
boundary lines once, then prepare the map:

```sh
python3 scripts/prepare_assets.py --download-map-boundaries --map-only
```

The boundary zips are cached in `.asset-cache/naturalearth/` and are reused on
later `--map-only` runs.

Expected local sizes are roughly 320 MB for the zip, 677 MB for
`world_map_highres.tif`, and a few hundred MB for the generated 512px tile set,
depending on whether you keep PNG fallbacks alongside QOI files. The
high-resolution source and generated `assets/maps/tiles/` directory are
intentionally ignored by Git. Generate them locally with the commands above, then
sync `assets/maps/tiles/` to the Pi as part of your runtime asset deploy.

## Desktop Build

Install raylib locally, then:

```sh
make
make run
make admin
```

`make run` starts windowed with FPS visible for development. Add `--profile` to
the executable to show frame, update, draw, average, and worst-frame timings.
Use `--start "Trip Name"` to jump directly to a specific manifest trip while
checking imported photos.

## Raspberry Pi OS Lite Build

On the Pi:

```sh
./scripts/install_pi_deps.sh
./scripts/build_raylib_drm.sh
make pi
```

The raylib build script compiles raylib for `PLATFORM_DRM` with OpenGL ES 2.0.
The kiosk executable links against that install.

The default runtime and systemd service are set to `1920x1080`.
The admin service listens on port `8080` by default.

## License

This project is licensed under the PolyForm Noncommercial License 1.0.0. Personal,
educational, hobby, and other noncommercial use is allowed; commercial use needs
separate permission from the copyright holder.

## Install Sketch

```sh
sudo mkdir -p /opt/travelpi/bin
sudo cp build/travelpi /opt/travelpi/bin/
sudo cp -R assets /opt/travelpi/
sudo cp -R admin config scripts /opt/travelpi/
sudo cp systemd/travelpi.service /etc/systemd/system/
sudo cp systemd/travelpi-admin.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable travelpi.service travelpi-admin.service
sudo systemctl start travelpi.service
sudo systemctl start travelpi-admin.service
```

Make sure the service user belongs to the `video` and `render` groups so it can
open DRM/KMS devices.

## Configure Trips

Use the admin UI or edit `config/trips.json` directly. The renderer watches the
JSON file and reloads valid changes while it is running. `src/travel_config.c`
is still generated for fallback builds and compatibility with older workflows.

- `geo` is the destination latitude/longitude. It is projected onto the world
  texture with a simple equirectangular transform.
- `pixel_nudge` is an art-direction offset in final map pixels for maps whose
  labels or visual geography need slight alignment correction.
- `caption` is kept for manifest compatibility; the current lower bar shows a
  right-aligned trip name and, when needed, a slightly smaller right-aligned
  `page N/M` label for multi-page trips.
- `close_zoom` controls how far into the region the camera travels.
- `hold_seconds`, `fade_seconds`, `zoom_in_seconds`, and `zoom_out_seconds`
  control the cinematic beat.
- `photos[].path` points at prepared runtime files in `assets/photos/`.
- `PhotoSpec.anchor` is legacy manifest layout data. The current renderer uses
  fixed non-overlapping gallery pages, but the field is still accepted for
  compatibility with older manifests.
