#!/usr/bin/env sh
set -eu

sudo apt-get update
sudo apt-get install -y \
  build-essential \
  cmake \
  git \
  libheif-examples \
  libheif-plugin-aomdec \
  libheif-plugin-libde265 \
  libimage-exiftool-perl \
  libasound2-dev \
  libdrm-dev \
  libegl1-mesa-dev \
  libgbm-dev \
  libgles2-mesa-dev \
  mesa-common-dev \
  python3-pil \
  pkg-config
