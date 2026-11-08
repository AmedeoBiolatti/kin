add_executable(shapes_demo
    src/main.cpp
)

target_link_libraries(shapes_demo PRIVATE
    kin::engine
)

kin_game(shapes_demo TITLE "Shapes Demo")

# Stable engine APIs used while editing this game's own sources.
if(KIN_ENABLE_PCH)
    target_precompile_headers(shapes_demo PRIVATE
        <kin/core/json.hpp>
        <kin/ecs/render.hpp>
        <kin/platform/window.hpp>
        <kin/renderer/svg.hpp>
        <kin/ui2/text.hpp>)
endif()

target_compile_options(shapes_demo PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
)

# Headless, the shapes are built, read from SVG and drawn.
add_test(NAME shapes-demo
    COMMAND shapes_demo --headless --frames=10 --report=-
)
set_tests_properties(shapes-demo PROPERTIES
    PASS_REGULAR_EXPRESSION "\"svg_elements\": 5.*\"svg_warnings\": 0.*\"bodies\": 10"
)
