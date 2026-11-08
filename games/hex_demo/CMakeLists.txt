option(KIN_HEX_DEMO_BAKED_FONT "Use the embedded ASCII HUD font in hex_demo" OFF)
add_executable(hex_demo src/main.cpp)
if(KIN_EXPERIMENTAL_CPP_MODULES)
    if(CMAKE_VERSION VERSION_LESS 3.28)
        message(FATAL_ERROR "KIN_EXPERIMENTAL_CPP_MODULES requires CMake 3.28 or newer")
    endif()
    if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR
       CMAKE_CXX_COMPILER_VERSION VERSION_LESS 16 OR
       NOT CMAKE_GENERATOR MATCHES "^Ninja" OR EMSCRIPTEN OR
       NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        message(FATAL_ERROR "The C++ module pilot currently requires native Linux Clang 16+ and Ninja")
    endif()
    if(KIN_HEX_DEMO_BAKED_FONT)
        message(FATAL_ERROR "The initial C++ module pilot uses the normal Hex Demo font")
    endif()
    add_library(kin_runtime_module STATIC)
    target_sources(kin_runtime_module PUBLIC
        FILE_SET CXX_MODULES
        BASE_DIRS ../../experimental/cpp_modules
        FILES ../../experimental/cpp_modules/kin_runtime.cppm)
    target_link_libraries(kin_runtime_module PRIVATE kin::engine)
    kin_enable_release_optimizations(kin_runtime_module)
    set_target_properties(kin_runtime_module PROPERTIES
        CXX_SCAN_FOR_MODULES ON DISABLE_PRECOMPILE_HEADERS ON)
    target_link_libraries(hex_demo PRIVATE kin_runtime_module)
    target_compile_definitions(hex_demo PRIVATE KIN_HEX_CPP_MODULES=1)
    set_target_properties(hex_demo PROPERTIES
        CXX_SCAN_FOR_MODULES ON KIN_CPP_MODULE_CONSUMER ON)
endif()

if(EMSCRIPTEN)
    set_property(TARGET hex_demo PROPERTY KIN_WEB_EXTRA_LICENSES "${CMAKE_CURRENT_SOURCE_DIR}/FONT-LICENSE.txt")
endif()

target_compile_definitions(hex_demo PRIVATE KIN_HEX_DEMO_BAKED_FONT=$<BOOL:${KIN_HEX_DEMO_BAKED_FONT}>)

target_link_libraries(hex_demo PRIVATE
    kin::engine
)

kin_game(hex_demo TITLE "Hex Demo")
if(KIN_HEX_DEMO_BAKED_FONT)
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/FONT-LICENSE.txt"
        DESTINATION licenses/game COMPONENT hex_demo)
endif()

target_compile_options(hex_demo PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
)

# The default map's path and range, the same on every platform.
add_test(NAME hex-demo
    COMMAND hex_demo --headless --render --frames=10 --report=-
)
set_tests_properties(hex-demo PROPERTIES
    PASS_REGULAR_EXPRESSION "\"path_cells\": 17,[ \n]*\"path_cost\": 170,[ \n]*\"reachable_cells\": 28"
)
