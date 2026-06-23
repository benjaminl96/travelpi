#!/usr/bin/env python3
import argparse
import json
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from import_trip import (  # noqa: E402
    DEFAULT_LAYOUT,
    IMAGE_SUFFIXES,
    PhotoCandidate,
    choose_photos,
    copy_as_jpeg,
    median,
    read_candidate,
    slugify,
    sort_manifest_trips,
    trip_date_range,
)
from prepare_assets import (  # noqa: E402
    find_qoiconv,
    save_prepared_photo,
    save_qoi_if_available,
)


DEFAULT_UPLOAD_ROOT = "~/Desktop/Trips/Upload"
DEFAULT_STAGE_ROOT = ".asset-cache/seed_pi_trips"


def repo_root() -> Path:
    return Path(__file__).resolve().parents[1]


def run(command: list[str], cwd: Path | None = None, dry_run: bool = False):
    print("$ " + " ".join(command))
    if dry_run:
        return
    subprocess.run(command, cwd=cwd, check=True)


def trip_dirs(upload_root: Path) -> list[Path]:
    return sorted(
        (path for path in upload_root.iterdir() if path.is_dir() and not path.name.startswith(".")),
        key=lambda path: path.name.casefold(),
    )


def scan_trip_images(folder: Path) -> list[PhotoCandidate]:
    candidates = []
    paths = sorted(path for path in folder.rglob("*") if path.suffix.lower() in IMAGE_SUFFIXES)

    for path in paths:
        candidate = read_candidate(path)
        if candidate is None:
            candidate = PhotoCandidate(path, None, None, None)
        candidates.append(candidate)

    return candidates


def load_geo_overrides(path: Path | None) -> dict:
    if path is None:
        return {}

    raw = json.loads(path.expanduser().read_text())
    overrides = {}

    for name, value in raw.items():
        if not isinstance(value, dict):
            raise ValueError(f"{name}: override must be an object")

        lat = value.get("lat", value.get("latitude"))
        lon = value.get("lon", value.get("longitude"))
        if lat is None or lon is None:
            raise ValueError(f"{name}: override must include lat/lon")

        overrides[name] = (float(lat), float(lon))

    return overrides


def trip_center(name: str, candidates: list[PhotoCandidate], overrides: dict) -> tuple[float, float] | None:
    if name in overrides:
        return overrides[name]

    gps_candidates = [c for c in candidates if c.latitude is not None and c.longitude is not None]

    if not gps_candidates:
        return None

    return (
        median([c.latitude for c in gps_candidates if c.latitude is not None]),
        median([c.longitude for c in gps_candidates if c.longitude is not None]),
    )


def next_unique_slug(base: str, used: set[str]) -> str:
    slug = base
    suffix = 2

    while slug in used:
        slug = f"{base}_{suffix}"
        suffix += 1

    used.add(slug)
    return slug


def build_trip(
    folder: Path,
    candidates: list[PhotoCandidate],
    source_root: Path,
    used_slugs: set[str],
    overrides: dict,
    max_photos: int | None,
    source_quality: int,
    dry_run: bool,
) -> tuple[dict | None, dict]:
    name = folder.name
    center = trip_center(name, candidates, overrides)
    gps_count = sum(1 for c in candidates if c.latitude is not None and c.longitude is not None)
    selected = choose_photos(candidates, max_photos)
    report = {
        "name": name,
        "source_count": len(candidates),
        "selected_count": len(selected),
        "gps_count": gps_count,
        "skipped": None,
    }

    if not candidates:
        report["skipped"] = "no still images"
        return None, report

    if center is None:
        report["skipped"] = "no GPS; add a geo override"
        return None, report

    slug = next_unique_slug(slugify(name), used_slugs)
    photos = []

    for index, candidate in enumerate(selected, start=1):
        filename = f"{slug}_{index:02d}.jpg"
        source_dest = source_root / filename

        if not dry_run:
            copy_as_jpeg(candidate.path, source_dest, source_quality)

        layout = dict(DEFAULT_LAYOUT[(index - 1) % len(DEFAULT_LAYOUT)])
        layout["path"] = f"assets/photos/{filename}"
        photos.append(layout)

    start_date, end_date = trip_date_range(candidates)
    trip = {
        "name": name,
        "caption": name,
        "geo": {
            "latitude": round(center[0], 6),
            "longitude": round(center[1], 6),
        },
        "pixel_nudge": [0.0, 0.0],
        "close_zoom": 10.0,
        "zoom_in_seconds": 2.35,
        "hold_seconds": 15.0,
        "fade_seconds": 0.45,
        "zoom_out_seconds": 2.05,
        "photos": photos,
    }

    if start_date is not None:
        trip["start_date"] = start_date
        trip["end_date"] = end_date

    return trip, report


def prepare_staged_photos(stage_root: Path, photo_long_edge: int, quality: int, dry_run: bool):
    source_root = stage_root / "assets/source/photos"
    prepared_root = stage_root / "assets/photos"
    qoiconv = find_qoiconv(repo_root())

    for source in sorted(source_root.glob("*.jpg")):
        dest = prepared_root / source.name
        if dry_run:
            print(f"would prepare {dest.relative_to(stage_root)} long_edge={photo_long_edge}")
            continue

        save_prepared_photo(source, dest, photo_long_edge, quality)
        print(f"photo {dest.relative_to(stage_root)} long_edge={photo_long_edge}")

        qoi_dest = dest.with_suffix(".qoi")
        if save_qoi_if_available(dest, qoi_dest, qoiconv):
            print(f"photo {qoi_dest.relative_to(stage_root)}")


def write_manifest(stage_root: Path, trips: list[dict], dry_run: bool):
    manifest = sort_manifest_trips({"trips": trips})
    text = json.dumps(manifest, indent=2) + "\n"

    if dry_run:
        print(text)
        return

    manifest_path = stage_root / "config/trips.json"
    manifest_path.parent.mkdir(parents=True, exist_ok=True)
    manifest_path.write_text(text)


def reset_stage(stage_root: Path, dry_run: bool):
    if dry_run:
        return

    if stage_root.exists():
        shutil.rmtree(stage_root)

    (stage_root / "assets/source/photos").mkdir(parents=True, exist_ok=True)
    (stage_root / "assets/photos").mkdir(parents=True, exist_ok=True)
    (stage_root / "config").mkdir(parents=True, exist_ok=True)


def deploy(stage_root: Path, remote: str, remote_root: str, dry_run: bool):
    run(["ssh", remote, f"mkdir -p {remote_root}/config {remote_root}/assets/photos {remote_root}/assets/source/photos"], dry_run=dry_run)
    run(["ssh", remote, f"find {remote_root}/assets/photos -mindepth 1 ! -name .gitkeep -delete"], dry_run=dry_run)
    run(["ssh", remote, f"find {remote_root}/assets/source/photos -mindepth 1 ! -name .gitkeep -delete"], dry_run=dry_run)
    run(["rsync", "-az", "--delete", f"{stage_root}/assets/photos/", f"{remote}:{remote_root}/assets/photos/"], dry_run=dry_run)
    run(["rsync", "-az", "--delete", f"{stage_root}/assets/source/photos/", f"{remote}:{remote_root}/assets/source/photos/"], dry_run=dry_run)
    run(["rsync", "-az", f"{stage_root}/config/trips.json", f"{remote}:{remote_root}/config/trips.json"], dry_run=dry_run)
    run([
        "ssh",
        remote,
        f"cd {remote_root} && python3 -m py_compile admin/travelpi_admin.py scripts/prepare_assets.py scripts/import_trip.py && "
        "sudo systemctl restart travelpi-admin.service && sudo systemctl restart travelpi.service",
    ], dry_run=dry_run)


def main():
    parser = argparse.ArgumentParser(
        description="One-time seed import of ~/Desktop/Trips/Upload folders into the Raspberry Pi travelpi install."
    )
    parser.add_argument("--upload-root", default=DEFAULT_UPLOAD_ROOT)
    parser.add_argument("--stage-root", default=DEFAULT_STAGE_ROOT)
    parser.add_argument("--remote", default="travelpi.local")
    parser.add_argument("--remote-root", default="/opt/travelpi")
    parser.add_argument("--geo-overrides", type=Path, help="JSON mapping trip names to {lat, lon}.")
    parser.add_argument("--max-photos", type=int, help="Optional per-trip photo cap.")
    parser.add_argument("--source-quality", type=int, default=92)
    parser.add_argument("--prepared-quality", type=int, default=88)
    parser.add_argument("--photo-long-edge", type=int, default=1600)
    parser.add_argument("--skip-deploy", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()

    upload_root = Path(args.upload_root).expanduser()
    stage_root = (repo_root() / args.stage_root).resolve()
    overrides = load_geo_overrides(args.geo_overrides)

    if not upload_root.exists():
        raise SystemExit(f"upload root not found: {upload_root}")

    reset_stage(stage_root, args.dry_run)

    trips = []
    reports = []
    used_slugs = set()
    source_root = stage_root / "assets/source/photos"

    for folder in trip_dirs(upload_root):
        candidates = scan_trip_images(folder)
        trip, report = build_trip(
            folder,
            candidates,
            source_root,
            used_slugs,
            overrides,
            args.max_photos,
            args.source_quality,
            args.dry_run,
        )
        reports.append(report)

        if trip is not None:
            trips.append(trip)

    write_manifest(stage_root, trips, args.dry_run)

    if trips and not args.dry_run:
        prepare_staged_photos(stage_root, args.photo_long_edge, args.prepared_quality, args.dry_run)

    print("\nSeed summary")
    print(f"  trips ready: {len(trips)}")
    print(f"  skipped: {sum(1 for report in reports if report['skipped'])}")
    for report in reports:
        status = report["skipped"] or "ready"
        print(
            f"  - {report['name']}: {status}; "
            f"{report['selected_count']}/{report['source_count']} photos; GPS {report['gps_count']}"
        )

    skipped = [report for report in reports if report["skipped"]]
    if skipped:
        print("\nSkipped trips need attention before a full seed:")
        for report in skipped:
            print(f"  - {report['name']}: {report['skipped']}")

    if args.skip_deploy or args.dry_run:
        print(f"\nStaged at {stage_root}")
        return

    if skipped:
        raise SystemExit("Refusing to deploy partial seed while trips are skipped. Add --skip-deploy to stage only, or provide --geo-overrides.")

    deploy(stage_root, args.remote, args.remote_root, args.dry_run)


if __name__ == "__main__":
    main()
