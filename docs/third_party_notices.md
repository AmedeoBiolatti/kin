# Third-Party Notices

Kin's own source is under the [MIT License](../LICENSE). The repository contains no
third-party code but one table of data (see [Embedded data](#embedded-data)): CMake downloads the dependencies
below at configure time (see
[cmake/dependencies.cmake](../cmake/dependencies.cmake)) and links them statically
into the engine.

If you distribute a binary built with kin, include the license texts of these
components with it. Each license is in the fetched source tree, under
`<build>/_deps/<name>-src/`.
The experimental [desktop package helper](shipping.md) includes these notices
from the dependency sources automatically. Add notices for your own assets,
fonts and additional libraries through its `LICENSE_FILES` argument.

## Direct dependencies

| Component | Version | License | Copyright |
|---|---|---|---|
| [SDL](https://github.com/libsdl-org/SDL) | 3.4.10 | zlib | Sam Lantinga |
| [SDL_image](https://github.com/libsdl-org/SDL_image) | 3.4.4 | zlib | Sam Lantinga |
| [SDL_ttf](https://github.com/libsdl-org/SDL_ttf) | 3.2.2 | zlib | Sam Lantinga |
| [flecs](https://github.com/SanderMertens/flecs) | 4.0.5 | MIT | Sander Mertens |
| [AccessKit C](https://github.com/AccessKit/accesskit-c/tree/0.23.2) (optional `KIN_ENABLE_ACCESSIBILITY`) | 0.23.2 (`99023c87eca49531a8e1efc4657e35779257a463`) | MIT or Apache-2.0, with BSD Chromium-derived definitions | AccessKit contributors |
| [Box2D](https://github.com/erincatto/box2d) | 2.4.1 | MIT | Erin Catto |
| [Lua](https://www.lua.org) | 5.4.6 | MIT | Lua.org, PUC-Rio |
| [sol2](https://github.com/ThePhD/sol2) | 3.3.1 | MIT | Rapptz, ThePhD, and contributors |
| [earcut.hpp](https://github.com/mapbox/earcut.hpp) | 3.2.4 | ISC | Mapbox |
| [dr_libs](https://github.com/mackron/dr_libs) (dr_wav, dr_flac, dr_mp3) | commit dfe8377 (dr_wav 0.14.6, dr_flac 0.13.4, dr_mp3 0.7.4 pre-release) | Public domain (Unlicense) or MIT-0 | David Reid |
| [stb_vorbis](https://github.com/nothings/stb) | 1.22 (commit 2c980bb) | Public domain (Unlicense) or MIT | Sean Barrett |
| [cgltf](https://github.com/jkuhlmann/cgltf) (with `KIN_EXPERIMENTAL_3D`) | 1.14 | MIT | Johannes Kuhlmann |
| [meshoptimizer](https://github.com/zeux/meshoptimizer) (with `KIN_EXPERIMENTAL_3D`; simplifier, meshlet builder, cluster partitioner, index and vertex-cache tools) | 0.25 | MIT | Arseny Kapoulkine |
| [Jolt Physics](https://github.com/jrouwe/JoltPhysics) (with `KIN_EXPERIMENTAL_3D`; the rigid body engine behind `kin/physics3d`) | 5.5.0 | MIT | Jorrit Rouwe |

Native GPU builds use a marked, generated variant of SDL's Vulkan translation unit
to include compute shader reads in sampler barriers. The original source and zlib
notice are retained; shared/fetched sources are not edited. The guarded patch
is in [kin_sdl_gpu.cmake](../cmake/kin_sdl_gpu.cmake). The optional direct-API SDL
variant separately changes its private copy's `SDL_DYNAMIC_API` default; see
[kin_sdl_direct.cmake](../cmake/kin_sdl_direct.cmake).

## Embedded data

With `KIN_EXPERIMENTAL_3D`, the GI's area lights reflect through the linearly transformed cosines tables of Heitz,
Dupuy, Hill and Neubelt ("Real-Time Polygonal-Light Shading with Linearly Transformed Cosines", 2016), as
[three.js](https://github.com/mrdoob/three.js) r160 ships them (`examples/jsm/lights/RectAreaLightUniformsLib.js`):
[gpu_area_lights_ltc.cpp](../engine/src/renderer/gpu/gpu_area_lights_ltc.cpp), written by
[tools/gen_area_light_ltc.py](../tools/gen_area_light_ltc.py), compiled into the engine. MIT license, Copyright ©
2010-2023 three.js authors (also at the top of that file):

```text
The MIT License

Copyright © 2010-2023 three.js authors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
```

## Libraries bundled by SDL_image and SDL_ttf

AccessKit is an externally supplied library. Its license texts are retained in
[`docs/licenses/accesskit`](licenses/accesskit) and included by the desktop package
helper when enabled. When building your own AccessKit static distribution, also
retain the notices for its Rust dependencies. Kin does not fetch or build Rust
dependencies during ordinary configuration.

Kin builds SDL_image and SDL_ttf with their vendored libraries enabled, so these
are compiled in as well:

| Component | Pulled in by | License |
|---|---|---|
| [libpng](http://www.libpng.org/pub/png/libpng.html) | SDL_image | PNG Reference Library License v2 |
| [zlib](https://zlib.net) | SDL_image | zlib |
| [stb_image](https://github.com/nothings/stb) | SDL and SDL_image | Public domain / MIT |
| [FreeType](https://freetype.org) | SDL_ttf | FreeType License (FTL) or GPLv2 — kin uses it under the FTL |
| [HarfBuzz](https://harfbuzz.github.io) | SDL_ttf | "Old MIT" |
| [plutosvg](https://github.com/sammycage/plutosvg) and [plutovg](https://github.com/sammycage/plutovg) | SDL_ttf | MIT |

The FreeType License asks that documentation for binary distributions credit it,
for example: "Portions of this software are copyright © 2026 The FreeType
Project (https://freetype.org). All rights reserved."

## Bundled example assets

The 3D starting points (`camp_walk`, `projectile_range`, `lua_range`) include a selected subset of
[Survival Kit 2.0 by Kenney](https://kenney.nl/assets/survival-kit), released under
[CC0 1.0](https://creativecommons.org/publicdomain/zero/1.0/). The original license and a source/checksum manifest are in
`examples/camp_walk/content/kenney-survival/` and accompany the content package. These selected models and their shared
colour atlas are unmodified. They also include a fitted survivor mesh/skin and retargeted animations derived from
[Kenney Animated Characters Survivors](https://kenney.nl/assets/animated-characters-survivors), CC0-1.0; its license and
modification manifest are in `examples/camp_walk/content/kenney-character/`. See `examples/camp_walk/content/README.md`
for budgets and reproduction commands.

## Reference assets (fetched, never distributed)

`tools/reference/fetch.py` downloads sample models into a developer's cache for the 3D reference scenes and tests; none of them is
committed, packed or shipped with kin. They come from the
[Khronos glTF Sample Assets](https://github.com/KhronosGroup/glTF-Sample-Assets), pinned to one commit and checked against
`tools/reference/assets.lock.json`:

| Asset | Used for | License |
|---|---|---|
| CesiumMan | the `skinned_cesium` scene | CC BY 4.0 (Cesium); the Cesium logo and trademarks are excluded |
| Fox | the `skinned_fox` scene and the animation timing | CC0 1.0 (PixelMannen, the model) and CC BY 4.0 (tomkranis, rigging and animation) |
| BrainStem | locked and downloaded; not yet in a scene | Poser EULA (Keith Hunter): not redistributed |
| Flight Helmet, Sponza | the `helmet` and `sponza` scenes | CC0 1.0; Cryengine Limited License Agreement (Crytek) |
