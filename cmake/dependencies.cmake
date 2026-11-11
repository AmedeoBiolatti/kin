include(FetchContent)

# SDL_image/SDL_ttf cache this global option. Establish the same default before
# creating SDL, Lua and Box2D, or the next configure changes those targets'
# flags and recompiles them all. An explicit caller/toolchain setting wins.
if(KIN_ENABLE_IMAGE_IO OR KIN_ENABLE_TTF)
    set(_kin_pic_default ON)
    if(EMSCRIPTEN)
        set(_kin_pic_default OFF)
    endif()
    option(CMAKE_POSITION_INDEPENDENT_CODE "Build static libraries with -fPIC" ${_kin_pic_default})
endif()

set(SDL_TEST OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_CAMERA OFF CACHE BOOL "" FORCE)
# kin::FileDialogs: the system's dialogs (on Linux the desktop portal over D-Bus, else zenity).
set(SDL_DIALOG ON CACHE BOOL "" FORCE)
set(SDL_HAPTIC OFF CACHE BOOL "" FORCE)
set(SDL_POWER OFF CACHE BOOL "" FORCE)
set(SDL_SENSOR OFF CACHE BOOL "" FORCE)

set(_kin_sdl_source_options)
if(NOT KIN_ENABLE_SDL_DYNAMIC_API)
    # Populate normally (including user-supplied FETCHCONTENT_SOURCE_DIR_SDL3),
    # but defer add_subdirectory until an isolated source copy has been patched.
    set(_kin_sdl_source_options SOURCE_SUBDIR __kin_populate_only)
endif()
FetchContent_Declare(SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG release-3.4.10
    GIT_SHALLOW TRUE
    ${_kin_sdl_source_options}
)

function(kin_configure_sdl)
    set(policy_options AUDIO GPU RENDER OPENGL OPENGLES VULKAN METAL
        RENDER_D3D RENDER_D3D11 RENDER_D3D12 RENDER_METAL RENDER_GPU RENDER_VULKAN
        JOYSTICK HIDAPI HIDAPI_JOYSTICK HIDAPI_LIBUSB HIDAPI_LIBUSB_SHARED VIRTUAL_JOYSTICK
        TRAY DBUS IBUS X11 X11_SHARED X11_XCURSOR X11_XDBE X11_XINPUT X11_XFIXES
        X11_XRANDR X11_XSCRNSAVER X11_XSHAPE X11_XSYNC X11_XTEST
        FRIBIDI FRIBIDI_SHARED LIBTHAI LIBTHAI_SHARED WAYLAND WAYLAND_SHARED
        WAYLAND_LIBDECOR WAYLAND_LIBDECOR_SHARED KMSDRM KMSDRM_SHARED
        RPI ROCKCHIP VIVANTE OPENVR)
    foreach(option IN LISTS policy_options)
        get_property(had_${option} CACHE SDL_${option} PROPERTY TYPE SET)
        if(had_${option})
            get_property(value_${option} CACHE SDL_${option} PROPERTY VALUE)
            get_property(type_${option} CACHE SDL_${option} PROPERTY TYPE)
            get_property(help_${option} CACHE SDL_${option} PROPERTY HELPSTRING)
        endif()
    endforeach()
    if(NOT KIN_ENABLE_AUDIO)
        set(SDL_AUDIO OFF)
    endif()
    if(NOT KIN_ENABLE_SDL_GAMEPAD)
        foreach(option JOYSTICK HIDAPI HIDAPI_JOYSTICK HIDAPI_LIBUSB VIRTUAL_JOYSTICK)
            set(SDL_${option} OFF)
        endforeach()
    endif()
    if(NOT KIN_ENABLE_SDL_DESKTOP_SERVICES)
        foreach(option TRAY DBUS IBUS)
            set(SDL_${option} OFF)
        endforeach()
    endif()
    if(NOT KIN_LINUX_VIDEO STREQUAL "auto")
        foreach(option X11 WAYLAND KMSDRM RPI ROCKCHIP VIVANTE OPENVR)
            set(SDL_${option} OFF)
        endforeach()
        if(KIN_LINUX_VIDEO MATCHES "x11")
            set(SDL_X11 ON)
        endif()
        if(KIN_LINUX_VIDEO MATCHES "wayland")
            set(SDL_WAYLAND ON)
        endif()
    endif()
    # SDL's dependent options FORCE-write local overrides into the cache.
    # Preserve the caller's settings so software -> gpu -> auto reconfiguration
    # does not accidentally keep the previous profile's disabled drivers.
    if(KIN_RENDERER STREQUAL "software" OR KIN_RENDERER STREQUAL "compact")
        set(SDL_GPU OFF)
        set(SDL_RENDER ON)
        if(KIN_RENDERER STREQUAL "compact")
            set(SDL_RENDER OFF)
        endif()
        foreach(driver OPENGL OPENGLES VULKAN METAL RENDER_D3D RENDER_D3D11
                       RENDER_D3D12 RENDER_METAL RENDER_GPU RENDER_VULKAN)
            set(SDL_${driver} OFF)
        endforeach()
    elseif(KIN_RENDERER STREQUAL "webgl2")
        set(SDL_GPU OFF)
        set(SDL_RENDER OFF)
        set(SDL_OPENGL OFF)
        set(SDL_OPENGLES ON)
    elseif(KIN_RENDERER STREQUAL "gpu")
        set(SDL_RENDER OFF)
        set(SDL_GPU ON)
        set(SDL_OPENGL OFF)
        set(SDL_OPENGLES OFF)
    endif()
    FetchContent_MakeAvailable(SDL3)
    set(sdl3_SOURCE_DIR "${sdl3_SOURCE_DIR}" PARENT_SCOPE)
    set(sdl3_BINARY_DIR "${sdl3_BINARY_DIR}" PARENT_SCOPE)
    if(NOT KIN_ENABLE_SDL_DYNAMIC_API)
        include("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/kin_sdl_direct.cmake")
        kin_prepare_sdl_direct("${sdl3_SOURCE_DIR}" "${sdl3_BINARY_DIR}/../sdl3-direct-src")
        add_subdirectory("${KIN_SDL_DIRECT_SOURCE_DIR}" "${sdl3_BINARY_DIR}")
    endif()
    if(KIN_HAS_GPU_RENDERER)
        include("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/kin_sdl_gpu.cmake")
        kin_patch_sdl_gpu_sampler_barriers(SDL3-static)
    endif()
    if(NOT KIN_ENABLE_IMAGE_IO)
        # SDL 3.4 also embeds PNG/JPEG codecs independently of SDL_image. Keep its
        # built-in disabled-codec stubs, excluding those implementations too.
        target_compile_definitions(SDL3-static PRIVATE SDL_DISABLE_STB)
    endif()
    # SDL's cache describes requested options, not necessarily compiled drivers.
    # Capture generated configuration to report actual capabilities and reject
    # explicitly requested drivers that SDL silently disabled for missing deps.
    set(video_drivers)
    # The final per-configuration headers are file(GENERATE) outputs and do not
    # exist yet on a clean configure. Driver defines in the intermediate header
    # are already resolved and independent of the selected build configuration.
    file(STRINGS "${sdl3_BINARY_DIR}/CMakeFiles/SDL_build_config.h.intermediate"
        drivers REGEX "^#define SDL_VIDEO_DRIVER_[A-Z0-9]+ 1$")
    foreach(driver IN LISTS drivers)
        string(REGEX REPLACE "^#define SDL_VIDEO_DRIVER_([A-Z0-9]+) 1$" "\\1" driver "${driver}")
        string(TOLOWER "${driver}" driver)
        list(APPEND video_drivers "${driver}")
    endforeach()
    list(REMOVE_DUPLICATES video_drivers)
    list(SORT video_drivers)
    foreach(required x11 wayland)
        if(KIN_LINUX_VIDEO MATCHES "${required}" AND NOT required IN_LIST video_drivers)
            message(FATAL_ERROR "KIN_LINUX_VIDEO=${KIN_LINUX_VIDEO} requires SDL's ${required} driver; install its development dependencies")
        endif()
    endforeach()
    set(KIN_SDL_VIDEO_DRIVERS "${video_drivers}" PARENT_SCOPE)
    foreach(option IN LISTS policy_options)
        if(had_${option})
            set(SDL_${option} "${value_${option}}" CACHE "${type_${option}}" "${help_${option}}" FORCE)
        else()
            unset(SDL_${option} CACHE)
        endif()
    endforeach()
endfunction()
kin_configure_sdl()

set(FLECS_STATIC ON CACHE BOOL "" FORCE)
set(FLECS_SHARED OFF CACHE BOOL "" FORCE)
set(FLECS_TESTS OFF CACHE BOOL "" FORCE)

FetchContent_Declare(flecs
    GIT_REPOSITORY https://github.com/SanderMertens/flecs.git
    GIT_TAG v4.0.5
    GIT_SHALLOW TRUE
)

FetchContent_MakeAvailable(flecs)

if(TARGET flecs_static)
    get_target_property(_flecs_include_dirs flecs_static INTERFACE_INCLUDE_DIRECTORIES)
    if(_flecs_include_dirs)
        set_property(TARGET flecs_static PROPERTY
            INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_flecs_include_dirs}"
        )
    endif()
    target_compile_options(flecs_static INTERFACE
        $<$<CXX_COMPILER_ID:MSVC>:/external:W0>
    )
endif()

if(KIN_ENABLE_PHYSICS)
    set(BOX2D_BUILD_UNIT_TESTS OFF CACHE BOOL "" FORCE)
    set(BOX2D_BUILD_TESTBED OFF CACHE BOOL "" FORCE)
    set(BOX2D_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(BOX2D_USER_SETTINGS OFF CACHE BOOL "" FORCE)

    if(KIN_PHYSICS_BOX2D_V3)
        set(BOX2D_SAMPLES OFF CACHE BOOL "" FORCE)
        set(BOX2D_UNIT_TESTS OFF CACHE BOOL "" FORCE)
        set(BOX2D_BENCHMARKS OFF CACHE BOOL "" FORCE)
        set(BOX2D_DOCS OFF CACHE BOOL "" FORCE)
        set(BOX2D_VALIDATE OFF CACHE BOOL "" FORCE)
        set(BOX2D_PROFILE OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(box2d3
            GIT_REPOSITORY https://github.com/erincatto/box2d.git
            GIT_TAG v3.2.0
            GIT_SHALLOW TRUE
        )
        FetchContent_MakeAvailable(box2d3)
        set(box2d_SOURCE_DIR "${box2d3_SOURCE_DIR}")
    else()
        FetchContent_Declare(box2d
            GIT_REPOSITORY https://github.com/erincatto/box2d.git
            GIT_TAG v2.4.1
            GIT_SHALLOW TRUE
        )

        FetchContent_MakeAvailable(box2d)
    endif()
endif()

if(KIN_ENABLE_SCRIPTING)
    # lua.org is the one dependency host outside GitHub and sometimes times out (CI), so a copy of the same tarball
    # on the kin repo's `deps` release is tried first; the hash pins both.
    FetchContent_Declare(lua
        URL https://github.com/AmedeoBiolatti/kin/releases/download/deps/lua-5.4.6.tar.gz
            https://www.lua.org/ftp/lua-5.4.6.tar.gz
        URL_HASH SHA256=7d5ea1b9cb6aa0b59ca3dde1c6adcb57ef83a1ba8e5432c0ecd06bf439b3ad88
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )

    FetchContent_GetProperties(lua)
    if(NOT lua_POPULATED)
        FetchContent_Populate(lua)
        add_library(lua_static STATIC
            "${lua_SOURCE_DIR}/src/lapi.c"
            "${lua_SOURCE_DIR}/src/lauxlib.c"
            "${lua_SOURCE_DIR}/src/lbaselib.c"
            "${lua_SOURCE_DIR}/src/lcode.c"
            "${lua_SOURCE_DIR}/src/lcorolib.c"
            "${lua_SOURCE_DIR}/src/lctype.c"
            "${lua_SOURCE_DIR}/src/ldblib.c"
            "${lua_SOURCE_DIR}/src/ldebug.c"
            "${lua_SOURCE_DIR}/src/ldo.c"
            "${lua_SOURCE_DIR}/src/ldump.c"
            "${lua_SOURCE_DIR}/src/lfunc.c"
            "${lua_SOURCE_DIR}/src/lgc.c"
            "${lua_SOURCE_DIR}/src/linit.c"
            "${lua_SOURCE_DIR}/src/liolib.c"
            "${lua_SOURCE_DIR}/src/llex.c"
            "${lua_SOURCE_DIR}/src/lmathlib.c"
            "${lua_SOURCE_DIR}/src/lmem.c"
            "${lua_SOURCE_DIR}/src/loadlib.c"
            "${lua_SOURCE_DIR}/src/lobject.c"
            "${lua_SOURCE_DIR}/src/lopcodes.c"
            "${lua_SOURCE_DIR}/src/loslib.c"
            "${lua_SOURCE_DIR}/src/lparser.c"
            "${CMAKE_CURRENT_LIST_DIR}/../engine/src/scripting/lua_state_seed.c" # lstate.c with kin's hash seed
            "${lua_SOURCE_DIR}/src/lstring.c"
            "${lua_SOURCE_DIR}/src/lstrlib.c"
            "${lua_SOURCE_DIR}/src/ltable.c"
            "${lua_SOURCE_DIR}/src/ltablib.c"
            "${lua_SOURCE_DIR}/src/ltm.c"
            "${lua_SOURCE_DIR}/src/lundump.c"
            "${lua_SOURCE_DIR}/src/lutf8lib.c"
            "${lua_SOURCE_DIR}/src/lvm.c"
            "${lua_SOURCE_DIR}/src/lzio.c"
        )
        add_library(Lua::Lua ALIAS lua_static)
        target_include_directories(lua_static PUBLIC "${lua_SOURCE_DIR}/src")
        target_compile_definitions(lua_static PUBLIC LUA_COMPAT_5_3)
    endif()

    FetchContent_Declare(sol2
        GIT_REPOSITORY https://github.com/ThePhD/sol2.git
        # 3.5.0: its optional<T&>::emplace compiles with GCC 15 and Clang 20+
        # (3.3.1's was rejected before use: ThePhD/sol2#1695), Emscripten's included.
        GIT_TAG v3.5.0
        GIT_SHALLOW TRUE
    )

    FetchContent_MakeAvailable(sol2)
endif()

if(KIN_ENABLE_AUDIO)
# Audio decoders (kin/audio/audio_decoder.hpp): dr_wav, dr_flac and dr_mp3, and
# stb_vorbis. Single-file C libraries; engine/src/audio/codecs.c compiles them.
# dr_libs is pinned past its last release for its fuzzing fixes.
FetchContent_Declare(dr_libs
    URL https://github.com/mackron/dr_libs/archive/dfe8377631000664666519fdb83da193fd8037f4.tar.gz
    URL_HASH SHA256=4654acb029f4f2a43ac2edb60c4cb09f40615b4b5bee9709954f910cb979e5fd
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_GetProperties(dr_libs)
if(NOT dr_libs_POPULATED)
    FetchContent_Populate(dr_libs)
endif()

endif()

if(KIN_ENABLE_AUDIO OR KIN_EXPERIMENTAL_3D)
FetchContent_Declare(stb
    URL https://github.com/nothings/stb/archive/2c980bb59875b0d32144a71867fbdebb2f77cd20.tar.gz
    URL_HASH SHA256=9a955b1b49a4410088a2e0ee2a9c057c3c907d0c1d75454144cb980aca0ba515
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_GetProperties(stb)
if(NOT stb_POPULATED)
    FetchContent_Populate(stb)
endif()

endif()

# Polygon triangulation for filled shapes (kin/renderer/shape.hpp). Header-only.
set(EARCUT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(EARCUT_BUILD_BENCH OFF CACHE BOOL "" FORCE)
set(EARCUT_BUILD_VIZ OFF CACHE BOOL "" FORCE)
FetchContent_Declare(earcut
    GIT_REPOSITORY https://github.com/mapbox/earcut.hpp.git
    GIT_TAG v3.2.4
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(earcut)
if(TARGET earcut_hpp)
    get_target_property(_earcut_include_dirs earcut_hpp INTERFACE_INCLUDE_DIRECTORIES)
    set_property(TARGET earcut_hpp PROPERTY INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_earcut_include_dirs}")
endif()

set(SDLIMAGE_VENDORED ON CACHE BOOL "" FORCE)
set(SDLIMAGE_DEPS_SHARED OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_INSTALL OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_SAMPLES OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_TESTS OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_AVIF OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_JXL OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_TIF OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_WEBP OFF CACHE BOOL "" FORCE)

set(_sdl3_shim "${CMAKE_BINARY_DIR}/sdl3_shim")
file(MAKE_DIRECTORY "${_sdl3_shim}")
file(WRITE "${_sdl3_shim}/SDL3Config.cmake" [=[
set(SDL3_FOUND TRUE)
set(SDL3_VERSION "3.4.10")
set(SDL3_Headers_FOUND TRUE)
set(SDL3_SDL3-static_FOUND TRUE)
set(SDL3_SDL3_FOUND TRUE)
if(NOT TARGET SDL3::Headers)
    add_library(SDL3_Headers_shim INTERFACE)
    add_library(SDL3::Headers ALIAS SDL3_Headers_shim)
endif()
if(NOT TARGET SDL3::SDL3)
    add_library(SDL3::SDL3 ALIAS SDL3-static)
endif()
if(NOT TARGET SDL3::SDL3-shared)
    add_library(SDL3::SDL3-shared ALIAS SDL3-static)
endif()
]=])
file(WRITE "${_sdl3_shim}/SDL3ConfigVersion.cmake" [=[
set(PACKAGE_VERSION "3.4.10")
if(PACKAGE_VERSION VERSION_LESS PACKAGE_FIND_VERSION)
    set(PACKAGE_VERSION_COMPATIBLE FALSE)
else()
    set(PACKAGE_VERSION_COMPATIBLE TRUE)
    if(PACKAGE_FIND_VERSION STREQUAL PACKAGE_VERSION)
        set(PACKAGE_VERSION_EXACT TRUE)
    endif()
endif()
]=])
set(SDL3_DIR "${_sdl3_shim}" CACHE PATH "" FORCE)

if(KIN_ENABLE_IMAGE_IO)
    FetchContent_Declare(SDL3_image
        GIT_REPOSITORY https://github.com/libsdl-org/SDL_image.git
        GIT_TAG release-3.4.4
        GIT_SHALLOW TRUE
        # Only the vendored libraries for the formats left on above (JPEG and PNG).
        # AVIF, JXL, TIF and WEBP are off, and their submodules (aom, dav1d,
        # libjxl, ...) are most of a full checkout's ~1 GB: a configure spent
        # about three minutes cloning them.
        GIT_SUBMODULES external/jpeg external/libpng external/zlib
    )

    FetchContent_MakeAvailable(SDL3_image)
endif()

if(KIN_ENABLE_TTF)
    set(SDLTTF_VENDORED ON CACHE BOOL "" FORCE)
    set(SDLTTF_DEPS_SHARED OFF CACHE BOOL "" FORCE)
    set(SDLTTF_INSTALL OFF CACHE BOOL "" FORCE)
    set(SDLTTF_SAMPLES OFF CACHE BOOL "" FORCE)
    set(SDLTTF_TESTS OFF CACHE BOOL "" FORCE)

    FetchContent_Declare(SDL3_ttf
        GIT_REPOSITORY https://github.com/libsdl-org/SDL_ttf.git
        GIT_TAG release-3.2.2
        GIT_SHALLOW TRUE
    )

    FetchContent_MakeAvailable(SDL3_ttf)
endif()

# Exact dependency notices, only for components present in this build.
if(KIN_ENABLE_SCRIPTING)
file(READ "${lua_SOURCE_DIR}/src/lua.h" _kin_lua_header)
string(REGEX MATCH "\\* Copyright \\(C\\)[^\n]*Lua\\.org.*SOFTWARE\\.\n" _kin_lua_license "${_kin_lua_header}")
string(REGEX REPLACE "(^|\n)\\* ?" "\\1" _kin_lua_license "${_kin_lua_license}")
file(WRITE "${CMAKE_BINARY_DIR}/licenses/Lua.txt" "${_kin_lua_license}")
endif()
set(KIN_THIRD_PARTY_LICENSES "")
if(EXISTS "${sdl3_SOURCE_DIR}/LICENSE.txt")
    list(APPEND KIN_THIRD_PARTY_LICENSES "SDL.txt|${sdl3_SOURCE_DIR}/LICENSE.txt")
endif()
if(EXISTS "${sdl3_image_SOURCE_DIR}/LICENSE.txt")
    list(APPEND KIN_THIRD_PARTY_LICENSES "SDL_image.txt|${sdl3_image_SOURCE_DIR}/LICENSE.txt")
endif()
if(EXISTS "${sdl3_ttf_SOURCE_DIR}/LICENSE.txt")
    list(APPEND KIN_THIRD_PARTY_LICENSES "SDL_ttf.txt|${sdl3_ttf_SOURCE_DIR}/LICENSE.txt")
endif()
if(EXISTS "${flecs_SOURCE_DIR}/LICENSE")
    list(APPEND KIN_THIRD_PARTY_LICENSES "flecs.txt|${flecs_SOURCE_DIR}/LICENSE")
endif()
if(EXISTS "${box2d_SOURCE_DIR}/LICENSE")
    list(APPEND KIN_THIRD_PARTY_LICENSES "Box2D.txt|${box2d_SOURCE_DIR}/LICENSE")
endif()
if(EXISTS "${CMAKE_BINARY_DIR}/licenses/Lua.txt")
    list(APPEND KIN_THIRD_PARTY_LICENSES "Lua.txt|${CMAKE_BINARY_DIR}/licenses/Lua.txt")
endif()
if(EXISTS "${sol2_SOURCE_DIR}/LICENSE.txt")
    list(APPEND KIN_THIRD_PARTY_LICENSES "sol2.txt|${sol2_SOURCE_DIR}/LICENSE.txt")
endif()
if(EXISTS "${earcut_SOURCE_DIR}/LICENSE")
    list(APPEND KIN_THIRD_PARTY_LICENSES "earcut.hpp.txt|${earcut_SOURCE_DIR}/LICENSE")
endif()
if(EXISTS "${stb_SOURCE_DIR}/LICENSE")
    list(APPEND KIN_THIRD_PARTY_LICENSES "stb.txt|${stb_SOURCE_DIR}/LICENSE")
endif()
if(EXISTS "${sdl3_image_SOURCE_DIR}/external/libpng/LICENSE")
    list(APPEND KIN_THIRD_PARTY_LICENSES "libpng.txt|${sdl3_image_SOURCE_DIR}/external/libpng/LICENSE")
endif()
if(EXISTS "${sdl3_image_SOURCE_DIR}/external/zlib/LICENSE")
    list(APPEND KIN_THIRD_PARTY_LICENSES "zlib.txt|${sdl3_image_SOURCE_DIR}/external/zlib/LICENSE")
endif()
if(EXISTS "${sdl3_ttf_SOURCE_DIR}/external/freetype/docs/FTL.TXT")
    list(APPEND KIN_THIRD_PARTY_LICENSES "FreeType.txt|${sdl3_ttf_SOURCE_DIR}/external/freetype/docs/FTL.TXT")
endif()
if(EXISTS "${sdl3_ttf_SOURCE_DIR}/external/harfbuzz/COPYING")
    list(APPEND KIN_THIRD_PARTY_LICENSES "HarfBuzz.txt|${sdl3_ttf_SOURCE_DIR}/external/harfbuzz/COPYING")
endif()
if(EXISTS "${sdl3_ttf_SOURCE_DIR}/external/plutosvg/LICENSE")
    list(APPEND KIN_THIRD_PARTY_LICENSES "plutosvg.txt|${sdl3_ttf_SOURCE_DIR}/external/plutosvg/LICENSE")
endif()
if(EXISTS "${sdl3_ttf_SOURCE_DIR}/external/plutovg/LICENSE")
    list(APPEND KIN_THIRD_PARTY_LICENSES "plutovg.txt|${sdl3_ttf_SOURCE_DIR}/external/plutovg/LICENSE")
endif()
set(KIN_THIRD_PARTY_LICENSES "${KIN_THIRD_PARTY_LICENSES}" CACHE INTERNAL "" FORCE)
# Later optional dependencies append to the cache; do not shadow those entries.
unset(KIN_THIRD_PARTY_LICENSES)

# Jolt Physics (kin/physics3d, docs/physics_3d.md), only with the experimental 3D
# option: a rigid body engine, MIT. Built as a static library without its
# samples, tests, viewer, debug renderer, profiler or object stream, in single
# precision and with CROSS_PLATFORM_DETERMINISTIC on, so that a lockstep step
# gives the same bits on every machine. A local clone can stand in for the
# download: -DFETCHCONTENT_SOURCE_DIR_JOLTPHYSICS=<path to the JoltPhysics clone>.
if(KIN_EXPERIMENTAL_3D AND KIN_ENABLE_PHYSICS3D)
    set(CROSS_PLATFORM_DETERMINISTIC ON CACHE BOOL "" FORCE)
    set(DOUBLE_PRECISION OFF CACHE BOOL "" FORCE)
    set(ENABLE_ALL_WARNINGS OFF CACHE BOOL "" FORCE)         # -Werror on a compiler Jolt was not tried with
    set(INTERPROCEDURAL_OPTIMIZATION OFF CACHE BOOL "" FORCE) # kin's own targets are not built with LTO
    set(GENERATE_DEBUG_SYMBOLS OFF CACHE BOOL "" FORCE)
    set(ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    set(USE_ASSERTS OFF CACHE BOOL "" FORCE)
    set(DEBUG_RENDERER_IN_DEBUG_AND_RELEASE OFF CACHE BOOL "" FORCE)
    set(DEBUG_RENDERER_IN_DISTRIBUTION OFF CACHE BOOL "" FORCE)
    set(PROFILER_IN_DEBUG_AND_RELEASE OFF CACHE BOOL "" FORCE)
    set(PROFILER_IN_DISTRIBUTION OFF CACHE BOOL "" FORCE)
    set(ENABLE_OBJECT_STREAM OFF CACHE BOOL "" FORCE)
    # The same MSVC runtime as kin's own targets: the static one only in a shipping
    # build (CMAKE_MSVC_RUNTIME_LIBRARY in the top-level CMakeLists.txt), else the
    # DLL runtime CMake defaults to. Jolt's own default is ON on MSVC, which would
    # link a /MT Jolt into a /MD kin (LNK2038) in a plain developer build. When OFF
    # Jolt leaves CMAKE_MSVC_RUNTIME_LIBRARY alone, so it inherits kin's setting.
    if(KIN_SHIPPING AND MSVC)
        set(USE_STATIC_MSVC_RUNTIME_LIBRARY ON CACHE BOOL "" FORCE)
    else()
        set(USE_STATIC_MSVC_RUNTIME_LIBRARY OFF CACHE BOOL "" FORCE)
    endif()
    FetchContent_Declare(JoltPhysics
        GIT_REPOSITORY https://github.com/jrouwe/JoltPhysics.git
        GIT_TAG v5.5.0
        GIT_SHALLOW TRUE
        SOURCE_SUBDIR Build
    )
    FetchContent_MakeAvailable(JoltPhysics)
    set(KIN_THIRD_PARTY_LICENSES "${KIN_THIRD_PARTY_LICENSES};JoltPhysics.txt|${joltphysics_SOURCE_DIR}/LICENSE" CACHE INTERNAL "")
    if(NOT EXISTS "${joltphysics_SOURCE_DIR}/LICENSE")
        message(FATAL_ERROR "License text not found: ${joltphysics_SOURCE_DIR}/LICENSE")
    endif()
endif()

# PocketPy (kin/scripting/python_scripts.hpp, docs/python_scripting.md), only with
# the experimental PYTHON component: a Python 3 interpreter in C, MIT. The
# release's amalgamation (pocketpy.c and pocketpy.h) and the repository's
# LICENSE at that tag, each pinned by its hash; engine/src/scripting/pocketpy_sandbox.c
# compiles it. A folder holding those three files can stand in for the
# downloads: -DFETCHCONTENT_SOURCE_DIR_POCKETPY=<folder>.
if(KIN_ENABLE_PYTHON)
    if(FETCHCONTENT_SOURCE_DIR_POCKETPY)
        set(FETCHCONTENT_SOURCE_DIR_POCKETPY_HEADER "${FETCHCONTENT_SOURCE_DIR_POCKETPY}")
        set(FETCHCONTENT_SOURCE_DIR_POCKETPY_LICENSE "${FETCHCONTENT_SOURCE_DIR_POCKETPY}")
    endif()
    set(_kin_pocketpy_release https://github.com/pocketpy/pocketpy/releases/download/v2.2.0)
    FetchContent_Declare(pocketpy
        URL ${_kin_pocketpy_release}/pocketpy.c
        URL_HASH SHA256=0f0c19071b4fd0b37cb292e15bea49ca163f151af901b00893d3a2005bb056e6
        DOWNLOAD_NO_EXTRACT TRUE
    )
    FetchContent_Declare(pocketpy_header
        URL ${_kin_pocketpy_release}/pocketpy.h
        URL_HASH SHA256=43864cfa090d65b80915467900430dfae52c769f13d6edb137d0227d1fbe03ad
        DOWNLOAD_NO_EXTRACT TRUE
    )
    FetchContent_Declare(pocketpy_license
        URL https://raw.githubusercontent.com/pocketpy/pocketpy/v2.2.0/LICENSE
        URL_HASH SHA256=5ad14c41f26ed1b8c9a0039ae566bc7ce00277aec1634220eae5db2bb8fc54ef
        DOWNLOAD_NO_EXTRACT TRUE
    )
    FetchContent_MakeAvailable(pocketpy pocketpy_header pocketpy_license) # files only: nothing to add_subdirectory
    foreach(_kin_file "${pocketpy_SOURCE_DIR}/pocketpy.c" "${pocketpy_header_SOURCE_DIR}/pocketpy.h"
                      "${pocketpy_license_SOURCE_DIR}/LICENSE")
        if(NOT EXISTS "${_kin_file}")
            message(FATAL_ERROR "PocketPy: ${_kin_file} not found")
        endif()
    endforeach()
    # Its notice: PocketPy's license, then those of the code its amalgamation
    # carries (fastermath's logarithms, which its math uses; the Mersenne Twister).
    file(READ "${pocketpy_license_SOURCE_DIR}/LICENSE" _kin_pocketpy_notice)
    file(READ "${pocketpy_SOURCE_DIR}/pocketpy.c" _kin_pocketpy_code)
    foreach(_kin_first "Copyright (c) 2012,2013   Axel Kohlmeyer" "Copyright (c) 2011 Mutsuo Saito")
        string(FIND "${_kin_pocketpy_code}" "${_kin_first}" _kin_begin)
        if(_kin_begin EQUAL -1)
            message(FATAL_ERROR "PocketPy: the notice beginning '${_kin_first}' is not in pocketpy.c")
        endif()
        string(SUBSTRING "${_kin_pocketpy_code}" ${_kin_begin} 4000 _kin_part)
        string(FIND "${_kin_part}" "SUCH DAMAGE." _kin_end)
        if(_kin_end EQUAL -1)
            message(FATAL_ERROR "PocketPy: the notice beginning '${_kin_first}' has no end")
        endif()
        math(EXPR _kin_end "${_kin_end} + 12")
        string(SUBSTRING "${_kin_part}" 0 ${_kin_end} _kin_part)
        string(APPEND _kin_pocketpy_notice "\nPocketPy includes code under this notice:\n\n${_kin_part}\n")
    endforeach()
    unset(_kin_pocketpy_code)
    file(WRITE "${CMAKE_BINARY_DIR}/licenses/PocketPy.txt" "${_kin_pocketpy_notice}")
    set(KIN_THIRD_PARTY_LICENSES "${KIN_THIRD_PARTY_LICENSES};PocketPy.txt|${CMAKE_BINARY_DIR}/licenses/PocketPy.txt" CACHE INTERNAL "")
endif()
