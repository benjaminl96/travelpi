# Assets

Runtime paths are resolved relative to the working directory by default. Set
`TRAVELPI_ASSET_ROOT=/opt/travelpi` when running from a systemd service.

Expected layout:

```text
assets/
  maps/
    world_map.qoi
    world_map.png
  photos/
    your_trip_01.jpg
    your_trip_02.jpg
    your_trip_03.jpg
```

Original source files can live under `assets/source/`, or you can import a trip
with `scripts/import_trip.py`. If prepared files are missing, the app generates
checker textures so the camera and transition loop remain testable.

Personal trip photos are intentionally ignored by Git. Keep `.gitkeep` files in
place so the empty directories exist in fresh clones.

Memory notes:

- Run `python3 scripts/import_trip.py /path/to/photos --name "Trip Name"
  --prepare` for the normal photo workflow, or run `python3
  scripts/prepare_assets.py --width 1920` after manual manifest edits.
- The included `world_map.png` was prepared from Natural Earth II with Shaded
  Relief, public-domain raster data. The runtime prefers `world_map.qoi` for
  faster decode and falls back to `world_map.png`.
- A 4096x2048 RGBA map consumes about 32 MiB of GPU memory after upload.
- Photos should be prepared before upload. The helper currently uses a default
  minimum 1280px long edge so large non-overlapping gallery cells stay sharp.
- A trip can contain any number of photos. The runtime renders them as pages of
  up to four photos, keeping only the current and next page's textures resident.
