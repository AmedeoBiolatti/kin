cmake_minimum_required(VERSION 3.28)
project(KinCppModulesExperiment LANGUAGES C CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR
   NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR
   CMAKE_CXX_COMPILER_VERSION VERSION_LESS 21 OR
   NOT CMAKE_GENERATOR MATCHES "^Ninja")
    message(FATAL_ERROR "This isolated experiment requires native Linux Clang 21+ and Ninja 1.11+")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
# Ordinary engine and header-consumer files retain their usual compilation.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
set(KIN_ENABLE_PCH ON CACHE BOOL "Use the current development PCH configuration")
set(KIN_ENABLE_LTO OFF CACHE BOOL "Keep link-time optimization outside the experiment")
set(KIN_COMPILER_CACHE off CACHE STRING "Measure actual compilations")
set(KIN_BUILD_GAMES OFF CACHE BOOL "Only the experiment's clients are needed")
set(KIN_BUILD_EXAMPLES OFF CACHE BOOL "Only the experiment's clients are needed")
set(KIN_BUILD_2D_WORKFLOW OFF CACHE BOOL "Only the experiment's clients are needed")
set(KIN_BUILD_BENCHMARKS OFF CACHE BOOL "Only the experiment's clients are needed")
set(KIN_BUILD_TESTS OFF CACHE BOOL "The experiment has its own runtime comparisons")
set(KIN_EXPERIMENTAL_3D OFF CACHE BOOL "This experiment measures the 2D Hex game")
option(KIN_MODULE_EXPERIMENT_REDUCED_BMI "Try Clang's reduced module interface format" OFF)

get_filename_component(kin_root "../.." ABSOLUTE)
add_subdirectory("${kin_root}" kin EXCLUDE_FROM_ALL)
enable_testing()

set(api_headers
    kin/core/hex.hpp
    kin/core/json.hpp
    kin/pathfinding/pathfinding.hpp
    kin/platform/input.hpp
    kin/platform/window.hpp
    kin/renderer/renderer2d.hpp
    kin/runtime/game_info.hpp
    kin/runtime/scene_app.hpp
    kin/scene/scene.hpp
    kin/scene/scene_manager.hpp
    kin/ui2/text.hpp)

add_library(kin_runtime_module STATIC)
target_sources(kin_runtime_module PUBLIC FILE_SET CXX_MODULES
    BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}" FILES kin_runtime.cppm)
target_link_libraries(kin_runtime_module PRIVATE kin::engine)
kin_enable_release_optimizations(kin_runtime_module)
set_target_properties(kin_runtime_module PROPERTIES
    CXX_SCAN_FOR_MODULES ON DISABLE_PRECOMPILE_HEADERS ON)
if(KIN_MODULE_EXPERIMENT_REDUCED_BMI)
    target_compile_options(kin_runtime_module PRIVATE -fmodules-reduced-bmi)
endif()

# Copies keep source-edit measurements away from the user's game checkout.
file(READ "${kin_root}/games/hex_demo/src/main.cpp" game_main)
file(READ "${kin_root}/games/hex_demo/src/hex_game.hpp" game_header)
string(FIND "${game_main}" "\nnamespace demo {" main_body_start)
string(FIND "${game_header}" "#include <algorithm>" std_start)
string(FIND "${game_header}" "\nnamespace demo {" header_body_start)
if(main_body_start LESS 0 OR std_start LESS 0 OR header_body_start LESS 0)
    message(FATAL_ERROR "Hex source layout changed: review the module client transformation")
endif()
string(SUBSTRING "${game_main}" ${main_body_start} -1 main_body)
string(SUBSTRING "${game_header}" ${header_body_start} -1 header_body)
math(EXPR std_length "${header_body_start} - ${std_start}")
string(SUBSTRING "${game_header}" ${std_start} ${std_length} standard_headers)

foreach(client IN ITEMS headers matched modules)
    set(client_dir "${CMAKE_CURRENT_BINARY_DIR}/clients/${client}")
    if(client STREQUAL "modules")
        set(client_main "#include \"hex_game.hpp\"\n${main_body}")
        set(client_header "#pragma once\n${standard_headers}\nimport kin.runtime;\n${header_body}")
    else()
        set(client_main "${game_main}")
        set(client_header "${game_header}")
    endif()
    file(CONFIGURE OUTPUT "${client_dir}/main.cpp" CONTENT "${client_main}" @ONLY)
    file(CONFIGURE OUTPUT "${client_dir}/hex_game.hpp" CONTENT "${client_header}" @ONLY)
    set(target "hex_cpp_${client}")
    add_executable(${target} "${client_dir}/main.cpp")
    target_link_libraries(${target} PRIVATE kin::engine)
    target_compile_definitions(${target} PRIVATE KIN_HEX_DEMO_BAKED_FONT=0)
    if(client STREQUAL "modules")
        # Precompile only standard headers; the facade provides the engine API.
        kin_precompile_target(${target})
        target_link_libraries(${target} PRIVATE kin_runtime_module)
        set_target_properties(${target} PROPERTIES CXX_SCAN_FOR_MODULES ON)
    endif()
    kin_game(${target} TITLE "Hex Demo")
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    if(client STREQUAL "matched" AND KIN_ENABLE_PCH)
        foreach(header IN LISTS api_headers)
            target_precompile_headers(${target} PRIVATE "<${header}>")
        endforeach()
    endif()
    add_test(NAME ${target}
        COMMAND ${target} --headless --render --frames=10 --report=-)
    set_tests_properties(${target} PROPERTIES
        PASS_REGULAR_EXPRESSION "\"path_cells\": 17,[ \n]*\"path_cost\": 170,[ \n]*\"reachable_cells\": 28")
endforeach()
