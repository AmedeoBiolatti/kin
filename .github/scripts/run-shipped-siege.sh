#!/usr/bin/env bash
# Unpacks Signal Siege's package in a new folder and runs it from there, in
# French. French shows only if the pack's French file loaded (the game falls
# back to English without it), and --fail-on-missing-text fails the run if
# any French text is missing.
# Usage: run-shipped-siege.sh <packages folder> <folder to run in>
set -euo pipefail
packages=$1
work=$2
mkdir -p "$work"
cd "$work"
for archive in "$packages"/signal_siege-*.zip "$packages"/signal_siege-*.tar.gz; do
  if [ -e "$archive" ]; then
    cmake -E tar xf "$archive"
  fi
done
game=$(ls -d signal_siege-*/)
SDL_VIDEO_DRIVER=dummy SDL_AUDIO_DRIVER=dummy "./${game}signal_siege" --headless --frames=120 --seed=7 \
  --locale=fr --fail-on-missing-text --mute --report=report.json
grep -q '"status": "ok"' report.json
grep -q '"locale": "fr"' report.json
test -f "${game}licenses/SDL.txt"
echo "Signal Siege ran from $work/$game"
