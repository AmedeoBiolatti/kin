#!/usr/bin/env bash
# Packages kin needs to build on Ubuntu 24.04 (SDL3's windowing and audio
# backends), plus any extra packages passed as arguments.
#
# KIN_APT_ARCHIVES, when set, is the folder apt keeps downloaded packages in, so
# a cache can hold them between runs (see .github/actions/linux-deps).
#
# The runners' Ubuntu mirror (azure.archive.ubuntu.com) sometimes serves a few
# of the larger packages at 40 kB/s, and a run can wait 15 minutes for 37 MB. So
# the download is a step of its own with a deadline, and when it misses the
# deadline the packages come from the main archive instead; installing what is
# already downloaded takes seconds.
set -euo pipefail
apt_options=(-o Acquire::Retries=3 -o Acquire::http::Timeout=30)
if [ -n "${KIN_APT_ARCHIVES:-}" ]; then
  mkdir -p "$KIN_APT_ARCHIVES"
  apt_options+=(-o "Dir::Cache::Archives=$KIN_APT_ARCHIVES")
fi
packages=(ninja-build pkg-config
  libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxfixes-dev libxi-dev
  libxss-dev libxtst-dev libxkbcommon-dev libwayland-dev wayland-protocols
  libdecor-0-dev libegl1-mesa-dev libgl1-mesa-dev libgles2-mesa-dev
  libdrm-dev libgbm-dev libasound2-dev libpulse-dev libpipewire-0.3-dev
  libdbus-1-dev libudev-dev libibus-1.0-dev
  "$@")
download=(apt-get install -y --download-only --no-install-recommends "${apt_options[@]}" "${packages[@]}")

sudo apt-get update "${apt_options[@]}"
if ! sudo timeout "${KIN_APT_DEADLINE:-150}" "${download[@]}"; then
  echo "::warning::The package download missed its deadline; trying archive.ubuntu.com"
  sudo sed -i 's|//azure\.archive\.ubuntu\.com/|//archive.ubuntu.com/|g' /etc/apt/sources.list /etc/apt/sources.list.d/*.sources 2>/dev/null || true
  sudo apt-get update "${apt_options[@]}"
  sudo "${download[@]}"
fi
sudo apt-get install -y --no-install-recommends "${apt_options[@]}" "${packages[@]}"
