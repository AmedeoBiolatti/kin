add_executable(shapes_demo
    src/main.cpp
)

target_link_libraries(shapes_demo PRIVATE
    kin::engine
)

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
