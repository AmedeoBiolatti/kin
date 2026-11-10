if(NOT KIN_ENABLE_AUDIO)
    message(FATAL_ERROR "KIN_BUILD_EXAMPLES requires KIN_ENABLE_AUDIO; disable examples for an audio-free build")
endif()
if(KIN_EXPERIMENTAL_3D AND (NOT KIN_ENABLE_PHYSICS3D OR NOT KIN_ENABLE_SCENE3D_RUNTIME))
    message(FATAL_ERROR "The existing 3D examples require KIN_ENABLE_PHYSICS3D and KIN_ENABLE_SCENE3D_RUNTIME; disable KIN_BUILD_EXAMPLES for render-only builds")
endif()

add_library(kin_example_workloads STATIC
    arena.cpp
    arena_art.cpp
    arena_3d.cpp
    arena_hud.cpp
    siege_fonts.cpp
    siege_audio.cpp
    siege_scenes.cpp
    power_grid.cpp
    tracker.cpp
    example_app.cpp
)
target_include_directories(kin_example_workloads PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(kin_example_workloads PUBLIC kin::engine)
target_compile_definitions(kin_example_workloads PRIVATE KIN_EXAMPLE_GAME_VERSION="${PROJECT_VERSION}")
kin_enable_release_optimizations(kin_example_workloads)

foreach(example signal_siege run_observatory)
    add_executable(${example} ${example}/main.cpp)
    target_link_libraries(${example} PRIVATE kin_example_workloads)
endforeach()
# Signal Siege's content (its language files) is read from content/ while it
# is made, reloading as it changes, and ships as signal_siege.kinpak.
kin_game(signal_siege TITLE "Signal Siege" MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/signal_siege/assets.manifest.json")
kin_game(run_observatory TITLE "Run Observatory" NO_CONTENT)

# Signal Breach: Signal Siege first person, on the experimental 3D renderer
# (docs/3d_plan.md); it shares Signal Siege's language files.
if(KIN_EXPERIMENTAL_3D)
    add_library(kin_breach STATIC breach.cpp breach_view.cpp)
    target_include_directories(kin_breach PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(kin_breach PUBLIC kin_example_workloads)
    # The shield's shader, built into it. Without glslc it is not built, and the
    # shield is not drawn.
    kin_compile_glsl(_breach_spv OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders" SOURCES shaders/shield.frag.glsl)
    kin_embed_files(kin_breach NAME breach_shader FILES ${_breach_spv})
    kin_enable_release_optimizations(kin_breach)
    add_executable(signal_breach signal_breach/main.cpp)
    target_link_libraries(signal_breach PRIVATE kin_breach)
    kin_game(signal_breach TITLE "Signal Breach" CONTENT "${CMAKE_CURRENT_SOURCE_DIR}/content")

    # Sponza Walk: a viewer for the 3D renderer, every feature switchable from
    # a panel. Sponza is fetched into a cache (tools/reference/fetch.py), not
    # kept here, so it has no content of its own.
    add_executable(sponza_walk sponza_walk/main.cpp)
    target_link_libraries(sponza_walk PRIVATE kin_example_workloads)
    kin_game(sponza_walk TITLE "Sponza Walk" NO_CONTENT)

    # The 3D starting points: camp_walk, projectile_range and lua_range share one set of CC0 content (camp scenes,
    # Kenney's survival kit and survivor), which the engine's 3D tests also read.
    set(_camp_content "${CMAKE_CURRENT_SOURCE_DIR}/camp_walk/content")
    add_executable(camp_walk camp_walk/main.cpp)
    target_link_libraries(camp_walk PRIVATE kin::engine)
    kin_game(camp_walk TITLE "Camp walk" CONTENT "${_camp_content}")
    add_executable(projectile_range projectile_range/main.cpp)
    target_link_libraries(projectile_range PRIVATE kin::engine)
    kin_game(projectile_range TITLE "Projectile range" CONTENT "${_camp_content}")
    add_executable(lua_range lua_range/main.cpp)
    target_link_libraries(lua_range PRIVATE kin::engine)
    kin_game(lua_range TITLE "Lua range" CONTENT "${_camp_content}")
    if(BUILD_TESTING)
        add_test(NAME lua-range-smoke COMMAND lua_range --headless --frames=240 --seed=7 --mute)
    endif()
endif()

if(KIN_EXPERIMENTAL_3D)
    add_executable(industrial_yard industrial_yard/main.cpp)
    target_link_libraries(industrial_yard PRIVATE kin::engine)
    # planar_views: mirrors, still water and portals; kin's planar surfaces compiled at build time (kin_planar_surfaces).
    if(KIN_GLSLC)
        add_executable(planar_views planar_views/main.cpp)
        target_link_libraries(planar_views PRIVATE kin::engine)
        kin_planar_surfaces(planar_views)
    endif()
    # bunker_exit: auto exposure, decals and morph targets in one small scene (every picture made in code).
    add_executable(bunker_exit bunker_exit/main.cpp)
    target_link_libraries(bunker_exit PRIVATE kin::engine)
endif()

if(BUILD_TESTING)
    add_executable(kin_example_tests example_tests.cpp)
    target_link_libraries(kin_example_tests PRIVATE kin_example_workloads)
    if(KIN_EXPERIMENTAL_3D)
        target_link_libraries(kin_example_tests PRIVATE kin_breach)
    endif()
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
    if(KIN_EXPERIMENTAL_3D)
        # Exercise the Sponza host with a vendored model so clean CI never
        # depends on an external reference cache. The full Sponza benchmark
        # still uses the separately downloaded asset. Required GPU runs fail
        # when the device is unavailable; optional runs may skip.
        add_test(NAME sponza_walk-required-gpu-policy
            COMMAND ${CMAKE_COMMAND} "-DPROGRAM=$<TARGET_FILE:sponza_walk>" -DMODE=sponza
                    -P "${PROJECT_SOURCE_DIR}/tests/required_gpu_smoke.cmake")
        set_tests_properties(sponza_walk-required-gpu-policy PROPERTIES TIMEOUT 90)
        set(_sponza_no_reference "${CMAKE_CURRENT_BINARY_DIR}/sponza_smoke_no_reference")
        file(WRITE "${_sponza_no_reference}" "Smoke tests must use their explicit vendored model.\n")
        add_test(NAME sponza-walk-smoke
            COMMAND sponza_walk --headless --frames 3 --width 480 --height 300
                    --model "${PROJECT_SOURCE_DIR}/tests/fixtures/models/parts.glb" --eye 0,0,3 --target 0,0,0
                    "--screenshot=${CMAKE_CURRENT_BINARY_DIR}/sponza_walk_smoke.png" --preset ao --skip-if-unavailable)
        set_tests_properties(sponza-walk-smoke PROPERTIES ENVIRONMENT "KIN_RENDER_BACKEND=gpu;KIN_REFERENCE_CACHE=${_sponza_no_reference}" SKIP_RETURN_CODE 77
            TIMEOUT 300)
    endif()

    set_tests_properties(signal-siege-pack-create PROPERTIES FIXTURES_SETUP signal_siege_pack)
    set_tests_properties(signal-siege-pack-verify signal-siege-from-pack PROPERTIES FIXTURES_REQUIRED signal_siege_pack)
    set_tests_properties(signal-siege-from-pack PROPERTIES
        PASS_REGULAR_EXPRESSION "\"status\": \"ok\".*\"locale\": \"fr\"")
endif()
