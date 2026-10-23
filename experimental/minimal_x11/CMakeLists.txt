# This source selection is intentional: the experiment has no SDL, ECS, scene
# runtime or optional dependency fetch. It is not a replacement for kin::engine.
find_package(X11 REQUIRED)
set(kin_root "${PROJECT_SOURCE_DIR}")
add_library(kin_x11_host STATIC x11_host.cpp)
target_link_libraries(kin_x11_host PUBLIC X11::X11)
target_include_directories(kin_x11_host PUBLIC "${kin_root}/engine/include" "${CMAKE_CURRENT_SOURCE_DIR}")
add_library(kin_hex_runtime STATIC
    ${kin_root}/engine/src/core/hex.cpp
    ${kin_root}/engine/src/core/json.cpp
    ${kin_root}/engine/src/core/utf8.cpp
    ${kin_root}/engine/src/core/bidi.cpp
    ${kin_root}/engine/src/pathfinding/hex_pathfinding.cpp
    ${kin_root}/engine/src/platform/log.cpp
    ${kin_root}/engine/src/renderer/compact_renderer2d_backend.cpp
    ${kin_root}/engine/src/renderer/renderer2d.cpp
    ${kin_root}/engine/src/renderer/renderer2d_shapes.cpp
    ${kin_root}/engine/src/renderer/color.cpp
    ${kin_root}/engine/src/renderer/shader_disabled.cpp
    ${kin_root}/engine/src/ui2/text.cpp
    ${kin_root}/engine/src/ui2/text_baked.cpp)
target_include_directories(kin_hex_runtime PUBLIC "${kin_root}/engine/include"
    "${kin_root}/engine/src/renderer" "${kin_root}/games/hex_demo/src")
target_compile_definitions(kin_hex_runtime PUBLIC KIN_HAS_COMPACT_RENDERER=1 KIN_HAS_SDL_RENDERER=0
    KIN_HAS_GPU_RENDERER=0 KIN_ENABLE_IMAGE_IO=0 KIN_ENABLE_TTF=0 KIN_HEX_DEMO_BAKED_FONT=1 KIN_LOG_SDL_SINK=0)
foreach(target kin_x11_host kin_hex_runtime)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -ffunction-sections -fdata-sections)
endforeach()
# Package as a thin X11 desktop export. These named display libraries, and
# the usual C/C++ runtime, must already be installed on the destination.
include(kin_packaging)
foreach(feature SCRIPTING PHYSICS AUDIO SCENE_SERVER DEBUG_OVERLAY RENDER_PROBE
        DETERMINISM_CHECK IMAGE_IO TTF RUNTIME_PROFILING RUNTIME_ASSETS PROFILING
        RELEASE_PROFILING SDL_DYNAMIC_API SDL_GAMEPAD SDL_DESKTOP_SERVICES)
    set(KIN_ENABLE_${feature} OFF)
endforeach()
set(KIN_ENABLE_SIZE_OPTIMIZATIONS ON)
set(KIN_RENDERER compact)
set(KIN_LINUX_VIDEO minimal-x11)
set(KIN_SDL_VIDEO_DRIVERS "")
set(stages x11_empty x11_framebuffer x11_hex x11_hud hex_demo)
set(stage 0)
foreach(target IN LISTS stages)
    add_executable(${target} main.cpp)
    target_compile_definitions(${target} PRIVATE KIN_X11_STAGE=${stage})
    target_link_libraries(${target} PRIVATE kin_x11_host)
    if(stage GREATER_EQUAL 2)
        target_link_libraries(${target} PRIVATE kin_hex_runtime)
    endif()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -ffunction-sections -fdata-sections)
    target_link_options(${target} PRIVATE -Wl,--gc-sections)
    set(notices "${CMAKE_CURRENT_SOURCE_DIR}/HOST-REQUIREMENTS.txt")
    if(stage EQUAL 3 OR stage EQUAL 4)
        list(APPEND notices "${kin_root}/games/hex_demo/FONT-LICENSE.txt")
    endif()
    kin_package_game(${target} LICENSE_FILES ${notices}
        HOST_LIBRARY_REGEXES "^libX11\\.so\\..*" "^libxcb\\.so\\..*" "^libXau\\.so\\..*" "^libXdmcp\\.so\\..*")
    math(EXPR stage "${stage} + 1")
endforeach()
if(BUILD_TESTING AND KIN_BUILD_TESTS)
    add_executable(kin_minimal_x11_tests tests.cpp)
    target_link_libraries(kin_minimal_x11_tests PRIVATE kin_x11_host kin_hex_runtime)
    target_compile_options(kin_minimal_x11_tests PRIVATE -UNDEBUG)
    target_link_options(kin_minimal_x11_tests PRIVATE -Wl,--gc-sections)
    add_test(NAME minimal-x11-headless COMMAND kin_minimal_x11_tests)
    add_test(NAME hex-demo COMMAND hex_demo --headless --render --frames=10 --report=-)
    set_tests_properties(hex-demo PROPERTIES PASS_REGULAR_EXPRESSION
        "\"path_cells\": 17,[ \n]*\"path_cost\": 170,[ \n]*\"reachable_cells\": 28")
endif()

add_custom_target(minimal_x11_packages DEPENDS x11_empty_package x11_framebuffer_package
    x11_hex_package x11_hud_package hex_demo_package)
