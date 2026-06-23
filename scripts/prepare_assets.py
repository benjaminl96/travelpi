#!/usr/bin/env python3
import argparse
import json
import re
import shutil
import subprocess
import struct
import urllib.request
import zipfile
from pathlib import Path

try:
    from PIL import Image, ImageEnhance, ImageFilter, ImageOps
except ModuleNotFoundError as exc:
    raise SystemExit(
        "prepare_assets.py requires Pillow. Install it with "
        "`python3 -m pip install Pillow` on desktop, or run "
        "`./scripts/install_pi_deps.sh` on Raspberry Pi OS."
    ) from exc

Image.MAX_IMAGE_PIXELS = None


PHOTO_PATTERN = re.compile(
    r'\{\s*"(?P<path>assets/photos/[^"]+)"\s*,\s*'
    r'\{\s*[-0-9.]+f?\s*,\s*[-0-9.]+f?\s*\}\s*,\s*'
    r'(?P<scale>[-0-9.]+)f?'
)
BOUNDARY_SOURCES = {
    "country": {
        "archive": "ne_10m_admin_0_boundary_lines_land.zip",
        "shape": "ne_10m_admin_0_boundary_lines_land.shp",
        "url": "https://naturalearth.s3.amazonaws.com/10m_cultural/ne_10m_admin_0_boundary_lines_land.zip",
    },
    "state": {
        "archive": "ne_10m_admin_1_states_provinces_lines.zip",
        "shape": "ne_10m_admin_1_states_provinces_lines.shp",
        "url": "https://naturalearth.s3.amazonaws.com/10m_cultural/ne_10m_admin_1_states_provinces_lines.zip",
    },
}


def repo_root() -> Path:
    return Path(__file__).resolve().parents[1]


def configured_photos_from_manifest(manifest_path: Path):
    manifest = json.loads(manifest_path.read_text())
    seen = set()

    for trip in manifest.get("trips", []):
        for photo in trip.get("photos", []):
            rel = photo["path"]
            scale = float(photo["scale"])

            if rel in seen:
                continue

            seen.add(rel)
            yield rel, scale


def configured_photos_from_c(config_path: Path):
    text = config_path.read_text()
    seen = set()

    for match in PHOTO_PATTERN.finditer(text):
        rel = match.group("path")
        scale = float(match.group("scale"))

        if rel in seen:
            continue

        seen.add(rel)
        yield rel, scale


def configured_photos(root: Path):
    manifest_path = root / "config/trips.json"
    example_manifest_path = root / "config/trips.example.json"

    if manifest_path.exists():
        yield from configured_photos_from_manifest(manifest_path)
    elif example_manifest_path.exists():
        yield from configured_photos_from_manifest(example_manifest_path)
    else:
        yield from configured_photos_from_c(root / "src/travel_config.c")


def file_mtime(path: Path) -> float:
    try:
        return path.stat().st_mtime
    except FileNotFoundError:
        return 0.0


def photo_needs_prepare(source: Path, dest: Path, long_edge: int) -> bool:
    if not dest.exists():
        return True

    try:
        if source.resolve() == dest.resolve():
            return False
    except FileNotFoundError:
        return True

    if file_mtime(source) > file_mtime(dest):
        return True

    try:
        with Image.open(dest) as image:
            return max(image.size) < long_edge
    except OSError:
        return True


def save_prepared_photo(source: Path, dest: Path, long_edge: int, quality: int):
    with Image.open(source) as image:
        image.draft("RGB", (long_edge * 2, long_edge * 2))
        image = ImageOps.exif_transpose(image).convert("RGB")
        image.load()
        width, height = image.size
        current_long_edge = max(width, height)

        if current_long_edge != long_edge:
            ratio = long_edge / float(current_long_edge)
            resized = (max(1, round(width * ratio)), max(1, round(height * ratio)))
            image = image.resize(resized, Image.Resampling.LANCZOS)

        dest.parent.mkdir(parents=True, exist_ok=True)
        output = dest

        if source.resolve() == dest.resolve():
            output = dest.with_name(f"{dest.stem}.tmp{dest.suffix}")

        image.save(output, quality=quality, optimize=True, progressive=False)

        if output != dest:
            output.replace(dest)


def prepare_photos(root: Path, width: int, quality: int, photo_long_edge: int):
    source_root = root / "assets/source/photos"
    fallback_root = root / "assets/photos"
    qoiconv = find_qoiconv(root)

    for rel, scale in configured_photos(root):
        dest = root / rel
        source = source_root / Path(rel).name

        if not source.exists() and fallback_root.joinpath(Path(rel).name).exists():
            source = fallback_root / Path(rel).name

        if not source.exists():
            print(f"missing photo source: {source_root / Path(rel).name}")
            continue

        long_edge = max(1, round(width * scale), photo_long_edge)

        if not photo_needs_prepare(source, dest, long_edge):
            continue

        save_prepared_photo(source, dest, long_edge, quality)
        print(f"photo {dest.relative_to(root)} long_edge={long_edge}")

        qoi_dest = dest.with_suffix(".qoi")
        if save_qoi_if_available(dest, qoi_dest, qoiconv):
            print(f"photo {qoi_dest.relative_to(root)}")


def save_qoi_if_available(png_path: Path, qoi_path: Path, qoiconv):
    if qoiconv is None:
        if qoi_path.exists():
            qoi_path.unlink()
        return False

    source = png_path
    temp_png = None

    if png_path.suffix.lower() != ".png":
        temp_png = qoi_path.with_suffix(".qoi-source.png")
        with Image.open(png_path) as image:
            image = ImageOps.exif_transpose(image).convert("RGB")
            image.save(temp_png, optimize=True)
        source = temp_png

    try:
        subprocess.run([qoiconv, str(source), str(qoi_path)], check=True)
    finally:
        if temp_png is not None and temp_png.exists():
            temp_png.unlink()

    return True


def find_qoiconv(root: Path):
    local = root / ".asset-cache/bin/qoiconv"

    if local.exists():
        return str(local)

    return shutil.which("qoiconv")


def sharpen_map_image(image, radius: float, percent: int, threshold: int):
    if percent <= 0:
        return image

    return image.filter(ImageFilter.UnsharpMask(
        radius=radius,
        percent=percent,
        threshold=threshold,
    ))


def grade_map_image(image):
    image = image.convert("RGB")
    image = ImageEnhance.Color(image).enhance(0.48)
    image = ImageEnhance.Contrast(image).enhance(0.82)

    parchment = Image.new("RGB", image.size, (189, 158, 102))
    image = Image.blend(image, parchment, 0.20)

    r, g, b = image.split()
    r = r.point(lambda value: max(0, min(255, round(value * 0.96))))
    g = g.point(lambda value: max(0, min(255, round(value * 0.92))))
    b = b.point(lambda value: max(0, min(255, round(value * 0.82))))
    return Image.merge("RGB", (r, g, b))


def boundary_archive_path(root: Path, source: dict) -> Path:
    cache_path = root / ".asset-cache/naturalearth" / source["archive"]
    local_path = root / "assets/source/maps" / source["archive"]
    return local_path if local_path.exists() else cache_path


def download_boundary_archives(root: Path):
    cache = root / ".asset-cache/naturalearth"
    cache.mkdir(parents=True, exist_ok=True)

    for source in BOUNDARY_SOURCES.values():
        dest = cache / source["archive"]

        if dest.exists():
            continue

        print(f"download {dest.relative_to(root)}")
        urllib.request.urlretrieve(source["url"], dest)


def iter_shapefile_parts(archive_path: Path, shape_name: str):
    if not archive_path.exists():
        return

    with zipfile.ZipFile(archive_path) as archive:
        with archive.open(shape_name) as handle:
            data = handle.read()

    offset = 100
    total = len(data)

    while offset + 8 <= total:
        content_words = struct.unpack(">i", data[offset + 4:offset + 8])[0]
        record_start = offset + 8
        record_end = record_start + content_words*2
        offset = record_end

        if record_end > total or record_start + 44 > total:
            break

        shape_type = struct.unpack("<i", data[record_start:record_start + 4])[0]

        if shape_type not in (3, 5):
            continue

        cursor = record_start + 36
        part_count, point_count = struct.unpack("<2i", data[cursor:cursor + 8])
        cursor += 8

        if part_count <= 0 or point_count <= 0:
            continue

        parts = list(struct.unpack(f"<{part_count}i", data[cursor:cursor + part_count*4]))
        cursor += part_count*4
        points = []

        for _ in range(point_count):
            lon, lat = struct.unpack("<2d", data[cursor:cursor + 16])
            cursor += 16
            points.append((lon, lat))

        parts.append(point_count)

        for index in range(part_count):
            start = parts[index]
            end = parts[index + 1]

            if end - start >= 2:
                yield points[start:end]


def project_lon_lat(lon: float, lat: float, width: int, height: int) -> tuple[float, float]:
    x = ((lon + 180.0)/360.0)*float(width)
    y = ((90.0 - lat)/180.0)*float(height)
    return x, y


def projected_segments(points, width: int, height: int):
    segment = []
    previous_x = None

    for lon, lat in points:
        x, y = project_lon_lat(lon, lat, width, height)

        if previous_x is not None and abs(x - previous_x) > width*0.5:
            if len(segment) >= 2:
                yield segment
            segment = []

        segment.append((x, y))
        previous_x = x

    if len(segment) >= 2:
        yield segment


def draw_boundary_archive(draw, archive_path: Path, shape_name: str, width: int, height: int, fill, line_width: int):
    count = 0

    for points in iter_shapefile_parts(archive_path, shape_name):
        for segment in projected_segments(points, width, height):
            draw.line(segment, fill=fill, width=line_width, joint="curve")
        count += 1

    return count


def overlay_boundaries(root: Path, image):
    from PIL import ImageDraw

    draw = ImageDraw.Draw(image, "RGBA")
    width, height = image.size
    state_width = max(1, round(width/4600))
    country_width = max(state_width + 1, round(width/3300))
    total = 0

    state_source = BOUNDARY_SOURCES["state"]
    country_source = BOUNDARY_SOURCES["country"]
    state_archive = boundary_archive_path(root, state_source)
    country_archive = boundary_archive_path(root, country_source)

    if state_archive.exists():
        total += draw_boundary_archive(
            draw,
            state_archive,
            state_source["shape"],
            width,
            height,
            (59, 48, 34, 72),
            state_width)

    if country_archive.exists():
        total += draw_boundary_archive(
            draw,
            country_archive,
            country_source["shape"],
            width,
            height,
            (47, 35, 24, 116),
            country_width)

    if total > 0:
        print(f"map boundaries features={total}")

    return image


def prepare_map_tiles(root: Path, source: Path, tile_size: int, qoiconv, sharpen_radius: float, sharpen_percent: int, sharpen_threshold: int):
    tile_root = root / "assets/maps/tiles"
    tile_dest = tile_root / "z1"

    if tile_root.exists():
        shutil.rmtree(tile_root)

    tile_dest.mkdir(parents=True, exist_ok=True)

    with Image.open(source) as image:
        image = ImageOps.exif_transpose(image).convert("RGB")
        image.load()
        image = overlay_boundaries(root, image)
        width, height = image.size
        columns = (width + tile_size - 1) // tile_size
        rows = (height + tile_size - 1) // tile_size
        count = 0

        for row in range(rows):
            for column in range(columns):
                left = column * tile_size
                top = row * tile_size
                right = min(left + tile_size, width)
                bottom = min(top + tile_size, height)
                tile = image.crop((left, top, right, bottom))
                tile = grade_map_image(tile)
                tile = sharpen_map_image(tile, sharpen_radius, sharpen_percent, sharpen_threshold)
                png_dest = tile_dest / f"tile_{column:03d}_{row:03d}.png"
                qoi_dest = tile_dest / f"tile_{column:03d}_{row:03d}.qoi"

                tile.save(png_dest, optimize=True)
                save_qoi_if_available(png_dest, qoi_dest, qoiconv)
                count += 1

    metadata = [
        f"width={width}",
        f"height={height}",
        f"tile_size={tile_size}",
        f"columns={columns}",
        f"rows={rows}",
        "",
    ]
    (tile_root / "tiles.txt").write_text("\n".join(metadata))
    print(f"map tiles assets/maps/tiles/z1 count={count} tile_size={tile_size} source={source.relative_to(root)}")


def prepare_map(root: Path, base_width: int, tile_size: int, make_tiles: bool, sharpen_radius: float, sharpen_percent: int, sharpen_threshold: int):
    source = root / "assets/source/maps/world_map.png"
    tile_sources = [
        root / "assets/source/maps/world_map_highres.png",
        root / "assets/source/maps/world_map_highres.tif",
        root / "assets/source/maps/world_map_highres.tiff",
        root / "assets/source/maps/world_map_highres.jpg",
        root / "assets/source/maps/world_map_highres.jpeg",
    ]
    tile_source = next((path for path in tile_sources if path.exists()), source)
    png_dest = root / "assets/maps/world_map.png"
    qoi_dest = root / "assets/maps/world_map.qoi"

    if tile_size <= 0:
        raise ValueError("--map-tile-size must be positive")

    if not source.exists():
        print(f"missing map source: {source}")
        return

    png_dest.parent.mkdir(parents=True, exist_ok=True)
    with Image.open(source) as image:
        image = ImageOps.exif_transpose(image).convert("RGB")
        image.load()

        if base_width > 0 and image.width > base_width:
            ratio = base_width / float(image.width)
            resized = (base_width, max(1, round(image.height * ratio)))
            image = image.resize(resized, Image.Resampling.LANCZOS)

        image = overlay_boundaries(root, image)
        image = grade_map_image(image)
        image = sharpen_map_image(image, sharpen_radius, sharpen_percent, sharpen_threshold)
        image.save(png_dest, optimize=True)
    print(f"map {png_dest.relative_to(root)}")

    qoiconv = find_qoiconv(root)
    if qoiconv is None:
        print("qoiconv not found; keeping PNG fallback only")
    elif save_qoi_if_available(png_dest, qoi_dest, qoiconv):
        print(f"map {qoi_dest.relative_to(root)}")

    if make_tiles:
        prepare_map_tiles(root, tile_source, tile_size, qoiconv, sharpen_radius, sharpen_percent, sharpen_threshold)


def main():
    parser = argparse.ArgumentParser(description="Prepare 1080p travelpi assets.")
    parser.add_argument("--width", type=int, default=1920)
    parser.add_argument("--quality", type=int, default=88)
    parser.add_argument("--photo-long-edge", type=int, default=1600)
    parser.add_argument("--map-base-width", type=int, default=4096)
    parser.add_argument("--map-tile-size", type=int, default=512)
    parser.add_argument("--map-sharpen-radius", type=float, default=1.1)
    parser.add_argument("--map-sharpen-percent", type=int, default=150)
    parser.add_argument("--map-sharpen-threshold", type=int, default=2)
    parser.add_argument("--download-map-boundaries", action="store_true")
    parser.add_argument("--no-map-tiles", action="store_true")
    parser.add_argument("--photos-only", action="store_true")
    parser.add_argument("--map-only", action="store_true")
    args = parser.parse_args()

    root = repo_root()

    if args.download_map_boundaries:
        download_boundary_archives(root)

    if not args.map_only:
        prepare_photos(root, args.width, args.quality, args.photo_long_edge)

    if not args.photos_only:
        prepare_map(
            root,
            args.map_base_width,
            args.map_tile_size,
            not args.no_map_tiles,
            args.map_sharpen_radius,
            args.map_sharpen_percent,
            args.map_sharpen_threshold)


if __name__ == "__main__":
    main()
