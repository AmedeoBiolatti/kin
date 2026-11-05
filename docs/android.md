# Android builds

Status: experimental, 2026-11-05. 2D games build and run on arm64 phones
(checked on a Samsung Galaxy A55: Android 16, Xclipse 530, Vulkan 1.3) with
kin's SDL_GPU renderer on Vulkan. APKs are debug-signed, for sideloading while
a game is made; there is no release (Play) packaging yet. Touch is read as on
any touch screen ([input](input.md#touch)): the first finger is the pointer,
gestures bind like keys; Back is Escape.

## What is needed

- The Android SDK command-line tools, with `platform-tools`, `platforms;android-35`,
  `build-tools;35.0.0` and an NDK **r29 or newer** (`ndk;29.0.14206865`): r28's
  C++ library lacks `std::stop_token` and floating-point `std::from_chars`,
  which kin uses. The default SDK folder is `~/Android/Sdk` (`ANDROID_HOME`).
- A JDK 17 or newer for `javac`, `jar` and `keytool` (`JAVA_HOME`, or
  `~/Android/jdk-21`).
- `glslc` on the build machine, for the engine's shaders and the content's.
- A phone with developer options and USB or wireless debugging on. Wireless
  needs no USB permissions on Linux: *Developer options → Wireless debugging →
  Pair device with pairing code*, then `adb pair <ip>:<port> <code>`; adb finds
  the phone itself afterwards.

## Building and running

```sh
tools/android/configure.sh                      # kin's own games, in out/build-android
tools/android/run.sh out/build-android hex_demo # build, install, start, follow the log

# A game outside kin (its CMakeLists.txt adds kin from KIN_DIR):
tools/android/configure.sh --source ~/Desktop/kin_projects/lantern_vale --build out/build-lv-android
tools/android/run.sh out/build-lv-android lantern_vale
```

`configure.sh` first makes a desktop build with `kin_pack` (the APK build runs
it on the build machine), then configures the Android build with the NDK's
toolchain, reusing the desktop build's dependency sources. `run.sh` takes
`--no-build` and `--no-log`; with several devices set `ANDROID_SERIAL`.

## How a game becomes an app

`kin_game(<target> ...)` on Android calls `kin_android_app` (cmake/kin_android.cmake):

- The game's executable target is linked as the app's `lib/arm64-v8a/libmain.so`
  (`-shared`, its sources `-fPIC`, 16 KB pages). SDL is linked into it
  statically. A small C file (cmake/android/sdl_main_shim.c) gives SDL's Java
  activity the `SDL_main` it calls, which calls the game's own `main()`.
- `<target>_apk` builds `android/<target>/<name>.apk` with the SDK's tools
  directly (javac, d8, aapt2, zipalign, apksigner); Gradle is not used. The
  activity is SDL's, subclassed as `org.kin.runtime.KinActivity`. The
  application id is `org.kin.<name>` (`KIN_ANDROID_ID_PREFIX`).
- The content becomes `assets/<name>.kinpak`, stored uncompressed, which
  `find_content_root` opens in place from the APK
  (`ContentPack::open_android_asset`) and mounts at `/android_asset/<name>`.
  The content's `.frag.glsl`/`.comp.glsl` shaders get their `.spv` made by the
  build machine's glslc when the content has no current one: a phone has no
  glslc.
- The APK is signed with the debug key `~/.android/debug.keystore` (made if
  missing).

Settings: `KIN_ANDROID_SDK`, `KIN_ANDROID_JAVA_HOME`, `KIN_HOST_TOOLS_DIR`,
`KIN_ANDROID_ID_PREFIX`, `KIN_ANDROID_TARGET_SDK` (35), `KIN_ANDROID_ORIENTATION`
(the manifest's; the window's shape decides at run time, below).

## What differs on a phone

- **Window:** always fullscreen at the display's pixels, system bars hidden. A
  window wider than tall turns the phone to landscape, taller to portrait,
  unless the game set `SDL_HINT_ORIENTATIONS`.
- **Frame rate:** always presented with vsync, at the display's rate (120 Hz on
  the A55); `App::set_max_fps` lowers it.
- **Saves:** `user_data_dir` is the app's private files folder
  (`/data/user/0/<id>/files`), removed with the app.
- **Fonts:** the system UI font is Roboto from `/system/fonts`.
- **Back, the cutout, the keyboard:** Back is `Key::Escape`; lay UI out in
  `Window::safe_area()`, clear of the camera cutout and the system's edges; a
  ui2 text field opens the on-screen keyboard
  ([input](input.md#touch)).
- **Logs:** kin's log goes to logcat (tag `SDL/APP`).
- **Leaving the screen:** the app stops while away (SDL blocks its thread) and
  may be ended there without warning. `App::on_app_event` and the scenes'
  `on_app_background` / `on_app_foreground` / `on_low_memory` say so as it
  happens; save in `on_app_background`. The time away is not played as game
  time afterwards. Android also reports low memory each time an app leaves
  the screen.
- **Not on a phone:** child processes (`Process`), the system's file dialogs,
  compiling shaders at run time (glslc), hot reload of the build machine's
  content folder.

## Known gaps

- On-screen controls (a stick and buttons over the game) and touch scrolling
  of ui2 scroll boxes.
- The first frame after returning to the screen logs one Vulkan
  `dequeueBuffer failed` while SDL_GPU replaces the old surface's swapchain;
  drawing continues.
- BC-compressed textures are not checked against the phone's support.
- 3D, release packaging (AAB, signing). CI builds hex_demo's APK
  (.github/workflows/android.yml) but runs nothing on a phone.
