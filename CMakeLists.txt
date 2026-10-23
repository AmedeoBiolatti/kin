cmake_minimum_required(VERSION 3.22)

project(kin VERSION 0.2.6 LANGUAGES C CXX)

include(CTest)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

# A build to give to players (docs/shipping.md): no agent server, probes or
# profiling, each game's content in one pack, and on Windows a windowed app on
# the static runtime. Its defaults below apply to a new build folder; the `ship`
# preset makes one.
option(KIN_SHIPPING "Build games to ship to players" OFF)
if(KIN_SHIPPING)
    set(_kin_developer_default OFF)
else()
    set(_kin_developer_default ON)
endif()

if(KIN_SHIPPING AND MSVC)
    # The static C/C++ runtime, so players need no Visual C++ redistributable. Set
    # before the dependencies are added, and for those that ask for an older CMake.
    set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

function(kin_enable_release_optimizations target)
    target_compile_definitions(${target} PRIVATE
        "$<$<CONFIG:Release>:NDEBUG>"
    )

    target_compile_options(${target} PRIVATE
        "$<$<AND:$<CONFIG:Release>,$<CXX_COMPILER_ID:MSVC>>:/O2;/Ob2;/Oi;/Gy;/EHsc>"
        "$<$<AND:$<CONFIG:Release>,$<CXX_COMPILER_ID:GNU,Clang>>:-O2>"
    )
endfunction()

function(kin_enable_release_link_optimizations target)
    target_link_options(${target} PRIVATE
        "$<$<AND:$<CONFIG:Release>,$<CXX_COMPILER_ID:MSVC>>:/OPT:REF;/OPT:ICF>"
    )
endfunction()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(kin_release_contract)

# Built by default only when kin is the top-level project: a game that adds
# kin with add_subdirectory gets the engine alone.
option(KIN_BUILD_GAMES "Build sample games" ${PROJECT_IS_TOP_LEVEL})
option(KIN_BUILD_EXAMPLES "Build interactive performance examples" ${PROJECT_IS_TOP_LEVEL})
option(KIN_BUILD_BENCHMARKS "Build benchmark executables" ${PROJECT_IS_TOP_LEVEL})
option(KIN_BUILD_TESTS "Build kin's tests (with BUILD_TESTING)" ${PROJECT_IS_TOP_LEVEL})
option(KIN_ENABLE_PROFILING "Enable Kin manual profiling macros" OFF)
option(KIN_ENABLE_RELEASE_PROFILING "Enable Kin manual profiling macros in Release builds" ${_kin_developer_default})
option(KIN_EXPERIMENTAL_3D "Build kin's experimental 3D rendering" OFF)
option(KIN_ENABLE_AGENT_SERVER "Let games run as an agent server (--server)" ${_kin_developer_default})
option(KIN_PACK_CONTENT "Pack each game's content into a .kinpak when it is installed" ON)
option(KIN_REQUIRE_GPU_SHADERS "Require glslc for the GPU backend's shaders" ${KIN_SHIPPING})
include(kin_components)
# Retain the existing server option; the component switch controls compilation.
set(KIN_ENABLE_AGENT_SERVER ${KIN_ENABLE_SCENE_SERVER})
kin_resolve_components()
option(KIN_ENABLE_SIZE_OPTIMIZATIONS "Discard unused functions/data at link time" OFF)
option(KIN_ENABLE_SDL_DYNAMIC_API "Allow runtime replacement of the statically linked SDL (OFF is experimental)" ON)
option(KIN_ENABLE_SDL_GAMEPAD "Build SDL joystick/gamepad and HID support" ON)
option(KIN_ENABLE_SDL_DESKTOP_SERVICES "Build SDL tray, D-Bus and IBus integrations" ON)
set(KIN_LINUX_VIDEO "auto" CACHE STRING "Linux native window backends: auto, x11, wayland, x11-wayland")
set_property(CACHE KIN_LINUX_VIDEO PROPERTY STRINGS auto x11 wayland x11-wayland)
if(NOT KIN_LINUX_VIDEO MATCHES "^(auto|x11|wayland|x11-wayland)$")
    message(FATAL_ERROR "Invalid KIN_LINUX_VIDEO: '${KIN_LINUX_VIDEO}'")
endif()
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT KIN_LINUX_VIDEO STREQUAL "auto")
    message(FATAL_ERROR "KIN_LINUX_VIDEO=${KIN_LINUX_VIDEO} requires a Linux target")
endif()
option(KIN_ENABLE_LTO "Enable interprocedural optimization in optimized configurations" OFF)
option(KIN_PACKAGE_DEBUG_SYMBOLS "Generate debug information and a separate desktop symbols archive" OFF)
option(KIN_PACKAGE_STRIP "Strip installed package binaries (keep build originals)" OFF)
set(KIN_RENDERER "auto" CACHE STRING "Renderer included in this build: auto, software, gpu, compact")
set_property(CACHE KIN_RENDERER PROPERTY STRINGS auto software gpu compact)
if(NOT KIN_RENDERER MATCHES "^(auto|software|gpu|compact)$")
    message(FATAL_ERROR "KIN_RENDERER must be auto, software, gpu or compact (got '${KIN_RENDERER}')")
endif()
set(KIN_HAS_GPU_RENDERER ON)
set(KIN_HAS_SDL_RENDERER ON)
set(KIN_HAS_COMPACT_RENDERER OFF)
if(KIN_RENDERER STREQUAL "software")
    set(KIN_HAS_GPU_RENDERER OFF)
elseif(KIN_RENDERER STREQUAL "gpu")
    set(KIN_HAS_SDL_RENDERER OFF)
elseif(KIN_RENDERER STREQUAL "compact")
    set(KIN_HAS_GPU_RENDERER OFF)
    set(KIN_HAS_SDL_RENDERER OFF)
    set(KIN_HAS_COMPACT_RENDERER ON)
endif()

if(KIN_ENABLE_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT ipo_supported OUTPUT ipo_error LANGUAGES C CXX)
    if(NOT ipo_supported)
        message(FATAL_ERROR "KIN_ENABLE_LTO is unsupported: ${ipo_error}")
    endif()
    foreach(config RELEASE RELWITHDEBINFO MINSIZEREL)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_${config} ON)
    endforeach()
endif()

include(kin_release)

# An isolated Linux size experiment; deliberately bypass the full engine and
# dependency fetching so its binaries cannot accidentally retain SDL.
option(KIN_EXPERIMENTAL_MINIMAL_X11 "Build only the experimental SDL-free X11 hex runtime" OFF)
if(KIN_EXPERIMENTAL_MINIMAL_X11)
    if(NOT KIN_PGO_MODE STREQUAL "off")
        message(FATAL_ERROR "Compiler PGO is not wired into the isolated minimal-X11 exporter")
    endif()
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        message(FATAL_ERROR "KIN_EXPERIMENTAL_MINIMAL_X11 requires Linux")
    endif()
    add_subdirectory(experimental/minimal_x11)
    return()
endif()

kin_generate_component_config("${CMAKE_CURRENT_BINARY_DIR}/generated/kin")

# Options must be known before fetching optional third-party dependencies.
if(KIN_EXPERIMENTAL_3D AND NOT KIN_HAS_GPU_RENDERER)
    message(FATAL_ERROR "KIN_EXPERIMENTAL_3D requires KIN_RENDERER=auto or gpu")
endif()
if(KIN_EXPERIMENTAL_3D)
    # SceneSession3D currently owns the Lua scene lifecycle.
    kin_require_components(KIN_EXPERIMENTAL_3D SCRIPTING)
endif()
include(dependencies)
include(kin_embed)
include(kin_game)

add_subdirectory(engine)
add_subdirectory(tools)

if(KIN_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(KIN_BUILD_GAMES AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/games/CMakeLists.txt")
    add_subdirectory(games)
endif()

if(BUILD_TESTING AND KIN_BUILD_TESTS)
    add_subdirectory(tests)
endif()

# Include all compiled dependencies, including nested codec/font libraries.
kin_apply_release_tree("${CMAKE_CURRENT_SOURCE_DIR}")
