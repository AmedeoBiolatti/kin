#!/usr/bin/env bash
# Configures an Android (arm64-v8a) build of kin, or of a game built on kin,
# after making the desktop tools the APK build runs (kin_pack) in a host build
# (docs/android.md).
#
#   tools/android/configure.sh [--source <dir>] [--build <dir>] [--host <dir>] [-- <cmake args>]
#
#   --source  the CMake project (default: this kin checkout; a game's folder
#             for a game outside kin, which then gets -DKIN_DIR=<this kin>)
#   --build   the Android build folder (default: out/build-android)
#   --host    the desktop build holding bin/kin_pack (default: out/build-host;
#             configured and built here when it has none)
#
# Uses ANDROID_HOME (default ~/Android/Sdk), its newest NDK (r29 or newer: kin
# needs its C++ library), and JAVA_HOME (default ~/Android/jdk-21 if present).
set -euo pipefail

kin="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source_dir="$kin"
build="$kin/out/build-android"
host="$kin/out/build-host"
extra=()
while [ $# -gt 0 ]; do
    case "$1" in
        --source) source_dir="$(cd "$2" && pwd)"; shift 2 ;;
        --build) build="$2"; shift 2 ;;
        --host) host="$2"; shift 2 ;;
        --) shift; extra=("$@"); break ;;
        *) sed -n '2,16p' "$0"; exit 2 ;;
    esac
done

sdk="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-$HOME/Android/Sdk}}"
ndk="${ANDROID_NDK_HOME:-$(ls -d "$sdk"/ndk/* 2>/dev/null | sort -V | tail -1)}"
if [ -z "$ndk" ] || [ ! -f "$ndk/build/cmake/android.toolchain.cmake" ]; then
    echo "no NDK in $sdk/ndk (sdkmanager \"ndk;29.0.14206865\")" >&2
    exit 1
fi
ndk_major=$(sed -n 's/^Pkg.Revision *= *\([0-9]*\).*/\1/p' "$ndk/source.properties")
if [ "${ndk_major:-0}" -lt 29 ]; then
    echo "NDK $ndk is r$ndk_major; kin needs r29 or newer (std::stop_token, floating from_chars)" >&2
    exit 1
fi
java_home="${JAVA_HOME:-}"
if [ -z "$java_home" ] && [ -x "$HOME/Android/jdk-21/bin/javac" ]; then
    java_home="$HOME/Android/jdk-21"
fi
glslc="$(command -v glslc || true)"
export TMPDIR="${TMPDIR:-$kin/out/tmp}"
mkdir -p "$TMPDIR"

# The desktop tools, with only what they need.
if [ ! -x "$host/bin/kin_pack" ]; then
    cmake -S "$kin" -B "$host" -G Ninja -DCMAKE_BUILD_TYPE=Release -DKIN_BUILD_GAMES=OFF \
        -DKIN_BUILD_EXAMPLES=OFF -DKIN_BUILD_TESTS=OFF -DKIN_BUILD_BENCHMARKS=OFF
    cmake --build "$host" --target kin_pack
fi

args=(
    -DCMAKE_TOOLCHAIN_FILE="$ndk/build/cmake/android.toolchain.cmake"
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DANDROID_STL=c++_static
    -DCMAKE_BUILD_TYPE=Release
    -DKIN_HOST_TOOLS_DIR="$host/bin" -DKIN_ANDROID_SDK="$sdk"
    -DKIN_BUILD_EXAMPLES=OFF -DKIN_BUILD_TESTS=OFF -DKIN_BUILD_BENCHMARKS=OFF
)
[ -n "$java_home" ] && args+=(-DKIN_ANDROID_JAVA_HOME="$java_home")
[ -n "$glslc" ] && args+=(-DKIN_GLSLC="$glslc")
[ "$source_dir" != "$kin" ] && args+=(-DKIN_DIR="$kin")
# The dependencies' sources from the host build: fetched once.
if [ -d "$host/_deps" ]; then
    for src in "$host"/_deps/*-src; do
        name=$(basename "$src" -src | tr 'a-z' 'A-Z')
        args+=(-DFETCHCONTENT_SOURCE_DIR_"$name"="$src")
    done
fi

cmake -S "$source_dir" -B "$build" -G Ninja "${args[@]}" "${extra[@]}"
echo
echo "Configured $build. Build and run a game's APK with:"
echo "  tools/android/run.sh $build <game target>"
