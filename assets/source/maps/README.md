# World Map Source

`world_map.png` was prepared from Natural Earth II with Shaded Relief,
medium-size raster dataset version 3.2.0.

Natural Earth raster and vector map data are public domain. Attribution is not
required, but the suggested credit is:

```text
Made with Natural Earth. Free vector and raster map data @ naturalearthdata.com.
```

The prepared project base asset is capped at 4096px wide for the kiosk runtime.
For sharper close zooms, add an optional `world_map_highres.*` here; asset prep
will keep the base map Pi-friendly while cutting close-zoom tiles from the
higher-resolution source.

## Optional High-Resolution Source

Use Natural Earth's 1:10m Natural Earth II large raster for close-zoom tiles:

```text
https://naciscdn.org/naturalearth/10m/raster/NE2_HR_LC_SR_W.zip
```

Extract `NE2_HR_LC_SR_W.tif` from that archive and place it here as:

```text
assets/source/maps/world_map_highres.tif
```

Then regenerate map assets:

```sh
./scripts/build_qoiconv.sh
python3 scripts/prepare_assets.py --map-only
```

The high-resolution source is intentionally ignored by Git because it is several
hundred MB. The generated runtime tiles live in `assets/maps/tiles/` and are
also ignored by Git; generate them locally and sync them to the Pi with the rest
of the runtime assets. QOI files are preferred at runtime, with PNG fallback
available for development.
