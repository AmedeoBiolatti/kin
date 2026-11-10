#!/usr/bin/env bash
# Builds, installs and starts a kin game's APK on the connected Android device,
# then follows its log (docs/android.md).
#
#   tools/android/run.sh <build dir> <game target> [--no-build] [--no-log] [-- adb args]
#
# ADB defaults to $ANDROID_HOME/platform-tools/adb (or ~/Android/Sdk). With
# several devices, set ANDROID_SERIAL.
set -euo pipefail

if [ $# -lt 2 ]; then
    sed -n '2,9p' "$0"
    exit 2
fi
build="$1"
target="$2"
shift 2
do_build=1
do_log=1
for arg in "$@"; do
    case "$arg" in
        --no-build) do_build=0 ;;
        --no-log) do_log=0 ;;
        *) echo "unknown argument: $arg" >&2; exit 2 ;;
    esac
done

sdk="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-$HOME/Android/Sdk}}"
adb="${ADB:-$sdk/platform-tools/adb}"

if [ "$do_build" = 1 ]; then
    cmake --build "$build" --target "${target}_apk"
fi

apk=$(ls "$build/android/$target/"*.apk 2>/dev/null | head -1 || true)
if [ -z "$apk" ]; then
    echo "no APK in $build/android/$target (build ${target}_apk)" >&2
    exit 1
fi
# The application id is in the manifest kin_android_app wrote.
app_id=$(sed -n 's/.*package="\([^"]*\)".*/\1/p' "$build/android/$target/work/AndroidManifest.xml" | head -1)

"$adb" install -r -t "$apk"
"$adb" shell am force-stop "$app_id" || true
"$adb" logcat -c || true
"$adb" shell am start -W -n "$app_id/org.kin.runtime.KinActivity"

if [ "$do_log" = 1 ]; then
    pid=""
    for _ in $(seq 1 50); do
        pid=$("$adb" shell pidof "$app_id" 2>/dev/null | tr -d '\r' || true)
        [ -n "$pid" ] && break
        sleep 0.1
    done
    if [ -z "$pid" ]; then
        echo "$app_id is not running; recent log:" >&2
        "$adb" logcat -d -t 200 | grep -E "AndroidRuntime|SDL|DEBUG|libc|$app_id" >&2 || true
        exit 1
    fi
    echo "--- log of $app_id (pid $pid); Ctrl-C to stop ---"
    exec "$adb" logcat --pid="$pid" -v brief
fi
