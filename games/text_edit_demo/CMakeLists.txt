add_executable(text_edit_demo
    src/main.cpp
)

target_link_libraries(text_edit_demo PRIVATE
    kin::engine
)

kin_game(text_edit_demo TITLE "Text Edit Demo")

# Stable engine APIs used while editing this game's own sources.
if(KIN_ENABLE_PCH)
    target_precompile_headers(text_edit_demo PRIVATE
        <kin/core/json.hpp>
        <kin/ui2/context.hpp>
        <kin/ui2/text.hpp>
        <kin/ui2/widgets.hpp>)
endif()

target_compile_options(text_edit_demo PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
)

# Headless it lays the editors out: the history overflows and gets a scrollbar,
# and the chat box grows to fit its two lines.
add_test(NAME text-edit-demo
    COMMAND text_edit_demo --headless --frames=5 --report=-
)
set_tests_properties(text-edit-demo PROPERTIES
    PASS_REGULAR_EXPRESSION "\"history_scrollbar\": true.*\"chat_lines\": 2"
)
