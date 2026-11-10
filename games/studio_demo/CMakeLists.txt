add_executable(studio_demo
    src/main.cpp
)

target_link_libraries(studio_demo PRIVATE
    kin::engine
)

kin_game(studio_demo TITLE "Studio Demo" NO_CONTENT)

target_compile_options(studio_demo PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
)

# Headless it lays the window out: the explorer has 20000+ rows, of which only
# the visible few are built each frame.
add_test(NAME studio-demo
    COMMAND studio_demo --headless --frames=5 --report=-
)
set_tests_properties(studio-demo PROPERTIES
    PASS_REGULAR_EXPRESSION "\"tree_rows_built\": [1-3]?[0-9],"
)
