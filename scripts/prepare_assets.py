#!/usr/bin/env python3
import argparse
import json
import re
import shutil
import subprocess
from pathlib import Path

try:
    from PIL import Image, ImageOps
except ModuleNotFoundError as exc:
    raise SystemExit(
        "prepare_assets.py requires Pillow. Install it with "
        "`python3 -m pip install Pillow` on desktop, or run "
        "`./scripts/install_pi_deps.sh` on Raspberry Pi OS."
    ) from exc


PHOTO_PATTERN = re.compile(
    r'\{\s*"(?P<path>assets/photos/[^"]+)"\s*,\s*'
    r'\{\s*[-0-9.]+f?\s*,\s*[-0-9.]+f?\s*\}\s*,\s*'
    r'(?P<scale>[-0-9.]+)f?'
)


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


def save_prepared_photo(source: Path, dest: Path, long_edge: int, quality: int):
    with Image.open(source) as image:
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

    for rel, scale in configured_photos(root):
        dest = root / rel
        source = source_root / Path(rel).name

        if not source.exists() and fallback_root.joinpath(Path(rel).name).exists():
            source = fallback_root / Path(rel).name

        if not source.exists():
            print(f"missing photo source: {source_root / Path(rel).name}")
            continue

        long_edge = max(1, round(width * scale), photo_long_edge)
        save_prepared_photo(source, dest, long_edge, quality)
        print(f"photo {dest.relative_to(root)} long_edge={long_edge}")


def prepare_map(root: Path):
    source = root / "assets/source/maps/world_map.png"
    png_dest = root / "assets/maps/world_map.png"
    qoi_dest = root / "assets/maps/world_map.qoi"

    if not source.exists():
        print(f"missing map source: {source}")
        return

    png_dest.parent.mkdir(parents=True, exist_ok=True)
    with Image.open(source) as image:
        image = ImageOps.exif_transpose(image).convert("RGB")
        image.save(png_dest, optimize=True)
    print(f"map {png_dest.relative_to(root)}")

    qoiconv = shutil.which("qoiconv")
    if qoiconv is None:
        print("qoiconv not found; keeping PNG fallback only")
        return

    subprocess.run([qoiconv, str(png_dest), str(qoi_dest)], check=True)
    print(f"map {qoi_dest.relative_to(root)}")


def main():
    parser = argparse.ArgumentParser(description="Prepare 1080p travelpi assets.")
    parser.add_argument("--width", type=int, default=1920)
    parser.add_argument("--quality", type=int, default=88)
    parser.add_argument("--photo-long-edge", type=int, default=1280)
    parser.add_argument("--photos-only", action="store_true")
    parser.add_argument("--map-only", action="store_true")
    args = parser.parse_args()

    root = repo_root()

    if not args.map_only:
        prepare_photos(root, args.width, args.quality, args.photo_long_edge)

    if not args.photos_only:
        prepare_map(root)


if __name__ == "__main__":
    main()
