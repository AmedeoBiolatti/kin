add_executable(charts_demo
    src/main.cpp
)

target_link_libraries(charts_demo PRIVATE
    kin::engine
)

kin_game(charts_demo TITLE "Charts Demo")

target_compile_options(charts_demo PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
)

# Headless it draws the charts: the waveform's view is its two seconds, the
# frame times are fitted to a round value, and the gain is where it starts.
add_test(NAME charts-demo
    COMMAND charts_demo --headless --frames=30 --report=-
)
set_tests_properties(charts-demo PROPERTIES
    PASS_REGULAR_EXPRESSION "\"spectrogram_columns\": 360.*\"wave_view_max\": 1\\.99.*\"times_view_y_max\": (10|15|20).*\"histogram_bars\": 16.*\"gain_db\": 0"
)
