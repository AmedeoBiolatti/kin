#!/usr/bin/env bash
# Packages kin needs to build on Ubuntu 24.04 (SDL3's windowing and audio
# backends), plus any extra packages passed as arguments.
set -euo pipefail
sudo apt-get update
sudo apt-get install -y --no-install-recommends ninja-build pkg-config \
  libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxfixes-dev libxi-dev \
  libxss-dev libxtst-dev libxkbcommon-dev libwayland-dev wayland-protocols \
  libdecor-0-dev libegl1-mesa-dev libgl1-mesa-dev libgles2-mesa-dev \
  libdrm-dev libgbm-dev libasound2-dev libpulse-dev libpipewire-0.3-dev \
  libdbus-1-dev libudev-dev libibus-1.0-dev \
  "$@"
