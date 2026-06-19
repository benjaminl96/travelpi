#!/usr/bin/env python3
import shutil
import subprocess
import sys
from pathlib import Path


def repo_root() -> Path:
    return Path(__file__).resolve().parents[1]


def main():
    root = repo_root()
    manifest = root / "config/trips.json"
    example = root / "config/trips.example.json"

    if not example.exists():
        raise SystemExit(f"missing example manifest: {example}")

    if manifest.exists():
        print(f"using existing {manifest.relative_to(root)}")
    else:
        manifest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(example, manifest)
        print(f"created {manifest.relative_to(root)} from {example.relative_to(root)}")

    subprocess.run(
        [
            sys.executable,
            str(root / "scripts/generate_travel_config.py"),
            "--manifest",
            str(manifest),
            "--output",
            str(root / "src/travel_config.c"),
        ],
        cwd=root,
        check=True,
    )
    print("generated src/travel_config.c")


if __name__ == "__main__":
    main()
