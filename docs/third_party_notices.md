# Third-Party Notices

Kin's own source is under the [MIT License](../LICENSE). The repository contains no
third-party code: CMake downloads the dependencies below at configure time (see
[cmake/dependencies.cmake](../cmake/dependencies.cmake)) and links them statically
into the engine.

If you distribute a binary built with kin, include the license texts of these
components with it. Each license is in the fetched source tree, under
`<build>/_deps/<name>-src/`.

## Direct dependencies

| Component | Version | License | Copyright |
|---|---|---|---|
| [SDL](https://github.com/libsdl-org/SDL) | 3.4.10 | zlib | Sam Lantinga |
| [SDL_image](https://github.com/libsdl-org/SDL_image) | 3.4.4 | zlib | Sam Lantinga |
| [SDL_ttf](https://github.com/libsdl-org/SDL_ttf) | 3.2.2 | zlib | Sam Lantinga |
| [flecs](https://github.com/SanderMertens/flecs) | 4.0.3 | MIT | Sander Mertens |
| [Box2D](https://github.com/erincatto/box2d) | 2.4.1 | MIT | Erin Catto |
| [Lua](https://www.lua.org) | 5.4.6 | MIT | Lua.org, PUC-Rio |
| [sol2](https://github.com/ThePhD/sol2) | 3.3.1 | MIT | Rapptz, ThePhD, and contributors |

## Libraries bundled by SDL_image and SDL_ttf

Kin builds SDL_image and SDL_ttf with their vendored libraries enabled, so these
are compiled in as well:

| Component | Pulled in by | License |
|---|---|---|
| [libpng](http://www.libpng.org/pub/png/libpng.html) | SDL_image | PNG Reference Library License v2 |
| [zlib](https://zlib.net) | SDL_image | zlib |
| [stb_image](https://github.com/nothings/stb) | SDL_image | Public domain / MIT |
| [FreeType](https://freetype.org) | SDL_ttf | FreeType License (FTL) or GPLv2 — kin uses it under the FTL |
| [HarfBuzz](https://harfbuzz.github.io) | SDL_ttf | "Old MIT" |
| [plutosvg](https://github.com/sammycage/plutosvg) and [plutovg](https://github.com/sammycage/plutovg) | SDL_ttf | MIT |

The FreeType License asks that documentation for binary distributions credit it,
for example: "Portions of this software are copyright © 2026 The FreeType
Project (https://freetype.org). All rights reserved."
