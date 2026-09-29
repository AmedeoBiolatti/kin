add_executable(lighting_demo
    src/main.cpp
)

target_link_libraries(lighting_demo PRIVATE
    kin::engine
)

target_compile_options(lighting_demo PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
)

# Headless, the software backend lights the scene too.
add_test(NAME lighting-demo
    COMMAND lighting_demo --headless --frames=10 --report=-
)
set_tests_properties(lighting-demo PROPERTIES
    PASS_REGULAR_EXPRESSION "\"time_of_day\": \"night\".*\"lit\": true"
)
