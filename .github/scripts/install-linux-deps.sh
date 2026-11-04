#!/usr/bin/env bash
# Packages kin needs to build on Ubuntu 24.04 (SDL3's windowing and audio
# backends), plus any extra packages passed as arguments.
#
# KIN_APT_ARCHIVES, when set, is the folder apt keeps downloaded packages in, so
# a cache can hold them between runs (see .github/actions/linux-deps). The
# timeouts and retries are because the runners' Ubuntu mirror sometimes slows
# to a crawl: one run fetched 37 MB in 15 minutes.
set -euo pipefail
apt_options=(-o Acquire::Retries=3 -o Acquire::http::Timeout=30)
if [ -n "${KIN_APT_ARCHIVES:-}" ]; then
  mkdir -p "$KIN_APT_ARCHIVES"
  apt_options+=(-o "Dir::Cache::Archives=$KIN_APT_ARCHIVES")
fi
sudo apt-get update "${apt_options[@]}"
sudo apt-get install -y --no-install-recommends "${apt_options[@]}" ninja-build pkg-config \
  libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxfixes-dev libxi-dev \
  libxss-dev libxtst-dev libxkbcommon-dev libwayland-dev wayland-protocols \
  libdecor-0-dev libegl1-mesa-dev libgl1-mesa-dev libgles2-mesa-dev \
  libdrm-dev libgbm-dev libasound2-dev libpulse-dev libpipewire-0.3-dev \
  libdbus-1-dev libudev-dev libibus-1.0-dev \
  "$@"
