option(KIN_HEX_DEMO_BAKED_FONT "Use the embedded ASCII HUD font in hex_demo" OFF)
add_executable(hex_demo
    src/main.cpp
)

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
