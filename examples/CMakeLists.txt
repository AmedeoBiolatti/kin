add_library(kin_example_workloads STATIC
    arena.cpp
    arena_art.cpp
    arena_hud.cpp
    siege_audio.cpp
    siege_scenes.cpp
    power_grid.cpp
    tracker.cpp
    example_app.cpp
)
target_include_directories(kin_example_workloads PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(kin_example_workloads PUBLIC kin::engine)
kin_enable_release_optimizations(kin_example_workloads)

foreach(example signal_siege run_observatory)
    add_executable(${example} ${example}/main.cpp)
    target_link_libraries(${example} PRIVATE kin_example_workloads)
endforeach()
# Signal Siege's content (its language files) is read from content/ while it
# is made, reloading as it changes, and ships as signal_siege.kinpak.
kin_game(signal_siege TITLE "Signal Siege")
kin_game(run_observatory TITLE "Run Observatory" NO_CONTENT)

if(BUILD_TESTING)
    add_executable(kin_example_tests example_tests.cpp)
    target_link_libraries(kin_example_tests PRIVATE kin_example_workloads)
    target_compile_definitions(kin_example_tests PRIVATE KIN_GAME_CONTENT_NAME="signal_siege"
        KIN_GAME_CONTENT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/content")
    target_compile_options(kin_example_tests PRIVATE
        "$<$<CXX_COMPILER_ID:GNU,Clang>:-UNDEBUG>"
        "$<$<CXX_COMPILER_ID:MSVC>:/UNDEBUG>")
    add_test(NAME kin-examples COMMAND kin_example_tests)

    # Signal Siege from a pack of its content, as it ships. The run shows French
    # only if the pack's French file loaded (it falls back to English without),
    # and --fail-on-missing-text fails it if any French text is missing.
    set(_siege_pack "${CMAKE_CURRENT_BINARY_DIR}/signal_siege_test.kinpak")
    add_test(NAME signal-siege-pack-create COMMAND kin_pack create --force "${CMAKE_CURRENT_SOURCE_DIR}/content" "${_siege_pack}")
    add_test(NAME signal-siege-pack-verify COMMAND kin_pack verify "${_siege_pack}")
    add_test(NAME signal-siege-from-pack
        COMMAND signal_siege "--content=${_siege_pack}" --headless --frames=30 --seed=7 --locale=fr
                --fail-on-missing-text --mute --report=-)
    set_tests_properties(signal-siege-pack-create PROPERTIES FIXTURES_SETUP signal_siege_pack)
    set_tests_properties(signal-siege-pack-verify signal-siege-from-pack PROPERTIES FIXTURES_REQUIRED signal_siege_pack)
    set_tests_properties(signal-siege-from-pack PROPERTIES
        PASS_REGULAR_EXPRESSION "\"status\": \"ok\".*\"locale\": \"fr\"")
endif()
