add_executable(hex_demo
    src/main.cpp
)

target_link_libraries(hex_demo PRIVATE
    kin::engine
)

target_compile_options(hex_demo PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
)

# The default map's path and range, the same on every platform.
add_test(NAME hex-demo
    COMMAND hex_demo --headless --frames=10 --report=-
)
set_tests_properties(hex-demo PROPERTIES
    PASS_REGULAR_EXPRESSION "\"path_cells\": 17,[ \n]*\"path_cost\": 170,[ \n]*\"reachable_cells\": 28"
)
