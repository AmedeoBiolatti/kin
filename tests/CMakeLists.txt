# Assertions are part of the test contract. Keep them enabled even when the
# surrounding build uses a Release configuration (whose standard flags define
# NDEBUG), and preserve setup expressions written inside legacy assertions.
add_compile_options(
    "$<$<CXX_COMPILER_ID:GNU,Clang>:-UNDEBUG>"
    "$<$<CXX_COMPILER_ID:MSVC>:/UNDEBUG>"
)

function(kin_add_test target test_name)
    cmake_parse_arguments(TEST "" "" "SOURCES;LIBRARIES;DEFINITIONS;COMPILE_OPTIONS" ${ARGN})
    add_executable(${target} ${TEST_SOURCES})
    target_link_libraries(${target} PRIVATE kin::engine ${TEST_LIBRARIES})
    if(TEST_DEFINITIONS)
        target_compile_definitions(${target} PRIVATE ${TEST_DEFINITIONS})
    endif()
    if(TEST_COMPILE_OPTIONS)
        target_compile_options(${target} PRIVATE ${TEST_COMPILE_OPTIONS})
    endif()
    add_test(NAME ${test_name} COMMAND ${target})
endfunction()

kin_add_test(kin_core_tests kin-core SOURCES core_tests.cpp)
kin_add_test(kin_jobs_tests kin-jobs SOURCES jobs_tests.cpp)
kin_add_test(kin_instrumentation_tests kin-instrumentation
    SOURCES instrumentation_tests.cpp
    LIBRARIES kin::core_allocation_instrumentation
    DEFINITIONS KIN_ENABLE_INSTRUMENTATION
    COMPILE_OPTIONS
        "$<$<CXX_COMPILER_ID:GNU,Clang>:-O0>"
        "$<$<CXX_COMPILER_ID:MSVC>:/Od>"
)
kin_add_test(kin_anim_core_tests kin-anim-core SOURCES anim_core_tests.cpp)
kin_add_test(kin_anim_player_tests kin-anim-player SOURCES anim_player_tests.cpp)
kin_add_test(kin_anim_event_tests kin-anim-events SOURCES anim_event_tests.cpp anim_event_consumer_tests.cpp)
kin_add_test(kin_anim_state_machine_tests kin-anim-state-machine SOURCES anim_state_machine_tests.cpp)
kin_add_test(kin_anim_format_tests kin-anim-format SOURCES anim_format_tests.cpp)
kin_add_test(kin_ecs_tests kin-ecs SOURCES ecs_tests.cpp)
kin_add_test(kin_ecs_entity_tests kin-ecs-entity SOURCES ecs_entity_tests.cpp)
kin_add_test(kin_ecs_events_tests kin-ecs-events SOURCES ecs_events_tests.cpp)
kin_add_test(kin_ecs_component_tests kin-ecs-component SOURCES ecs_component_tests.cpp)
kin_add_test(kin_ecs_inspection_tests kin-ecs-inspection SOURCES ecs_inspection_tests.cpp)
kin_add_test(kin_ecs_editor_tests kin-ecs-editor SOURCES ecs_editor_tests.cpp)
kin_add_test(kin_ecs_system_tests kin-ecs-system SOURCES ecs_system_tests.cpp)
kin_add_test(kin_ecs_render_tests kin-ecs-render SOURCES ecs_render_tests.cpp)
kin_add_test(kin_ecs_animation_tests kin-ecs-animation SOURCES ecs_animation_tests.cpp)
kin_add_test(kin_particle_tests kin-particles SOURCES particle_tests.cpp)
kin_add_test(kin_pathfinding_tests kin-pathfinding SOURCES pathfinding_tests.cpp)
kin_add_test(kin_hex_tests kin-hex SOURCES hex_tests.cpp)
kin_add_test(kin_path_server_tests kin-path-server SOURCES path_server_tests.cpp)
kin_add_test(kin_physics_tests kin-physics SOURCES physics_tests.cpp)
kin_add_test(kin_ecs_physics_tests kin-ecs-physics SOURCES ecs_physics_tests.cpp)
kin_add_test(kin_ecs_time_tests kin-ecs-time SOURCES ecs_time_tests.cpp)
kin_add_test(kin_ecs_spawn_tests kin-ecs-spawn SOURCES ecs_spawn_tests.cpp)
kin_add_test(kin_prefab_tests kin-prefab SOURCES prefab_tests.cpp prefab_data_tests.cpp)
kin_add_test(kin_ecs_lifecycle_tests kin-ecs-lifecycle SOURCES ecs_lifecycle_tests.cpp)
kin_add_test(kin_ui2_tests kin-ui2 SOURCES ui2_tests.cpp)
kin_add_test(kin_ui2_text_edit_tests kin-ui2-text-edit SOURCES ui2_text_edit_tests.cpp)
kin_add_test(kin_ui2_theme_file_tests kin-ui2-theme-file SOURCES ui2_theme_file_tests.cpp)
kin_add_test(kin_dialogue_tests kin-dialogue SOURCES dialogue_tests.cpp)
kin_add_test(kin_assets_tests kin-assets SOURCES assets_tests.cpp anim_asset_loader_tests.cpp)
kin_add_test(kin_file_watcher_tests kin-file-watcher SOURCES file_watcher_tests.cpp)
kin_add_test(kin_lua_script_tests kin-lua-script SOURCES lua_script_tests.cpp)
kin_add_test(kin_audio_tests kin-audio SOURCES audio_tests.cpp)
kin_add_test(kin_platform_tests kin-platform SOURCES platform_tests.cpp)
kin_add_test(kin_platform_io_tests kin-platform-io SOURCES platform_io_tests.cpp)
kin_add_test(kin_save_tests kin-save SOURCES save_tests.cpp)
kin_add_test(kin_renderer_tests kin-renderer SOURCES renderer_tests.cpp)
# Test-only material shaders for the GPU renderer tests. Without glslc (found by the
# engine) they are not compiled and the tests that need them skip.
set(_kin_test_shader_dir "${CMAKE_CURRENT_BINARY_DIR}/shaders")
target_compile_definitions(kin_renderer_tests PRIVATE KIN_TEST_SHADER_DIR="${_kin_test_shader_dir}")
target_compile_definitions(kin_renderer_tests PRIVATE
    KIN_GPU_SHADER_DIR_FOR_TESTS="${CMAKE_BINARY_DIR}/engine/shaders")
if(KIN_GLSLC)
    file(MAKE_DIRECTORY "${_kin_test_shader_dir}")
    set(_kin_test_spv)
    foreach(_name four_sources data_formats custom_vertex)
        add_custom_command(
            OUTPUT "${_kin_test_shader_dir}/${_name}.frag.spv"
            COMMAND "${KIN_GLSLC}" -fshader-stage=fragment
                    -o "${_kin_test_shader_dir}/${_name}.frag.spv"
                    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${_name}.frag.glsl"
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${_name}.frag.glsl"
            COMMENT "glslc: ${_name}.frag.glsl -> ${_name}.frag.spv"
            VERBATIM)
        list(APPEND _kin_test_spv "${_kin_test_shader_dir}/${_name}.frag.spv")
    endforeach()
    add_custom_target(kin_test_shaders DEPENDS ${_kin_test_spv})
    add_dependencies(kin_renderer_tests kin_test_shaders)
endif()
kin_add_test(kin_rendering_tests kin-rendering SOURCES rendering_tests.cpp)
kin_add_test(kin_isometric_demo_tests kin-isometric-demo SOURCES isometric_demo_tests.cpp)
kin_add_test(kin_tilemap_tests kin-tilemap SOURCES tilemap_tests.cpp)

if(KIN_BUILD_BENCHMARKS)
    add_executable(kin_bench
        tilemap_bench.cpp
        input_latency_bench.cpp
    )

    target_link_libraries(kin_bench PRIVATE
        kin::engine
        kin::core_allocation_instrumentation
    )

    target_compile_definitions(kin_bench PRIVATE
        KIN_ENABLE_INSTRUMENTATION
    )

    # SDL_GPU texture uploads: many data textures created and updated a frame.
    add_executable(kin_upload_bench upload_bench.cpp)
    target_link_libraries(kin_upload_bench PRIVATE kin::engine)
    # SDL_GPU quads: draw_texture, fill_rect and draw_sprites, many a frame.
    add_executable(kin_draw_bench draw_bench.cpp)
    target_link_libraries(kin_draw_bench PRIVATE kin::engine)

    if(TARGET kin_example_workloads)
        target_link_libraries(kin_bench PRIVATE kin_example_workloads)
        target_compile_definitions(kin_bench PRIVATE KIN_BENCH_HAS_EXAMPLES)
    endif()

    kin_enable_release_optimizations(kin_bench)
endif()

add_executable(kin_runtime_tests
    runtime_tests.cpp
)

target_link_libraries(kin_runtime_tests PRIVATE
    kin::engine
)

add_test(NAME kin-runtime COMMAND kin_runtime_tests)

if(KIN_ENABLE_RENDER_PROBE)
    kin_add_test(kin_render_probe_tests kin-render-probe SOURCES render_probe_tests.cpp)
endif()
if(KIN_ENABLE_DETERMINISM_CHECK)
    kin_add_test(kin_determinism_tests kin-determinism SOURCES determinism_tests.cpp)
endif()

add_executable(kin_server_tests
    server_tests.cpp
)

target_link_libraries(kin_server_tests PRIVATE
    kin::engine
)

add_test(NAME kin-server COMMAND kin_server_tests)

add_executable(kin_scene_tests
    scene_tests.cpp
)

target_link_libraries(kin_scene_tests PRIVATE
    kin::engine
)

add_test(NAME kin-scene COMMAND kin_scene_tests)

add_executable(kin_scene_asset_tests
    scene_asset_tests.cpp
)

target_link_libraries(kin_scene_asset_tests PRIVATE
    kin::engine
)

add_test(NAME kin-scene-asset COMMAND kin_scene_asset_tests)

add_executable(kin_scripting_tests
    scripting_tests.cpp
)

target_link_libraries(kin_scripting_tests PRIVATE
    kin::engine
)

add_test(NAME kin-scripting COMMAND kin_scripting_tests)

