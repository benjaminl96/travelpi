#!/usr/bin/env python3
import argparse
import json
import math
import re
import shutil
import subprocess
import sys
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path

try:
    from PIL import Image, ImageOps
except ModuleNotFoundError as exc:
    raise SystemExit(
        "import_trip.py requires Pillow. Install it with "
        "`python3 -m pip install Pillow` on desktop, or run "
        "`./scripts/install_pi_deps.sh` on Raspberry Pi OS."
    ) from exc


IMAGE_SUFFIXES = {".jpg", ".jpeg", ".png", ".tif", ".tiff", ".heic", ".heif"}
DEFAULT_LAYOUT = [
    {"anchor": [0.42, 0.31], "scale": 0.20, "rotation": -7.0, "drift": 0.4},
    {"anchor": [0.56, 0.28], "scale": 0.19, "rotation": 6.0, "drift": 1.5},
    {"anchor": [0.50, 0.53], "scale": 0.21, "rotation": -2.0, "drift": 2.6},
    {"anchor": [0.61, 0.52], "scale": 0.18, "rotation": 4.0, "drift": 3.2},
]


@dataclass
class PhotoCandidate:
    path: Path
    captured_at: datetime | None
    latitude: float | None
    longitude: float | None


def repo_root() -> Path:
    return Path(__file__).resolve().parents[1]


def slugify(value: str) -> str:
    slug = re.sub(r"[^a-z0-9]+", "_", value.lower()).strip("_")
    return slug or "trip"


def rational_to_float(value) -> float:
    if isinstance(value, tuple):
        return float(value[0]) / float(value[1])
    return float(value)


def dms_to_decimal(dms, ref: str) -> float:
    degrees = rational_to_float(dms[0])
    minutes = rational_to_float(dms[1])
    seconds = rational_to_float(dms[2])
    decimal = degrees + minutes / 60.0 + seconds / 3600.0

    if ref in ("S", "W"):
        decimal = -decimal

    return decimal


def exif_datetime(exif) -> datetime | None:
    for tag in (36867, 36868, 306):
        value = exif.get(tag)

        if not value:
            continue

        if isinstance(value, bytes):
            value = value.decode("utf-8", errors="ignore")

        try:
            return datetime.strptime(str(value), "%Y:%m:%d %H:%M:%S")
        except ValueError:
            continue

    return None


def exif_gps(exif) -> tuple[float, float] | tuple[None, None]:
    gps = exif.get_ifd(34853) if hasattr(exif, "get_ifd") else exif.get(34853)

    if not gps:
        return None, None

    try:
        lat_ref = gps[1]
        lat_dms = gps[2]
        lon_ref = gps[3]
        lon_dms = gps[4]
    except KeyError:
        return None, None

    if isinstance(lat_ref, bytes):
        lat_ref = lat_ref.decode("ascii", errors="ignore")
    if isinstance(lon_ref, bytes):
        lon_ref = lon_ref.decode("ascii", errors="ignore")

    return dms_to_decimal(lat_dms, lat_ref), dms_to_decimal(lon_dms, lon_ref)


def mdls_value(path: Path, key: str) -> str | None:
    try:
        result = subprocess.run(
            ["mdls", "-raw", "-name", key, str(path)],
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
        )
    except (subprocess.CalledProcessError, FileNotFoundError):
        return None

    value = result.stdout.strip()
    return None if value in ("", "(null)") else value


def read_candidate_with_mdls(path: Path) -> PhotoCandidate | None:
    lat_raw = mdls_value(path, "kMDItemLatitude")
    lon_raw = mdls_value(path, "kMDItemLongitude")
    date_raw = mdls_value(path, "kMDItemContentCreationDate")
    captured_at = None
    latitude = None
    longitude = None

    if date_raw:
        try:
            captured_at = datetime.strptime(date_raw.rsplit(" ", 1)[0], "%Y-%m-%d %H:%M:%S")
        except ValueError:
            captured_at = None

    if lat_raw and lon_raw:
        try:
            latitude = float(lat_raw)
            longitude = float(lon_raw)
        except ValueError:
            latitude = None
            longitude = None

    if captured_at is None and latitude is None and longitude is None:
        return None

    return PhotoCandidate(path, captured_at, latitude, longitude)


def read_candidate(path: Path) -> PhotoCandidate | None:
    try:
        with Image.open(path) as image:
            exif = image.getexif()
            captured_at = exif_datetime(exif)
            latitude, longitude = exif_gps(exif)
            return PhotoCandidate(path, captured_at, latitude, longitude)
    except Exception as exc:
        candidate = read_candidate_with_mdls(path)

        if candidate is not None:
            return candidate

        print(f"skip {path}: {exc}")
        return None


def scan_images(input_path: Path) -> list[PhotoCandidate]:
    paths = []

    if input_path.is_file():
        paths = [input_path]
    else:
        paths = sorted(p for p in input_path.rglob("*") if p.suffix.lower() in IMAGE_SUFFIXES)

    candidates = []

    for path in paths:
        candidate = read_candidate(path)

        if candidate is not None:
            candidates.append(candidate)

    return candidates


def median(values: list[float]) -> float:
    values = sorted(values)
    middle = len(values) // 2

    if len(values) % 2:
        return values[middle]

    return (values[middle - 1] + values[middle]) / 2.0


def trip_date_range(candidates: list[PhotoCandidate]) -> tuple[str | None, str | None]:
    dates = sorted(c.captured_at.date() for c in candidates if c.captured_at is not None)

    if not dates:
        return None, None

    return dates[0].isoformat(), dates[-1].isoformat()


def choose_photos(candidates: list[PhotoCandidate], count: int | None) -> list[PhotoCandidate]:
    ordered = sorted(
        candidates,
        key=lambda item: (
            item.captured_at is None,
            item.captured_at or datetime.min,
            str(item.path),
        ),
    )

    if count is None or len(ordered) <= count:
        return ordered

    chosen = []

    for index in range(count):
        sample = round(index * (len(ordered) - 1) / float(count - 1))
        chosen.append(ordered[sample])

    return chosen


def copy_as_jpeg(source: Path, dest: Path, quality: int):
    if source.suffix.lower() in (".heic", ".heif"):
        sips = shutil.which("sips")

        if sips is None:
            raise SystemExit(f"cannot convert {source}: sips not found")

        dest.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(
            [sips, "-s", "format", "jpeg", "-s", "formatOptions", str(quality), str(source), "--out", str(dest)],
            check=True,
            stdout=subprocess.DEVNULL,
        )
        return

    with Image.open(source) as image:
        image = ImageOps.exif_transpose(image).convert("RGB")
        image.load()
        dest.parent.mkdir(parents=True, exist_ok=True)
        image.save(dest, quality=quality, optimize=True, progressive=False)


def load_manifest(path: Path) -> dict:
    if path.exists():
        return json.loads(path.read_text())

    return {"trips": []}


def save_manifest(path: Path, manifest: dict):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(manifest, indent=2) + "\n")


def append_trip(manifest: dict, trip: dict, replace: bool):
    trips = manifest.setdefault("trips", [])

    for index, existing in enumerate(trips):
        if existing.get("name") == trip["name"]:
            if not replace:
                raise SystemExit(f"trip already exists: {trip['name']} (use --replace)")
            trips[index] = trip
            return

    trips.append(trip)


def build_trip(args, candidates: list[PhotoCandidate]) -> dict:
    gps_candidates = [c for c in candidates if c.latitude is not None and c.longitude is not None]

    if args.lat is not None and args.lon is not None:
        latitude = args.lat
        longitude = args.lon
    elif gps_candidates:
        latitude = median([c.latitude for c in gps_candidates if c.latitude is not None])
        longitude = median([c.longitude for c in gps_candidates if c.longitude is not None])
    else:
        raise SystemExit("no GPS EXIF found; pass --lat and --lon")

    selected = choose_photos(candidates, args.max_photos)
    start_date, end_date = trip_date_range(candidates)
    slug = args.slug or slugify(args.name)
    root = repo_root()
    source_root = root / "assets/source/photos"

    photos = []

    for index, candidate in enumerate(selected):
        filename = f"{slug}_{index + 1:02d}.jpg"
        source_dest = source_root / filename

        if not args.dry_run:
            copy_as_jpeg(candidate.path, source_dest, args.quality)

        layout = dict(DEFAULT_LAYOUT[index % len(DEFAULT_LAYOUT)])
        layout["path"] = f"assets/photos/{filename}"
        photos.append(layout)

    trip = {
        "name": args.name,
        "caption": args.caption or args.name,
        "geo": {
            "latitude": round(latitude, 6),
            "longitude": round(longitude, 6),
        },
        "pixel_nudge": [0.0, 0.0],
        "close_zoom": args.close_zoom,
        "zoom_in_seconds": 2.35,
        "hold_seconds": args.hold_seconds,
        "fade_seconds": args.fade_seconds,
        "zoom_out_seconds": 2.05,
        "photos": photos,
    }

    if start_date is not None:
        trip["start_date"] = start_date
        trip["end_date"] = end_date

    return trip


def run_script(script: str, *args: str):
    root = repo_root()
    command = [sys.executable, str(root / "scripts" / script), *args]
    subprocess.run(command, cwd=root, check=True)


def main():
    root = repo_root()
    parser = argparse.ArgumentParser(
        description="Import a trip from photos with EXIF GPS into config/trips.json."
    )
    parser.add_argument("input", help="Photo file or folder to import.")
    parser.add_argument("--name", required=True, help="Trip name shown on the frame.")
    parser.add_argument("--caption", help="Smaller subtitle shown under the trip name.")
    parser.add_argument("--slug", help="Filename prefix. Defaults to a slug from --name.")
    parser.add_argument("--lat", type=float, help="Latitude override when photos have no GPS.")
    parser.add_argument("--lon", type=float, help="Longitude override when photos have no GPS.")
    parser.add_argument("--max-photos", type=int, help="Optional cap. Defaults to every readable still image.")
    parser.add_argument("--close-zoom", type=float, default=3.0)
    parser.add_argument("--hold-seconds", type=float, default=15.0)
    parser.add_argument("--fade-seconds", type=float, default=0.45)
    parser.add_argument("--quality", type=int, default=92)
    parser.add_argument("--replace", action="store_true", help="Replace an existing trip with the same name.")
    parser.add_argument("--prepare", action="store_true", help="Run prepare_assets.py after import.")
    parser.add_argument("--dry-run", action="store_true", help="Print the generated trip JSON without writing files.")
    args = parser.parse_args()

    candidates = scan_images(Path(args.input).expanduser())

    if not candidates:
        raise SystemExit("no readable images found")

    trip = build_trip(args, candidates)

    if args.dry_run:
        print(json.dumps(trip, indent=2))
        return

    manifest_path = root / "config/trips.json"
    manifest = load_manifest(manifest_path)
    append_trip(manifest, trip, args.replace)
    save_manifest(manifest_path, manifest)

    run_script("generate_travel_config.py")

    if args.prepare:
        run_script("prepare_assets.py", "--photos-only")

    gps_count = sum(1 for c in candidates if c.latitude is not None and c.longitude is not None)
    print(f"imported {trip['name']}: {len(trip['photos'])} photos, {gps_count}/{len(candidates)} with GPS")
    print(f"center: {trip['geo']['latitude']}, {trip['geo']['longitude']}")


if __name__ == "__main__":
    main()
