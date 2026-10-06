cmake_minimum_required(VERSION 3.22)

project(kin VERSION 0.2.5 LANGUAGES C CXX)

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
include(dependencies)
include(kin_embed)
include(kin_game)

# Built by default only when kin is the top-level project: a game that adds
# kin with add_subdirectory gets the engine alone.
option(KIN_BUILD_GAMES "Build sample games" ${PROJECT_IS_TOP_LEVEL})
option(KIN_BUILD_EXAMPLES "Build interactive performance examples" ${PROJECT_IS_TOP_LEVEL})
option(KIN_BUILD_BENCHMARKS "Build benchmark executables" ${PROJECT_IS_TOP_LEVEL})
option(KIN_BUILD_TESTS "Build kin's tests (with BUILD_TESTING)" ${PROJECT_IS_TOP_LEVEL})
option(KIN_ENABLE_PROFILING "Enable Kin manual profiling macros" OFF)
option(KIN_ENABLE_RELEASE_PROFILING "Enable Kin manual profiling macros in Release builds" ${_kin_developer_default})
# The headless render probe (--probe-render), the determinism check
# (--check-determinism) and the agent server (--server): tools for developers
# and agents, off in shipping builds.
option(KIN_ENABLE_RENDER_PROBE "Build the headless render probe (--probe-render)" ${_kin_developer_default})
option(KIN_ENABLE_DETERMINISM_CHECK "Build the determinism check (--check-determinism)" ${_kin_developer_default})
option(KIN_ENABLE_AGENT_SERVER "Let games run as an agent server (--server)" ${_kin_developer_default})
# Ship a game's content as one .kinpak archive beside it, rather than a folder.
option(KIN_PACK_CONTENT "Pack each game's content into a .kinpak when it is installed" ON)
# Stop at configure when the GPU shaders cannot be built (no glslc, no
# KIN_SPIRV_DIR), rather than build a GPU backend without them (games then
# fall back to SDL_Renderer).
option(KIN_REQUIRE_GPU_SHADERS "Require glslc for the GPU backend's shaders" ${KIN_SHIPPING})

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
