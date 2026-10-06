# Shipping a game

A game built with kin ships as a folder: the executable, its content packed into
one `.kinpak` archive beside it, and the license texts it must carry.

```text
signal_siege-0.2.5-linux-x86_64/
  signal_siege             (signal_siege.exe on Windows)
  signal_siege.kinpak      the game's content: images, sounds, fonts, text, scripts, data
  licenses/                kin's license and those of the libraries built into it
```

Nothing else is needed. Kin and its libraries are linked into the executable,
the engine's GPU shaders are built into it, and the game finds its content
beside itself whatever folder it is started from. The folder can be zipped,
put in an installer or uploaded as a Steam depot as it is.

## Setting a game up

In CMake, `kin_game` sets up an executable as a game:

```cmake
add_executable(my_game src/main.cpp)
target_link_libraries(my_game PRIVATE kin::engine)
kin_game(my_game TITLE "My Game")   # content/ beside this CMakeLists.txt
```

| Argument | Default | |
|---|---|---|
| `CONTENT <dir>` | `content/` | The game's content folder |
| `NAME <name>` | the target | Its name beside the executable: `<name>.kinpak` or `<name>/` |
| `TITLE <text>` | the name | The product name in the Windows version info |
| `VERSION <x.y.z>` | the project's | Version info, and the package's name |
| `ICON <file.ico>` | none | The Windows executable's icon |
| `FILES <file>...` | none | More files to ship beside the executable (a readme) |
| `NO_CONTENT` | | The game has no content folder |

In the game, `find_content_root` finds the content and gives the folder to read
it from:

```cpp
#include <kin/assets/content.hpp>

int main(int argc, char** argv) {
    const std::filesystem::path root = kin::find_content_root(KIN_GAME_CONTENT, argc, argv);
    if (root.empty()) {
        return 1; // logged: where it looked
    }
    kin::AssetManager assets{root};
    kin::Localization l10n;
    l10n.load_directory(root / "lang", errors, &files);
    ...
}
```

`KIN_GAME_CONTENT` is defined by `kin_game` on the game's target. The first of
these that exists is used:

1. `--content=<folder or .kinpak>` on the command line, then the `KIN_CONTENT`
   environment variable: to try a pack, or another set of content.
2. The source content folder, in development builds. It is read in place, so
   edits hot-reload ([assets](assets.md#hot-reloading-game-data)). Shipping
   builds leave this out.
3. `<name>.kinpak` beside the executable.
4. `<name>/` beside the executable: the content as loose files.

A pack at `dir/my_game.kinpak` is mounted at `dir/my_game`, so `root` is the
same folder in every case and the game's paths do not change.

Games set up with the older `kin_configure_game_assets(target name)` get all of
this too, with their `assets/` folder as the content.

## Reading content

The engine reads content through one layer that answers from a mounted pack,
or from disk for any other path. Every loader goes through it: `AssetManager`
and `AssetServer`, images, WAV clips, TTF fonts (`load_ttf_font`), language
files, Lua scripts and `require`, themes, sprite, audio and particle catalogs,
tilemaps, prefabs, scenes, dialogue, SVG, LUTs, input maps and game info.

A game reading its own files does the same with `kin/assets/content.hpp`:

| Function | |
|---|---|
| `read_content_file(path)` | The bytes, or nullopt |
| `read_content_text(path)` | The same, with `\r\n` read as `\n` |
| `view_content_file(path)` | The bytes without a copy when packed |
| `content_file_exists`, `content_directory_exists`, `content_file_size` | |
| `list_content_files(dir, recursive)` | The files in a folder, sorted |

`std::ifstream` and `std::filesystem` see only the disk, so they miss a packed
file. Content is read-only: saves and settings go in `user_data_dir(app_id)`
([platform](platform.md#the-users-data)), which is always on disk.

Packed files never change, so hot reload watches nothing in a pack. Paths in a
pack are case-sensitive on every platform. Windows finds `Hero.png` when asked
for `hero.png`, but a pack does not. Kin logs the near miss, which shows the
mistake on the machine where it was made.

## Packs

A `.kinpak` holds a folder's files uncompressed, each aligned to 16 bytes,
with an index and a CRC-32 per file. The game maps it into memory: opening it
reads only the index, and a file costs nothing until it is read. Files and
folders whose names start with `.` are left out.

`kin_pack` makes and checks them:

```sh
kin_pack create content/ my_game.kinpak   # skipped when the pack is up to date (--force)
kin_pack list my_game.kinpak              # size and path of each file
kin_pack verify my_game.kinpak            # every file against its checksum
kin_pack extract my_game.kinpak out/      # back to a folder
```

Packs can also be mounted by hand, for example a mod or a DLC over the base
content:

```cpp
std::string error;
if (auto pack = kin::ContentPack::open(dir / "expansion.kinpak", &error)) {
    kin::mount_content_pack(root / "expansion", std::move(pack));
}
```

## Building a package

The `ship` preset makes a shipping build in `build-ship/`. `<game>_package`
builds the game, packs its content, and archives the folder under
`build-ship/packages/`:

```sh
cmake --preset ship
cmake --build --preset ship --target my_game_package
# build-ship/packages/my_game-1.0.0-linux-x86_64.tar.gz
```

On Windows use the `ship-vs` preset (Visual Studio 2022), or `ship` from a
developer prompt. It makes `my_game-1.0.0-windows-x86_64.zip`.

`<game>_content` builds only the pack, beside the executable. `cmake --install
build-ship --component my_game --prefix <dir>` installs the same folder
without archiving it. With `KIN_PACK_CONTENT=OFF` the content ships as a loose
`<name>/` folder instead of a pack.

## What a shipping build changes

`KIN_SHIPPING=ON`, which the `ship` preset sets, changes these defaults (in a
new build folder):

| | Development | Shipping |
|---|---|---|
| Content | Read from the source folder | Read from beside the executable |
| `--server` (`KIN_ENABLE_AGENT_SERVER`) | Built in | Refused |
| `--probe-render`, `--check-determinism` | Built in | Left out |
| Profiling in Release (`KIN_ENABLE_RELEASE_PROFILING`) | On | Off |
| Content pack | Built on request (`<game>_content`) | Built with the game |
| Log | The console | Also `log.txt` in the game's user data folder (the previous run's is kept as `log.previous.txt`), when the game sets `GameInfo::id` |
| GPU shaders (`KIN_REQUIRE_GPU_SHADERS`) | Optional | Required: glslc or `KIN_SPIRV_DIR` |
| Windows | Console app, shared runtime | Windowed app, static runtime |
| Linux | | libstdc++ and libgcc linked in |
| Runtime GLSL compiles (`shader_compiler.hpp`) | With the glslc found at build time, or `KIN_GLSLC` | Only with `KIN_GLSLC` |

`--headless`, `--seed`, `--frames` and `--report` stay, so a player's bug can
still be replayed from a seed. Code that links `kin::engine` sees `KIN_SHIPPING`
defined in a shipping build, for a game's own developer tools.

## GPU shaders

The engine's shaders are compiled with glslc and built into it. Without glslc a
development build still works: the GPU backend has no shaders, and kin falls
back to SDL_Renderer. A shipping build stops at configure instead, unless
`KIN_REQUIRE_GPU_SHADERS=OFF` says to ship without them.

glslc comes with the Vulkan SDK, as the `glslc` package on Debian and Ubuntu,
and on Windows also from MSYS2 (`pacman -S mingw-w64-ucrt-x86_64-shaderc`, then
`-DKIN_GLSLC=C:/msys64/ucrt64/bin/glslc.exe`). On a build machine where glslc
does not run, compile the SPIR-V elsewhere (it is the same on every machine)
and point the build at it:

```sh
cmake -DOUTPUT_DIR=$PWD/spirv -P cmake/kin_spirv.cmake   # where glslc runs
cmake --preset ship -DKIN_SPIRV_DIR=$PWD/spirv            # on the build machine
```

A game's own shaders can be built in the same way:

```cmake
kin_compile_glsl(spirv OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders" SOURCES shaders/water.frag.glsl)
kin_embed_files(my_game NAME my_game_shader FILES ${spirv})
```

```cpp
#include "my_game_shader.hpp"
const std::span<const unsigned char> spirv = my_game_shader("water.frag.spv"); // empty without glslc
```

Shaders can also be kept in the content as `.spv` files and read with
`read_content_file`.

## Platforms

**Windows.** The game is a windowed app (no console window) on the static C++
runtime, so players need no Visual C++ redistributable. Its version info comes
from `TITLE` and `VERSION`, and `ICON` sets its icon. The executable is not
code-signed, so SmartScreen warns about it until it is signed or has built up
a reputation.

**Linux.** libstdc++ and libgcc are linked in, and glibc stays shared, so the
oldest system the game runs on has the glibc of the machine that built it: a
build on Ubuntu 24.04 needs glibc 2.38. Build in the
[Steam Runtime SDK](https://gitlab.steamos.cloud/steamrt/sniper/sdk) (sniper,
glibc 2.31) for Steam Deck, Steam's Linux runtime and older distributions. Its
GCC 14 builds kin, but its CMake is too old and it has no glslc, so bring a
CMake (3.22 or newer) and Ninja, and the SPIR-V from `kin_spirv.cmake`:

```sh
cmake -DOUTPUT_DIR=$PWD/spirv -P cmake/kin_spirv.cmake
docker run --rm -v "$PWD:/src" -w /src -e CC=gcc-14 -e CXX=g++-14 \
    registry.gitlab.steamos.cloud/steamrt/sniper/sdk bash -c '
    export PATH=/src/.ci-tools/bin:$PATH    # CMake and Ninja, unpacked here
    cmake --preset ship -DKIN_SPIRV_DIR=/src/spirv
    cmake --build --preset ship --target my_game_package'
```

CI builds Signal Siege this way (`.github/workflows/ci.yml`, `ship-linux`),
checks that it asks for nothing newer than glibc 2.31, and runs it in the
runtime.

**Steam.** The package's folder is a depot as it is: the executable, its pack
and `licenses/`.

## Licenses

`licenses/` holds kin's license, a copy of
[third-party notices](third_party_notices.md), and the license text of each
library linked into the game. Those texts come from the dependencies' sources.
If a dependency moves a license file, configuring fails and names it.

## Not yet

- Compressed or encrypted packs (images and sounds are already compressed; a
  pack only gathers them).
- A pack appended to the executable, for a single-file game.
- Installers, code signing, a Linux `.desktop` file and icon, macOS bundles.
