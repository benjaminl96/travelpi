#!/usr/bin/env python3
import argparse
import json
import mimetypes
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import traceback
from datetime import datetime
from email import policy
from email.parser import BytesParser
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import unquote, urlparse

ROOT = Path(__file__).resolve().parents[1]
SCRIPTS = ROOT / "scripts"
MANIFEST_PATH = ROOT / "config/trips.json"
EXAMPLE_MANIFEST_PATH = ROOT / "config/trips.example.json"
SOURCE_PHOTO_ROOT = ROOT / "assets/source/photos"
PREPARED_PHOTO_ROOT = ROOT / "assets/photos"
SOURCE_MAP_ROOT = ROOT / "assets/source/maps"
MAP_ROOT = ROOT / "assets/maps"
STATIC_ROOT = ROOT / "admin/static"
TEMPLATE_ROOT = ROOT / "admin/templates"
UPLOAD_CACHE = ROOT / ".asset-cache/admin/uploads"

sys.path.insert(0, str(SCRIPTS))
from import_trip import (  # noqa: E402
    DEFAULT_LAYOUT,
    PhotoCandidate,
    copy_as_jpeg,
    median,
    read_candidate,
    slugify,
    trip_date_range,
)

MANIFEST_LOCK = threading.Lock()
JOB_LOCK = threading.Lock()
JOBS = []
NEXT_JOB_ID = 1

DEFAULT_TRIP = {
    "pixel_nudge": [0.0, 0.0],
    "close_zoom": 10.0,
    "zoom_in_seconds": 2.35,
    "hold_seconds": 15.0,
    "fade_seconds": 0.45,
    "zoom_out_seconds": 2.05,
}


def now_label():
    return datetime.now().strftime("%H:%M:%S")


def ensure_manifest():
    if MANIFEST_PATH.exists():
        return

    MANIFEST_PATH.parent.mkdir(parents=True, exist_ok=True)
    if EXAMPLE_MANIFEST_PATH.exists():
        shutil.copyfile(EXAMPLE_MANIFEST_PATH, MANIFEST_PATH)
    else:
        MANIFEST_PATH.write_text(json.dumps({"trips": []}, indent=2) + "\n")


def load_manifest():
    ensure_manifest()
    return json.loads(MANIFEST_PATH.read_text())


def save_manifest(manifest):
    MANIFEST_PATH.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile("w", delete=False, dir=str(MANIFEST_PATH.parent), prefix=".trips.", suffix=".json") as handle:
        json.dump(manifest, handle, indent=2)
        handle.write("\n")
        temp_name = handle.name

    Path(temp_name).replace(MANIFEST_PATH)


def secure_filename(name):
    name = Path(name).name
    stem = re.sub(r"[^A-Za-z0-9._-]+", "_", name).strip("._")
    return stem or "upload.jpg"


def default_photo(index, path):
    layout = dict(DEFAULT_LAYOUT[index % len(DEFAULT_LAYOUT)])
    layout["path"] = path
    return layout


def normalize_float(value, fallback):
    if value in (None, ""):
        return fallback
    return float(value)


def normalize_trip(raw):
    trip = dict(DEFAULT_TRIP)
    trip.update(raw)

    name = str(trip.get("name", "")).strip()
    if not name:
        raise ValueError("Trip name is required.")

    trip["name"] = name
    trip["caption"] = str(trip.get("caption") or name).strip()
    geo = trip.get("geo") or {}

    if "latitude" not in geo or "longitude" not in geo:
        raise ValueError("Latitude and longitude are required.")

    trip["geo"] = {
        "latitude": round(float(geo["latitude"]), 6),
        "longitude": round(float(geo["longitude"]), 6),
    }
    trip["pixel_nudge"] = [
        normalize_float((trip.get("pixel_nudge") or [0.0, 0.0])[0], 0.0),
        normalize_float((trip.get("pixel_nudge") or [0.0, 0.0])[1], 0.0),
    ]

    for key in ("close_zoom", "zoom_in_seconds", "hold_seconds", "fade_seconds", "zoom_out_seconds"):
        trip[key] = normalize_float(trip.get(key), DEFAULT_TRIP[key])

    for key in ("start_date", "end_date", "date_label"):
        if trip.get(key) in (None, ""):
            trip.pop(key, None)

    photos = []
    for index, photo in enumerate(trip.get("photos", [])):
        if not photo.get("path"):
            continue
        merged = default_photo(index, photo["path"])
        merged.update(photo)
        merged["anchor"] = [
            normalize_float((merged.get("anchor") or [0.5, 0.5])[0], 0.5),
            normalize_float((merged.get("anchor") or [0.5, 0.5])[1], 0.5),
        ]
        for key in ("scale", "rotation", "drift"):
            merged[key] = normalize_float(merged.get(key), default_photo(index, photo["path"])[key])
        photos.append(merged)
    trip["photos"] = photos

    return trip


def next_photo_filename(slug, index):
    SOURCE_PHOTO_ROOT.mkdir(parents=True, exist_ok=True)
    candidate = SOURCE_PHOTO_ROOT / f"{slug}_{index:02d}.jpg"
    while candidate.exists():
        index += 1
        candidate = SOURCE_PHOTO_ROOT / f"{slug}_{index:02d}.jpg"
    return candidate


def mtime(path):
    try:
        return path.stat().st_mtime
    except FileNotFoundError:
        return 0


def qoiconv_available():
    return (ROOT / ".asset-cache/bin/qoiconv").exists() or shutil.which("qoiconv") is not None


def photo_source_for_path(rel_path):
    name = Path(rel_path).name
    source = SOURCE_PHOTO_ROOT / name

    if source.exists():
        return source

    fallback = ROOT / rel_path
    return fallback if fallback.exists() else source


def photo_asset_status(manifest):
    reasons = []
    dirty_count = 0
    wants_qoi = qoiconv_available()
    seen = set()

    for trip in manifest.get("trips", []):
        for photo in trip.get("photos", []):
            rel = photo.get("path")

            if not rel or rel in seen:
                continue

            seen.add(rel)
            dest = ROOT / rel
            source = photo_source_for_path(rel)
            qoi_dest = dest.with_suffix(".qoi")
            photo_dirty = False

            if not source.exists():
                photo_dirty = True
                reasons.append(f"Missing source for {Path(rel).name}")
            elif not dest.exists():
                photo_dirty = True
                reasons.append(f"{Path(rel).name} has not been prepared")
            elif mtime(source) > mtime(dest):
                photo_dirty = True
                reasons.append(f"{Path(rel).name} source changed")

            if wants_qoi and dest.exists() and (not qoi_dest.exists() or mtime(dest) > mtime(qoi_dest)):
                photo_dirty = True
                reasons.append(f"{qoi_dest.name} needs updating")

            if photo_dirty:
                dirty_count += 1

    return {
        "dirty": dirty_count > 0,
        "count": dirty_count,
        "reasons": reasons[:6],
    }


def newest_existing(paths):
    return max((mtime(path) for path in paths if path.exists()), default=0)


def map_asset_status():
    reasons = []
    source = SOURCE_MAP_ROOT / "world_map.png"
    highres_sources = [
        SOURCE_MAP_ROOT / "world_map_highres.png",
        SOURCE_MAP_ROOT / "world_map_highres.tif",
        SOURCE_MAP_ROOT / "world_map_highres.tiff",
        SOURCE_MAP_ROOT / "world_map_highres.jpg",
        SOURCE_MAP_ROOT / "world_map_highres.jpeg",
    ]
    map_png = MAP_ROOT / "world_map.png"
    map_qoi = MAP_ROOT / "world_map.qoi"
    tiles_meta = MAP_ROOT / "tiles/tiles.txt"
    wants_qoi = qoiconv_available()
    source_time = mtime(source)
    highres_time = newest_existing(highres_sources)

    if source.exists() and (not map_png.exists() or source_time > mtime(map_png)):
        reasons.append("Base map needs preparing")

    if wants_qoi and map_png.exists() and (not map_qoi.exists() or mtime(map_png) > mtime(map_qoi)):
        reasons.append("Base map QOI needs updating")

    if highres_time > 0 and (not tiles_meta.exists() or highres_time > mtime(tiles_meta)):
        reasons.append("High-resolution map tiles need preparing")

    return {
        "dirty": len(reasons) > 0,
        "count": len(reasons),
        "reasons": reasons[:6],
    }


def asset_status(manifest):
    photos = photo_asset_status(manifest)
    maps = map_asset_status()

    return {
        "photos": photos,
        "map": maps,
        "all": {
            "dirty": photos["dirty"] or maps["dirty"],
            "reasons": (photos["reasons"] + maps["reasons"])[:6],
        },
    }


def run_command(command, job):
    job_log(job, "$ " + " ".join(command))
    process = subprocess.Popen(
        command,
        cwd=str(ROOT),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    assert process.stdout is not None
    for line in process.stdout:
        job_log(job, line.rstrip())

    rc = process.wait()
    if rc != 0:
        raise RuntimeError(f"command failed with exit code {rc}")


def job_log(job, line):
    with JOB_LOCK:
        job["log"].append(f"[{now_label()}] {line}")
        job["log"] = job["log"][-300:]


def start_job(label, target):
    global NEXT_JOB_ID
    with JOB_LOCK:
        job = {
            "id": NEXT_JOB_ID,
            "label": label,
            "status": "running",
            "created_at": time.time(),
            "updated_at": time.time(),
            "log": [],
        }
        NEXT_JOB_ID += 1
        JOBS.insert(0, job)
        del JOBS[30:]

    def runner():
        try:
            job_log(job, "started")
            target(job)
            with JOB_LOCK:
                job["status"] = "complete"
                job["updated_at"] = time.time()
            job_log(job, "complete")
        except Exception as exc:
            with JOB_LOCK:
                job["status"] = "failed"
                job["updated_at"] = time.time()
            job_log(job, f"failed: {exc}")
            job_log(job, traceback.format_exc().rstrip())

    threading.Thread(target=runner, daemon=True).start()
    return job


def prepare_job(mode):
    def run(job):
        run_command([sys.executable, str(SCRIPTS / "generate_travel_config.py")], job)
        args = [sys.executable, str(SCRIPTS / "prepare_assets.py")]
        if mode == "photos":
            args.append("--photos-only")
        elif mode == "map":
            args.append("--map-only")
        elif mode != "all":
            raise ValueError(f"unknown prepare mode: {mode}")
        run_command(args, job)
        job_log(job, "display app will hot-reload trips from config/trips.json")
    return start_job(f"prepare {mode}", run)


def restart_job():
    command = os.environ.get("TRAVELPI_RESTART_COMMAND")

    def run(job):
        if command:
            run_command(shlex.split(command), job)
            return

        for candidate in (
            ["systemctl", "--user", "restart", "travelpi.service"],
            ["systemctl", "restart", "travelpi.service"],
        ):
            try:
                run_command(candidate, job)
                return
            except Exception as exc:
                job_log(job, f"restart attempt failed: {exc}")

        raise RuntimeError("set TRAVELPI_RESTART_COMMAND for this install")

    return start_job("restart display", run)


def multipart_parts(handler):
    content_type = handler.headers.get("Content-Type", "")
    length = int(handler.headers.get("Content-Length", "0"))
    body = handler.rfile.read(length)
    raw = (
        f"Content-Type: {content_type}\r\n"
        "MIME-Version: 1.0\r\n\r\n"
    ).encode("utf-8") + body
    message = BytesParser(policy=policy.default).parsebytes(raw)
    fields = {}
    files = []

    for part in message.iter_parts():
        disposition = part.get("Content-Disposition", "")
        name = part.get_param("name", header="content-disposition")
        filename = part.get_filename()
        payload = part.get_payload(decode=True) or b""

        if not disposition or not name:
            continue

        if filename:
            files.append({
                "field": name,
                "filename": secure_filename(filename),
                "content": payload,
            })
        else:
            fields[name] = payload.decode(part.get_content_charset() or "utf-8", errors="replace")

    return fields, files


def temp_upload(file_item):
    UPLOAD_CACHE.mkdir(parents=True, exist_ok=True)
    suffix = Path(file_item["filename"]).suffix or ".jpg"
    handle = tempfile.NamedTemporaryFile(delete=False, dir=str(UPLOAD_CACHE), suffix=suffix)
    with handle:
        handle.write(file_item["content"])
    return Path(handle.name)


def candidates_from_uploads(files):
    candidates = []
    temp_paths = []

    for file_item in files:
        path = temp_upload(file_item)
        temp_paths.append(path)
        candidate = read_candidate(path)
        if candidate is None:
            candidate = PhotoCandidate(path, None, None, None)
        candidates.append(candidate)

    return candidates, temp_paths


def cleanup(paths):
    for path in paths:
        try:
            path.unlink()
        except FileNotFoundError:
            pass


def build_import_trip(fields, files):
    name = fields.get("name", "").strip()
    if not name:
        raise ValueError("Trip name is required.")
    if not files:
        raise ValueError("Upload at least one image.")

    candidates, temp_paths = candidates_from_uploads(files)
    try:
        gps_candidates = [c for c in candidates if c.latitude is not None and c.longitude is not None]
        if fields.get("lat") and fields.get("lon"):
            latitude = float(fields["lat"])
            longitude = float(fields["lon"])
        elif gps_candidates:
            latitude = median([c.latitude for c in gps_candidates if c.latitude is not None])
            longitude = median([c.longitude for c in gps_candidates if c.longitude is not None])
        else:
            raise ValueError("No GPS EXIF found; enter latitude and longitude.")

        start_date, end_date = trip_date_range(candidates)
        slug = slugify(fields.get("slug") or name)
        photos = []

        for index, candidate in enumerate(candidates, start=1):
            dest = next_photo_filename(slug, index)
            copy_as_jpeg(candidate.path, dest, 92)
            photos.append(default_photo(len(photos), f"assets/photos/{dest.name}"))

        trip = normalize_trip({
            "name": name,
            "caption": fields.get("caption") or name,
            "geo": {
                "latitude": latitude,
                "longitude": longitude,
            },
            "close_zoom": fields.get("close_zoom") or DEFAULT_TRIP["close_zoom"],
            "hold_seconds": fields.get("hold_seconds") or DEFAULT_TRIP["hold_seconds"],
            "fade_seconds": fields.get("fade_seconds") or DEFAULT_TRIP["fade_seconds"],
            "photos": photos,
            **({"start_date": start_date, "end_date": end_date} if start_date else {}),
        })
        return trip, len(gps_candidates), len(candidates)
    finally:
        cleanup(temp_paths)


def append_or_replace_trip(trip, replace_index=None):
    with MANIFEST_LOCK:
        manifest = load_manifest()
        trips = manifest.setdefault("trips", [])
        if replace_index is None:
            trips.append(trip)
            index = len(trips) - 1
        else:
            index = int(replace_index)
            if index < 0 or index >= len(trips):
                raise IndexError("trip index out of range")
            trips[index] = trip
        save_manifest(manifest)
    return index


def add_photos_to_trip(index, files):
    if not files:
        raise ValueError("Upload at least one image.")

    candidates, temp_paths = candidates_from_uploads(files)
    try:
        with MANIFEST_LOCK:
            manifest = load_manifest()
            trips = manifest.setdefault("trips", [])
            trip = trips[int(index)]
            slug = slugify(trip["name"])
            photos = trip.setdefault("photos", [])
            gps_candidates = [c for c in candidates if c.latitude is not None and c.longitude is not None]
            start_date, end_date = trip_date_range(candidates)

            for offset, candidate in enumerate(candidates, start=len(photos) + 1):
                dest = next_photo_filename(slug, offset)
                copy_as_jpeg(candidate.path, dest, 92)
                photos.append(default_photo(len(photos), f"assets/photos/{dest.name}"))

            if gps_candidates and (not trip.get("geo") or not trip["geo"].get("latitude") or not trip["geo"].get("longitude")):
                trip["geo"] = {
                    "latitude": round(median([c.latitude for c in gps_candidates if c.latitude is not None]), 6),
                    "longitude": round(median([c.longitude for c in gps_candidates if c.longitude is not None]), 6),
                }
            if start_date and not trip.get("start_date"):
                trip["start_date"] = start_date
                trip["end_date"] = end_date

            trips[int(index)] = normalize_trip(trip)
            save_manifest(manifest)

        return len(candidates), len(gps_candidates)
    finally:
        cleanup(temp_paths)


def public_trip(trip):
    copy = dict(trip)
    copy["photos"] = [dict(photo) for photo in trip.get("photos", [])]
    return copy


class AdminHandler(BaseHTTPRequestHandler):
    server_version = "travelpi-admin/1.0"

    def log_message(self, fmt, *args):
        sys.stderr.write("admin: " + fmt % args + "\n")

    def send_json(self, data, status=HTTPStatus.OK):
        payload = json.dumps(data).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(payload)

    def send_error_json(self, status, message):
        self.send_json({"error": message}, status)

    def read_json(self):
        length = int(self.headers.get("Content-Length", "0"))
        if length <= 0:
            return {}
        return json.loads(self.rfile.read(length).decode("utf-8"))

    def serve_file(self, path, content_type=None):
        if not path.exists() or not path.is_file():
            self.send_error(HTTPStatus.NOT_FOUND)
            return
        payload = path.read_bytes()
        self.send_response(HTTPStatus.OK)
        self.send_header("Content-Type", content_type or mimetypes.guess_type(path.name)[0] or "application/octet-stream")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def do_GET(self):
        parsed = urlparse(self.path)
        path = parsed.path

        if path == "/":
            self.serve_file(TEMPLATE_ROOT / "index.html", "text/html; charset=utf-8")
            return

        if path.startswith("/static/"):
            rel = Path(unquote(path.removeprefix("/static/")))
            self.serve_file(STATIC_ROOT / rel)
            return

        if path.startswith("/photos/"):
            name = secure_filename(unquote(path.removeprefix("/photos/")))
            for root in (PREPARED_PHOTO_ROOT, SOURCE_PHOTO_ROOT):
                candidate = root / name
                if candidate.exists():
                    self.serve_file(candidate)
                    return
            self.send_error(HTTPStatus.NOT_FOUND)
            return

        if path == "/api/state":
            with MANIFEST_LOCK:
                manifest = load_manifest()
            with JOB_LOCK:
                jobs = [dict(job, log=list(job["log"])) for job in JOBS]
            self.send_json({
                "trips": [public_trip(trip) for trip in manifest.get("trips", [])],
                "manifest": str(MANIFEST_PATH.relative_to(ROOT)),
                "assets": asset_status(manifest),
                "jobs": jobs,
            })
            return

        self.send_error(HTTPStatus.NOT_FOUND)

    def do_POST(self):
        parsed = urlparse(self.path)
        path = parsed.path

        try:
            if path == "/api/import":
                fields, files = multipart_parts(self)
                trip, gps_count, total = build_import_trip(fields, files)
                index = append_or_replace_trip(trip)
                self.send_json({
                    "trip": public_trip(trip),
                    "index": index,
                    "gps_count": gps_count,
                    "photo_count": total,
                }, HTTPStatus.CREATED)
                return

            match = re.fullmatch(r"/api/trips/(\d+)/photos", path)
            if match:
                fields, files = multipart_parts(self)
                count, gps_count = add_photos_to_trip(int(match.group(1)), files)
                self.send_json({"photo_count": count, "gps_count": gps_count}, HTTPStatus.CREATED)
                return

            if path == "/api/trips":
                payload = self.read_json()
                trip = normalize_trip(payload.get("trip", payload))
                index = append_or_replace_trip(trip)
                self.send_json({"trip": public_trip(trip), "index": index}, HTTPStatus.CREATED)
                return

            if path == "/api/prepare":
                payload = self.read_json()
                job = prepare_job(payload.get("mode", "photos"))
                self.send_json({"job": job["id"]}, HTTPStatus.ACCEPTED)
                return

            if path == "/api/restart":
                job = restart_job()
                self.send_json({"job": job["id"]}, HTTPStatus.ACCEPTED)
                return

            self.send_error(HTTPStatus.NOT_FOUND)
        except Exception as exc:
            self.send_error_json(HTTPStatus.BAD_REQUEST, str(exc))

    def do_PUT(self):
        match = re.fullmatch(r"/api/trips/(\d+)", urlparse(self.path).path)
        if not match:
            self.send_error(HTTPStatus.NOT_FOUND)
            return

        try:
            payload = self.read_json()
            trip = normalize_trip(payload.get("trip", payload))
            index = append_or_replace_trip(trip, int(match.group(1)))
            self.send_json({"trip": public_trip(trip), "index": index})
        except Exception as exc:
            self.send_error_json(HTTPStatus.BAD_REQUEST, str(exc))

    def do_DELETE(self):
        match = re.fullmatch(r"/api/trips/(\d+)", urlparse(self.path).path)
        if not match:
            self.send_error(HTTPStatus.NOT_FOUND)
            return

        try:
            index = int(match.group(1))
            with MANIFEST_LOCK:
                manifest = load_manifest()
                trips = manifest.setdefault("trips", [])
                if index < 0 or index >= len(trips):
                    raise IndexError("trip index out of range")
                removed = trips.pop(index)
                save_manifest(manifest)
            self.send_json({"removed": removed.get("name", "")})
        except Exception as exc:
            self.send_error_json(HTTPStatus.BAD_REQUEST, str(exc))


class AdminServer(ThreadingHTTPServer):
    def server_bind(self):
        self.socket.bind(self.server_address)
        self.server_address = self.socket.getsockname()
        self.server_name = str(self.server_address[0])
        self.server_port = int(self.server_address[1])


def main():
    parser = argparse.ArgumentParser(description="Run the travelpi local admin web UI.")
    parser.add_argument("--host", default=os.environ.get("TRAVELPI_ADMIN_HOST", "0.0.0.0"))
    parser.add_argument("--port", type=int, default=int(os.environ.get("TRAVELPI_ADMIN_PORT", "8080")))
    args = parser.parse_args()

    ensure_manifest()
    SOURCE_PHOTO_ROOT.mkdir(parents=True, exist_ok=True)
    PREPARED_PHOTO_ROOT.mkdir(parents=True, exist_ok=True)
    server = AdminServer((args.host, args.port), AdminHandler)
    print(f"travelpi admin listening on http://{args.host}:{args.port}")
    server.serve_forever()


if __name__ == "__main__":
    main()
