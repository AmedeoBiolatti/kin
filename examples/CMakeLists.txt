if(NOT KIN_ENABLE_AUDIO)
    message(FATAL_ERROR "KIN_BUILD_EXAMPLES requires KIN_ENABLE_AUDIO; disable examples for an audio-free build")
endif()

add_library(kin_example_workloads STATIC
    arena.cpp
    arena_art.cpp
    arena_3d.cpp
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

    # Summit: a one-player Peak-like on the 3D renderer and physics (docs/3d_plan.md, P4). The simulation is a library with no
    # window or renderer (summit/level, game, input_mapper, camera, script); the executable is its view.
    add_library(kin_summit_sim STATIC summit/level.cpp summit/game.cpp summit/script.cpp summit/input_map.cpp summit/camera.cpp
        summit/rock.cpp summit/climber.cpp summit/settings.cpp summit/environment.cpp summit/hud.cpp summit/audio.cpp summit/expedition.cpp summit/props.cpp)
    target_include_directories(kin_summit_sim PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(kin_summit_sim PUBLIC kin::engine)
    kin_enable_release_optimizations(kin_summit_sim)
    add_executable(summit summit/main.cpp summit/view.cpp summit/hud_draw.cpp summit/external_assets.cpp summit/benchmark.cpp)
    target_link_libraries(summit PRIVATE kin_summit_sim kin_example_workloads)
    kin_game(summit TITLE "Summit" CONTENT "${CMAKE_CURRENT_SOURCE_DIR}/summit/content")
    add_executable(camp_walk camp_walk/main.cpp)
    target_link_libraries(camp_walk PRIVATE kin::engine)
    kin_game(camp_walk TITLE "Camp walk" CONTENT "${CMAKE_CURRENT_SOURCE_DIR}/summit/content")
    add_executable(projectile_range projectile_range/main.cpp)
    target_link_libraries(projectile_range PRIVATE kin::engine)
    kin_game(projectile_range TITLE "Projectile range" CONTENT "${CMAKE_CURRENT_SOURCE_DIR}/summit/content")
    add_executable(lua_range lua_range/main.cpp)
    target_link_libraries(lua_range PRIVATE kin::engine)
    kin_game(lua_range TITLE "Lua range" CONTENT "${CMAKE_CURRENT_SOURCE_DIR}/summit/content")
    if(BUILD_TESTING)
        add_test(NAME lua-range-smoke COMMAND lua_range --headless --frames=240 --seed=7 --mute)
    endif()
    if(BUILD_TESTING)
        add_executable(kin_summit_asset_tests summit/asset_tests.cpp)
        target_link_libraries(kin_summit_asset_tests PRIVATE kin_summit_sim)
        target_compile_definitions(kin_summit_asset_tests PRIVATE SUMMIT_ASSET_CONTENT="${CMAKE_CURRENT_SOURCE_DIR}/summit/content")
        add_test(NAME kin-summit-assets COMMAND kin_summit_asset_tests)
        add_executable(kin_summit_tests summit/summit_tests.cpp)
        target_link_libraries(kin_summit_tests PRIVATE kin_summit_sim)
        add_test(NAME kin-summit COMMAND kin_summit_tests)
        add_executable(kin_summit_expedition_tests summit/expedition_tests.cpp)
        target_link_libraries(kin_summit_expedition_tests PRIVATE kin_summit_sim)
        add_test(NAME kin-summit-expedition COMMAND kin_summit_expedition_tests)
        set_tests_properties(kin-summit-expedition PROPERTIES TIMEOUT 120)
        add_test(NAME summit-script-expedition COMMAND summit --script expedition --seed 1)
        add_test(NAME summit-script-shelter COMMAND summit --script shelter --seed 1)
        set_tests_properties(summit-script-expedition summit-script-shelter PROPERTIES TIMEOUT 120)
        add_test(NAME summit-expedition-shots
            COMMAND summit --headless --script shelter --seed 1 --width 320 --height 180 --samples 1 --max-steps 12000
                    "--screenshot=${CMAKE_CURRENT_BINARY_DIR}/summit_shots/expedition" --skip-if-unavailable)
        set_tests_properties(summit-expedition-shots PROPERTIES ENVIRONMENT "KIN_RENDER_BACKEND=gpu" SKIP_RETURN_CODE 77 TIMEOUT 120
            PASS_REGULAR_EXPRESSION "summit_shots/expedition_complete.png")
        # Summit's presentation on the CPU (P4b): HUD model and layout, menu, sound cues and curves, environment, settings, and
        # that none of it changes the simulation.
        add_executable(kin_summit_presentation_tests summit/presentation_tests.cpp)
        target_link_libraries(kin_summit_presentation_tests PRIVATE kin_summit_sim)
        add_test(NAME kin-summit-presentation COMMAND kin_summit_presentation_tests)
        set_tests_properties(kin-summit-presentation PROPERTIES TIMEOUT 300)
        # The scripted runs: a CPU run with no window (the route bot to the summit, the same with a fall), and the same with
        # pictures at key moments, small, which skips (77) without a 3D device and fails without one when
        # KIN_REQUIRE_GPU_TESTS is set (lavapipe is enough). Unit/presentation replay
        # tests compare hashes within this build; no machine-specific golden is portable.
        add_test(NAME summit-script-climb
            COMMAND summit --script climb --seed 1 --timing)
        add_test(NAME summit-script-fall COMMAND summit --script fall --seed 1)
        add_test(NAME summit-script-shots
            COMMAND summit --headless --script climb --seed 1 --width 320 --height 180 --samples 1
                    "--screenshot=${CMAKE_CURRENT_BINARY_DIR}/summit_shots/climb" --skip-if-unavailable)
        set_tests_properties(summit-script-shots PROPERTIES ENVIRONMENT "KIN_RENDER_BACKEND=gpu" SKIP_RETURN_CODE 77 TIMEOUT 300
            PASS_REGULAR_EXPRESSION "summit_shots/climb_summit.png")
        # The same with the HUD hidden and the sound muted (the flags a picture or a CI run uses).
        add_test(NAME summit-script-shots-nohud
            COMMAND summit --headless --script climb --seed 1 --width 320 --height 180 --samples 1 --no-hud --mute
                    "--screenshot=${CMAKE_CURRENT_BINARY_DIR}/summit_shots/nohud" --skip-if-unavailable)
        set_tests_properties(summit-script-shots-nohud PROPERTIES ENVIRONMENT "KIN_RENDER_BACKEND=gpu" SKIP_RETURN_CODE 77 TIMEOUT 300
            PASS_REGULAR_EXPRESSION "summit_shots/nohud_summit.png")
        set_tests_properties(summit-script-climb summit-script-fall PROPERTIES TIMEOUT 120)
    endif()
endif()

if(KIN_EXPERIMENTAL_3D)
    add_library(kin_blockfield_world STATIC blockfield/world.cpp)
    target_include_directories(kin_blockfield_world PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/blockfield")
    target_link_libraries(kin_blockfield_world PUBLIC kin::engine)
    kin_enable_release_optimizations(kin_blockfield_world)
    add_executable(blockfield blockfield/main.cpp)
    target_link_libraries(blockfield PRIVATE kin_blockfield_world)
    kin_game(blockfield TITLE "Blockfield" NO_CONTENT)
    add_executable(blockfield_render_compare blockfield/render_compare.cpp)
    target_link_libraries(blockfield_render_compare PRIVATE kin_blockfield_world)
    add_executable(industrial_yard industrial_yard/main.cpp)
    target_link_libraries(industrial_yard PRIVATE kin::engine)
    if(BUILD_TESTING)
        add_executable(kin_blockfield_tests blockfield/tests.cpp)
        target_link_libraries(kin_blockfield_tests PRIVATE kin_blockfield_world)
        add_test(NAME kin-blockfield COMMAND kin_blockfield_tests)
        set_tests_properties(kin-blockfield PROPERTIES TIMEOUT 60)
    endif()
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
        foreach(_example IN ITEMS sponza_walk summit)
            if(_example STREQUAL "sponza_walk")
                set(_smoke_mode sponza)
            else()
                set(_smoke_mode summit)
            endif()
            add_test(NAME ${_example}-required-gpu-policy
                COMMAND ${CMAKE_COMMAND} "-DPROGRAM=$<TARGET_FILE:${_example}>" "-DMODE=${_smoke_mode}"
                        -P "${PROJECT_SOURCE_DIR}/tests/required_gpu_smoke.cmake")
            set_tests_properties(${_example}-required-gpu-policy PROPERTIES TIMEOUT 90)
        endforeach()
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
