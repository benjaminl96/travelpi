#!/usr/bin/env sh
set -eu

RAYLIB_VERSION="${RAYLIB_VERSION:-6.0}"
RAYLIB_DIR="${RAYLIB_DIR:-$HOME/src/raylib}"

mkdir -p "$(dirname "$RAYLIB_DIR")"

if [ ! -d "$RAYLIB_DIR/.git" ]; then
  git clone https://github.com/raysan5/raylib.git "$RAYLIB_DIR"
fi

cd "$RAYLIB_DIR"
git fetch --tags --depth 1 origin "refs/tags/$RAYLIB_VERSION:refs/tags/$RAYLIB_VERSION" || true
git checkout "$RAYLIB_VERSION"

cd src
make clean || true
make PLATFORM=PLATFORM_DRM \
  GRAPHICS=GRAPHICS_API_OPENGL_ES2 \
  RAYLIB_LIBTYPE=STATIC \
  BUILD_MODE=RELEASE
sudo make install PLATFORM=PLATFORM_DRM RAYLIB_LIBTYPE=STATIC
sudo ldconfig
