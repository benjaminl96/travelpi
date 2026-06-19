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
│   └── travel_config.c        # generated locally
├── config/
│   └── trips.example.json
├── assets/
│   ├── maps/
│   ├── photos/
│   └── source/
├── scripts/
│   ├── build_raylib_drm.sh
│   ├── generate_travel_config.py
│   ├── import_trip.py
│   ├── prepare_assets.py
│   └── install_pi_deps.sh
└── systemd/
    └── travelpi.service
```

## Core Loop

- Runs a continuous `SetTargetFPS(60)` render loop with vsync requested.
- Loads one static equirectangular world map texture into VRAM and draws it
  through `Camera2D`.
- Tracks camera target and zoom with framerate-independent exponential LERP.
- Cycles through the compiled `TRAVELPI_LOCATIONS` array indefinitely.
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
`config/trips.json` does not exist.

To create an editable local manifest from the example:

```sh
python3 scripts/bootstrap_config.py
```

## Prepare Assets

The runtime is configured for 1080p. The easiest path from real photos to
runtime is:

```sh
python3 -m pip install -r requirements-dev.txt

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
with up to four photos resident per page. Pages hold for 15 seconds by default
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
assets/source/photos/*.jpg
```

Then run:

```sh
python3 scripts/prepare_assets.py --width 1920
```

The script resizes each configured photo to the exact long-edge size implied by
its `PhotoSpec.scale` at 1920px, with a default minimum long edge of 1280px for
the non-overlapping gallery layout, and writes the prepared files into
`assets/photos/`. It also prepares `assets/maps/world_map.png` and will generate
`assets/maps/world_map.qoi` when `qoiconv` is installed. The app prefers QOI and
falls back to PNG.

## Desktop Build

Install raylib locally, then:

```sh
make
make run
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

## License

This project is licensed under the PolyForm Noncommercial License 1.0.0. Personal,
educational, hobby, and other noncommercial use is allowed; commercial use needs
separate permission from the copyright holder.

## Install Sketch

```sh
sudo mkdir -p /opt/travelpi/bin
sudo cp build/travelpi /opt/travelpi/bin/
sudo cp -R assets /opt/travelpi/
sudo cp systemd/travelpi.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable travelpi.service
sudo systemctl start travelpi.service
```

Make sure the service user belongs to the `video` and `render` groups so it can
open DRM/KMS devices.

## Configure Trips

Edit `config/trips.json`, then regenerate `src/travel_config.c`.

- `geo` is the destination latitude/longitude. It is projected onto the world
  texture with a simple equirectangular transform.
- `pixel_nudge` is an art-direction offset in final map pixels for maps whose
  labels or visual geography need slight alignment correction.
- `caption` is the compact lower-right trip label used during the foreground
  collage beat.
- `close_zoom` controls how far into the region the camera travels.
- `hold_seconds`, `fade_seconds`, `zoom_in_seconds`, and `zoom_out_seconds`
  control the cinematic beat.
- `photos[].path` points at prepared runtime files in `assets/photos/`.
- `PhotoSpec.anchor` is legacy manifest layout data. The current renderer uses
  fixed non-overlapping gallery pages, but the field is still accepted for
  compatibility with older manifests.
