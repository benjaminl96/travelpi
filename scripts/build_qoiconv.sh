#!/usr/bin/env sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
SRC_DIR="$ROOT/.asset-cache/qoi"
BIN_DIR="$ROOT/.asset-cache/bin"

mkdir -p "$SRC_DIR" "$BIN_DIR"

curl -L --fail -o "$SRC_DIR/qoi.h" \
  https://raw.githubusercontent.com/phoboslab/qoi/master/qoi.h
curl -L --fail -o "$SRC_DIR/qoiconv.c" \
  https://raw.githubusercontent.com/phoboslab/qoi/master/qoiconv.c
curl -L --fail -o "$SRC_DIR/stb_image.h" \
  https://raw.githubusercontent.com/nothings/stb/master/stb_image.h
curl -L --fail -o "$SRC_DIR/stb_image_write.h" \
  https://raw.githubusercontent.com/nothings/stb/master/stb_image_write.h

cc "$SRC_DIR/qoiconv.c" -std=c99 -O3 -I"$SRC_DIR" -o "$BIN_DIR/qoiconv"
printf '%s\n' "$BIN_DIR/qoiconv"
