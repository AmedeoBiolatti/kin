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
    kin_enable_release_optimizations(${example})
endforeach()

if(BUILD_TESTING)
    add_executable(kin_example_tests example_tests.cpp)
    target_link_libraries(kin_example_tests PRIVATE kin_example_workloads)
    target_compile_options(kin_example_tests PRIVATE
        "$<$<CXX_COMPILER_ID:GNU,Clang>:-UNDEBUG>"
        "$<$<CXX_COMPILER_ID:MSVC>:/UNDEBUG>")
    add_test(NAME kin-examples COMMAND kin_example_tests)
endif()
