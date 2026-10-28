# Assertions are part of the test contract. Keep them enabled even when the
# surrounding build uses a Release configuration (whose standard flags define
# NDEBUG), and preserve setup expressions written inside legacy assertions.
add_compile_options(
    "$<$<CXX_COMPILER_ID:GNU,Clang>:-UNDEBUG>"
    "$<$<CXX_COMPILER_ID:MSVC>:/UNDEBUG>"
)

find_package(Python3 QUIET COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_GENERATOR STREQUAL "Ninja")
        get_property(component_manifest GLOBAL PROPERTY KIN_COMPONENT_MANIFEST)
        add_test(NAME kin-component-targets
            COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/check_component_targets.py"
                --build "${CMAKE_BINARY_DIR}" --manifest "${component_manifest}"
                --output "${CMAKE_BINARY_DIR}/component-reports/targets-$<CONFIG>.json")
    endif()
    add_test(NAME kin-components
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/component_config_tests.py")
    add_test(NAME kin-release-acceptance-tools
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/release_acceptance_tests.py")
    add_test(NAME kin-release-publishing
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/release_publishing_tests.py")
    add_test(NAME kin-bundled-fonts
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/check_bundled_fonts.py")
    add_test(NAME kin-asset-cooking
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/asset_cooking_tests.py")
    add_test(NAME kin-asset-report
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/asset_report_tests.py")
endif()

function(kin_add_test target test_name)
    cmake_parse_arguments(TEST "" "LINK_TARGET" "SOURCES;LIBRARIES;DEFINITIONS;COMPILE_OPTIONS" ${ARGN})
    add_executable(${target} ${TEST_SOURCES})
    if(NOT TEST_LINK_TARGET)
        set(TEST_LINK_TARGET kin::engine)
    endif()
    target_link_libraries(${target} PRIVATE ${TEST_LINK_TARGET} ${TEST_LIBRARIES})
    if(TEST_DEFINITIONS)
        target_compile_definitions(${target} PRIVATE ${TEST_DEFINITIONS})
    endif()
    if(TEST_COMPILE_OPTIONS)
        target_compile_options(${target} PRIVATE ${TEST_COMPILE_OPTIONS})
    endif()
    add_test(NAME ${test_name} COMMAND ${target})
endfunction()

# Component-owned tests use the same registry as configuration and packaging.
function(kin_add_component_test component target test_name)
    get_property(declared GLOBAL PROPERTY KIN_COMPONENT_${component}_TESTS)
    if(NOT test_name IN_LIST declared)
        message(FATAL_ERROR "Test ${test_name} is not declared by component ${component}")
    endif()
    if(KIN_ENABLE_${component})
        kin_add_test(${target} ${test_name} ${ARGN})
        set_tests_properties(${test_name} PROPERTIES LABELS "component:${component}")
    endif()
endfunction()

kin_add_test(kin_component_tests kin-component-config SOURCES component_tests.cpp)
kin_add_test(kin_core_tests kin-core SOURCES core_tests.cpp)
kin_add_test(kin_math3d_tests kin-math3d SOURCES math3d_tests.cpp)
kin_add_test(kin_render3d_api_tests kin-render3d-api SOURCES render3d_api_tests.cpp)
kin_add_test(kin_shape_tests kin-shapes SOURCES shape_tests.cpp)
kin_add_test(kin_jobs_tests kin-jobs SOURCES jobs_tests.cpp)
kin_add_test(kin_serial_jobs_tests kin-serial-jobs SOURCES serial_jobs_tests.cpp LINK_TARGET kin_foundation)
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
kin_add_test(kin_navmesh3d_tests kin-navmesh3d SOURCES navmesh3d_tests.cpp)
kin_add_test(kin_hex_tests kin-hex SOURCES hex_tests.cpp)
kin_add_test(kin_hex_demo_tests kin-hex-demo SOURCES hex_demo_tests.cpp
    DEFINITIONS KIN_HEX_DEMO_BAKED_FONT=$<BOOL:${KIN_HEX_DEMO_BAKED_FONT}>)
kin_add_test(kin_path_server_tests kin-path-server SOURCES path_server_tests.cpp)
kin_add_component_test(PHYSICS kin_physics_tests kin-physics
    SOURCES physics_tests.cpp LINK_TARGET kin::physics_render)
kin_add_component_test(PHYSICS kin_physics_component_tests kin-physics-component
    SOURCES physics_component_tests.cpp LINK_TARGET kin::physics)
kin_add_component_test(PHYSICS kin_ecs_physics_tests kin-ecs-physics
    SOURCES ecs_physics_tests.cpp LINK_TARGET kin::physics_ecs)
kin_add_test(kin_ecs_time_tests kin-ecs-time SOURCES ecs_time_tests.cpp)
kin_add_test(kin_ecs_spawn_tests kin-ecs-spawn SOURCES ecs_spawn_tests.cpp)
kin_add_test(kin_prefab_tests kin-prefab SOURCES prefab_tests.cpp prefab_data_tests.cpp)
kin_add_test(kin_prefab_structure_tests kin-prefab-structure SOURCES prefab_structure_tests.cpp)
kin_add_test(kin_ecs_lifecycle_tests kin-ecs-lifecycle SOURCES ecs_lifecycle_tests.cpp)
kin_add_test(kin_ui2_tests kin-ui2 SOURCES ui2_tests.cpp)
kin_add_test(kin_ui2_text_edit_tests kin-ui2-text-edit SOURCES ui2_text_edit_tests.cpp)
kin_add_test(kin_ui2_text_tests kin-ui2-text SOURCES ui2_text_tests.cpp)
kin_add_test(kin_ui2_theme_file_tests kin-ui2-theme-file SOURCES ui2_theme_file_tests.cpp)
kin_add_test(kin_dialogue_tests kin-dialogue SOURCES dialogue_tests.cpp)
kin_add_test(kin_l10n_tests kin-l10n SOURCES l10n_tests.cpp)
kin_add_test(kin_bidi_tests kin-bidi SOURCES bidi_tests.cpp)
if(KIN_ENABLE_IMAGE_IO)
    kin_add_test(kin_assets_tests kin-assets SOURCES assets_tests.cpp anim_asset_loader_tests.cpp)
else()
    kin_add_test(kin_assets_tests kin-assets SOURCES assets_no_image_tests.cpp anim_asset_loader_tests.cpp)
endif()
kin_add_test(kin_shipping_features_tests kin-shipping-features SOURCES shipping_features_tests.cpp)
kin_add_test(kin_sdl_capabilities_tests kin-sdl-capabilities SOURCES sdl_capabilities_tests.cpp
    DEFINITIONS KIN_TEST_SDL_GAMEPAD=$<BOOL:${KIN_ENABLE_SDL_GAMEPAD}>
        "KIN_TEST_SDL_HAS_X11=$<IN_LIST:x11,${KIN_SDL_VIDEO_DRIVERS}>"
        "KIN_TEST_SDL_HAS_WAYLAND=$<IN_LIST:wayland,${KIN_SDL_VIDEO_DRIVERS}>")
kin_add_test(kin_content_tests kin-content SOURCES content_tests.cpp)
kin_add_test(kin_file_watcher_tests kin-file-watcher SOURCES file_watcher_tests.cpp)
kin_add_component_test(SCRIPTING kin_lua_script_tests kin-lua-script SOURCES lua_script_tests.cpp LINK_TARGET kin::scripting)
kin_add_component_test(AUDIO kin_audio_tests kin-audio SOURCES audio_tests.cpp LINK_TARGET kin::audio_ecs
    DEFINITIONS KIN_AUDIO_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/audio")
if(KIN_ENABLE_SCRIPTING AND KIN_ENABLE_AUDIO)
    target_link_libraries(kin_audio_tests PRIVATE kin::scripting_scenes)
endif()
kin_add_component_test(AUDIO kin_audio_component_tests kin-audio-component SOURCES audio_component_tests.cpp LINK_TARGET kin::audio)
kin_add_test(kin_platform_tests kin-platform SOURCES platform_tests.cpp)
kin_add_test(kin_platform_io_tests kin-platform-io SOURCES platform_io_tests.cpp)
if(CMAKE_SYSTEM_NAME MATCHES "^(Windows|Linux)$")
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_GENERATOR STREQUAL "Ninja" AND
       CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND KIN_PGO_MODE STREQUAL "off")
        find_package(Python3 QUIET COMPONENTS Interpreter)
        if(Python3_Interpreter_FOUND)
            add_test(NAME kin-compiler-pgo
                COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/compiler_pgo_tests.py"
                    --build "${CMAKE_CURRENT_BINARY_DIR}/compiler-pgo"
                    --cc "${CMAKE_C_COMPILER}" --cxx "${CMAKE_CXX_COMPILER}")
        endif()
    endif()
    add_test(NAME kin-release-policy
        COMMAND "${CMAKE_COMMAND}"
            "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
            "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}"
            "-DCONFIG=$<CONFIG>" "-DGENERATOR=${CMAKE_GENERATOR}"
            "-DCC=${CMAKE_C_COMPILER}" "-DCXX=${CMAKE_CXX_COMPILER}"
            "-DLTO=${KIN_ENABLE_LTO}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/release_policy_tests.cmake"
        CONFIGURATIONS Release RelWithDebInfo MinSizeRel)
    add_library(kin_desktop_package_library SHARED desktop_package_library.cpp)
    add_executable(kin_desktop_package_fixture desktop_package_fixture.cpp)
    target_link_libraries(kin_desktop_package_fixture PRIVATE kin::engine kin_desktop_package_library)
    # Exercise the low-level loose-assets exporter; real Signal Siege acceptance
    # covers the canonical kin_game/.kinpak path.
    kin_configure_asset_pipeline(kin_desktop_package_fixture package_fixture
        "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/desktop_package/assets"
        "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/desktop_package/assets.manifest.json")
    kin_package_game(kin_desktop_package_fixture RUNTIME_TARGETS kin_desktop_package_library)
    get_target_property(_package_script kin_desktop_package_fixture KIN_PACKAGE_SCRIPT)
    get_target_property(_package_name kin_desktop_package_fixture KIN_PACKAGE_NAME)
    add_test(NAME kin-desktop-package
        COMMAND "${CMAKE_COMMAND}"
            "-DPACKAGE_SCRIPT=${_package_script}"
            "-DPACKAGE_NAME=${_package_name}"
            "-DBUILD_DIR=${CMAKE_BINARY_DIR}"
            "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}"
            "-DCONFIG=$<CONFIG>"
            "-DEXECUTABLE=$<TARGET_FILE_NAME:kin_desktop_package_fixture>"
            "-DBUILD_EXECUTABLE=$<TARGET_FILE:kin_desktop_package_fixture>"
            "-DDEBUG_SYMBOLS=${KIN_PACKAGE_DEBUG_SYMBOLS}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/desktop_package_tests.cmake"
        CONFIGURATIONS Release RelWithDebInfo MinSizeRel)
endif()
kin_add_test(kin_save_tests kin-save SOURCES save_tests.cpp)
kin_add_test(kin_texture_file_tests kin-texture-file SOURCES texture_file_tests.cpp)
kin_add_test(kin_renderer_tests kin-renderer SOURCES renderer_tests.cpp)
if(KIN_HAS_COMPACT_RENDERER)
    kin_add_test(kin_compact_renderer_tests kin-compact-renderer SOURCES compact_renderer_tests.cpp)
endif()
# Test-only material shaders for the GPU renderer tests. Without glslc (found by the
# engine) they are not compiled and the tests that need them skip.
set(_kin_test_shader_dir "${CMAKE_CURRENT_BINARY_DIR}/shaders")
target_compile_definitions(kin_renderer_tests PRIVATE KIN_TEST_SHADER_DIR="${_kin_test_shader_dir}")
target_compile_definitions(kin_renderer_tests PRIVATE
    KIN_GPU_SHADER_DIR_FOR_TESTS="${CMAKE_BINARY_DIR}/engine/shaders")
if(KIN_GLSLC AND KIN_HAS_GPU_RENDERER)
    file(MAKE_DIRECTORY "${_kin_test_shader_dir}")
    set(_kin_test_spv)
    foreach(_name four_sources data_formats custom_vertex storage_read bench_read_texture bench_read_buffer bench_heavy)
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
    foreach(_name compute_gradient bench_light_field)
        add_custom_command(
            OUTPUT "${_kin_test_shader_dir}/${_name}.comp.spv"
            COMMAND "${KIN_GLSLC}" -fshader-stage=compute
                    -o "${_kin_test_shader_dir}/${_name}.comp.spv"
                    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${_name}.comp.glsl"
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${_name}.comp.glsl"
            COMMENT "glslc: ${_name}.comp.glsl -> ${_name}.comp.spv"
            VERBATIM)
        list(APPEND _kin_test_spv "${_kin_test_shader_dir}/${_name}.comp.spv")
    endforeach()
    add_custom_target(kin_test_shaders DEPENDS ${_kin_test_spv})
    add_dependencies(kin_renderer_tests kin_test_shaders)
endif()
kin_add_test(kin_rendering_tests kin-rendering SOURCES rendering_tests.cpp)
kin_add_test(kin_resource_graph_tests kin-resource-graph SOURCES resource_graph_tests.cpp)
if(KIN_HAS_GPU_RENDERER)
    kin_add_test(kin_gpu_memory_tests kin-gpu-memory SOURCES gpu_memory_tests.cpp)
    set_tests_properties(kin-gpu-memory PROPERTIES SKIP_RETURN_CODE 77)
endif()
if(KIN_EXPERIMENTAL_3D AND KIN_ENABLE_SCENE3D_RUNTIME)
    kin_add_test(kin_model_profile3d_tests kin-model-profile3d SOURCES model_profile3d_tests.cpp
        DEFINITIONS KIN_TEST_MODELS="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/models")
    add_test(NAME kin-model-profile3d-gpu COMMAND kin_model_profile3d_tests gpu)
    set_tests_properties(kin-model-profile3d-gpu PROPERTIES SKIP_RETURN_CODE 77)
    kin_add_test(kin_script_scene3d_tests kin-script-scene3d SOURCES script_scene3d_tests.cpp script_scene3d_advanced_tests.cpp
        DEFINITIONS KIN_TEST_MODELS="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/models")
    add_test(NAME kin-script-scene3d-gpu COMMAND kin_script_scene3d_tests gpu)
    set_tests_properties(kin-script-scene3d-gpu PROPERTIES SKIP_RETURN_CODE 77)
    kin_add_test(kin_errors3d_tests kin-errors3d SOURCES errors3d_tests.cpp
        DEFINITIONS KIN_TEST_MODELS="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/models")
    kin_add_test(kin_scene_frame3d_tests kin-scene-frame3d SOURCES scene_frame3d_tests.cpp
        DEFINITIONS KIN_TEST_MODELS="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/models")
    kin_add_test(kin_diagnostics3d_tests kin-diagnostics3d SOURCES diagnostics3d_tests.cpp)
    kin_add_test(kin_render3d_cpu_tests kin-render3d-cpu SOURCES render3d_cpu_tests.cpp)
    kin_add_test(kin_render3d_tracking_tests kin-render3d-tracking SOURCES render3d_tracking_tests.cpp)
    kin_add_test(kin_interaction3d_tests kin-interaction3d SOURCES interaction3d_tests.cpp
        LIBRARIES kin::core_allocation_instrumentation)
    set_tests_properties(kin-interaction3d PROPERTIES SKIP_RETURN_CODE 77)
    kin_add_test(kin_character_action3d_tests kin-character-actions3d SOURCES character_action3d_tests.cpp
        LIBRARIES kin::core_allocation_instrumentation)
    kin_add_test(kin_animation_event3d_tests kin-animation-events3d SOURCES animation_event3d_tests.cpp
        LIBRARIES kin::core_allocation_instrumentation)
    kin_add_test(kin_joint_attachment3d_tests kin-joint-attachment3d SOURCES joint_attachment3d_tests.cpp
        LIBRARIES kin::core_allocation_instrumentation)
    set_tests_properties(kin-joint-attachment3d PROPERTIES SKIP_RETURN_CODE 77)
    kin_add_test(kin_camera_rig3d_tests kin-camera-rig3d SOURCES camera_rig3d_tests.cpp
        LIBRARIES kin::core_allocation_instrumentation
        DEFINITIONS KIN_TEST_MODELS="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/models")
    set_tests_properties(kin-camera-rig3d PROPERTIES SKIP_RETURN_CODE 77)
    kin_add_test(kin_physics3d_query_tests kin-physics3d-queries SOURCES physics3d_query_tests.cpp
        LIBRARIES kin::core_allocation_instrumentation)
    kin_add_test(kin_scene_queries3d_tests kin-scene-queries3d SOURCES scene_queries3d_tests.cpp
        LIBRARIES kin::core_allocation_instrumentation)
    set_tests_properties(kin-scene-queries3d PROPERTIES SKIP_RETURN_CODE 77)
    kin_add_test(kin_camp_input_tests kin-camp-input SOURCES camp_input_tests.cpp)
    kin_add_test(kin_camp_gameplay3d_tests kin-camp-gameplay3d SOURCES camp_gameplay3d_tests.cpp
        DEFINITIONS KIN_TEST_SUMMIT="${PROJECT_SOURCE_DIR}/examples/summit/content")
    set_tests_properties(kin-camp-gameplay3d PROPERTIES SKIP_RETURN_CODE 77)
    add_test(NAME kin-camp-push COMMAND kin_camp_gameplay3d_tests push)
    set_tests_properties(kin-camp-push PROPERTIES SKIP_RETURN_CODE 77)
    # Isolate GPU/Jolt setup/retirement between cases on RAM-constrained hosts.
    foreach(_case RANGE 0 7)
        add_test(NAME kin-camp-action-${_case} COMMAND kin_camp_gameplay3d_tests action ${_case})
        set_tests_properties(kin-camp-action-${_case} PROPERTIES SKIP_RETURN_CODE 77)
    endforeach()
    kin_add_test(kin_scene_pipeline3d_tests kin-scene-pipeline3d SOURCES scene_pipeline3d_tests.cpp)
    set_tests_properties(kin-scene-pipeline3d PROPERTIES SKIP_RETURN_CODE 77)
    kin_add_test(kin_prefab_pool3d_tests kin-prefab-pool3d SOURCES prefab_pool3d_tests.cpp
        DEFINITIONS KIN_TEST_MODELS="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/models"
                    KIN_TEST_SUMMIT="${PROJECT_SOURCE_DIR}/examples/summit/content")
    set_tests_properties(kin-prefab-pool3d PROPERTIES SKIP_RETURN_CODE 77)
    kin_add_test(kin_runtime_scene3d_tests kin-runtime-scene3d SOURCES runtime_scene3d_tests.cpp
        DEFINITIONS KIN_TEST_MODELS="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/models"
                    KIN_TEST_SUMMIT="${PROJECT_SOURCE_DIR}/examples/summit/content")
    set_tests_properties(kin-runtime-scene3d PROPERTIES SKIP_RETURN_CODE 77)
    kin_add_test(kin_particle_assets3d_tests kin-particle-assets3d SOURCES particle_assets3d_tests.cpp
        DEFINITIONS KIN_TEST_MODELS="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/models"
                    KIN_TEST_SUMMIT="${PROJECT_SOURCE_DIR}/examples/summit/content")
    kin_add_test(kin_particle_shadow_tests kin-particle-shadows3d SOURCES particle_shadow_cast3d_tests.cpp)
    set_tests_properties(kin-particle-shadows3d PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 1800)
    add_test(NAME kin-particle-assets3d-cpu COMMAND kin_particle_assets3d_tests cpu)
    set_tests_properties(kin-particle-assets3d PROPERTIES SKIP_RETURN_CODE 77)
    kin_add_test(kin_physics3d_asset_tests kin-physics3d-assets SOURCES physics3d_asset_tests.cpp
        DEFINITIONS KIN_TEST_SUMMIT="${PROJECT_SOURCE_DIR}/examples/summit/content")
    kin_add_test(kin_material_assets3d_tests kin-material-assets3d SOURCES material_assets3d_tests.cpp
        DEFINITIONS KIN_TEST_MODELS="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/models"
                    KIN_TEST_SUMMIT="${PROJECT_SOURCE_DIR}/examples/summit/content")
    if(KIN_GLSLC)
        kin_compile_glsl(_kin_material_shader OUTPUT_DIR "${_kin_test_shader_dir}" SOURCES shaders/custom_flat.frag.glsl)
        add_custom_target(kin_material_test_shader DEPENDS ${_kin_material_shader})
        add_dependencies(kin_material_assets3d_tests kin_material_test_shader)
        target_compile_definitions(kin_material_assets3d_tests PRIVATE KIN_TEST_MATERIAL_SHADER="${_kin_test_shader_dir}/custom_flat.frag.spv")
    endif()
    set_tests_properties(kin-material-assets3d PROPERTIES SKIP_RETURN_CODE 77)
    kin_add_test(kin_scene_session3d_tests kin-scene-session3d SOURCES scene_session3d_tests.cpp
        DEFINITIONS KIN_TEST_MODELS="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/models"
                    KIN_TEST_SUMMIT="${PROJECT_SOURCE_DIR}/examples/summit/content")
    set_tests_properties(kin-scene-session3d PROPERTIES SKIP_RETURN_CODE 77)
    kin_add_test(kin_scene3d_asset_tests kin-scene3d-assets SOURCES scene3d_asset_tests.cpp
        DEFINITIONS KIN_TEST_MODELS="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/models"
                    KIN_TEST_SUMMIT="${PROJECT_SOURCE_DIR}/examples/summit/content")
    set_tests_properties(kin-scene3d-assets PROPERTIES SKIP_RETURN_CODE 77)
    kin_add_test(kin_model_assets3d_tests kin-model-assets3d SOURCES model_assets3d_tests.cpp
        DEFINITIONS KIN_TEST_MODELS="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/models")
    set_tests_properties(kin-model-assets3d PROPERTIES SKIP_RETURN_CODE 77)
    kin_add_test(kin_mesh_cluster_tests kin-mesh-clusters SOURCES mesh_cluster_tests.cpp)
    kin_add_test(kin_skeleton_tests kin-skeleton SOURCES skeleton_tests.cpp)
    kin_add_test(kin_physics3d_tests kin-physics3d SOURCES physics3d_tests.cpp)
    add_test(NAME kin-physics3d-first-hull COMMAND kin_physics3d_tests first-hull)
    add_test(NAME kin-physics3d-first-mesh COMMAND kin_physics3d_tests first-mesh)
    kin_add_test(kin_physics3d_ecs_tests kin-physics3d-ecs SOURCES physics3d_ecs_tests.cpp)
    kin_add_test(kin_physics3d_character_tests kin-physics3d-character SOURCES physics3d_character_tests.cpp
        DEFINITIONS KIN_TEST_REFERENCE_SCENES="${PROJECT_SOURCE_DIR}/tests/reference/scenes")
    kin_add_test(kin_physics3d_traversal_tests kin-physics3d-traversal SOURCES physics3d_traversal_tests.cpp
        DEFINITIONS KIN_TEST_REFERENCE_SCENES="${PROJECT_SOURCE_DIR}/tests/reference/scenes")
    kin_add_test(kin_physics3d_ragdoll_tests kin-physics3d-ragdoll SOURCES physics3d_ragdoll_tests.cpp)
    kin_add_test(kin_physics3d_rope_tests kin-physics3d-rope SOURCES physics3d_rope_tests.cpp)
    kin_add_test(kin_render3d_tests kin-render3d SOURCES render3d_tests.cpp
        DEFINITIONS KIN_TEST_MODELS="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/models"
                    KIN_TEST_MATERIAL_SHADERS="${CMAKE_CURRENT_SOURCE_DIR}/shaders")
    # Some 230 GPU tests: a few minutes on lavapipe here, longer on a CI runner, so
    # it has its own limit (a TIMEOUT property takes precedence over ctest --timeout).
    set_tests_properties(kin-render3d PROPERTIES TIMEOUT 1800)
endif()
if(KIN_EXPERIMENTAL_3D AND KIN_HAS_GPU_RENDERER)
    kin_add_test(kin_render3d_geometry_tests kin-render3d-geometry SOURCES render3d_geometry_tests.cpp)
    kin_compile_glsl(_geometry_spv OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/geometry/shaders"
        SOURCES shaders/geometry_probe.frag.glsl)
    kin_embed_files(kin_render3d_geometry_tests NAME geometry_probe FILES ${_geometry_spv})
    add_test(NAME kin-render3d-geometry-foreign COMMAND kin_render3d_geometry_tests --foreign-device)
    add_test(NAME kin-render3d-preparation-lifetime COMMAND kin_render3d_geometry_tests --lifetime)
    set_tests_properties(kin-render3d-geometry kin-render3d-geometry-foreign kin-render3d-preparation-lifetime
        PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180)
    kin_add_test(kin_render3d_bounds_tests kin-render3d-bounds SOURCES render3d_bounds_tests.cpp)
    set_tests_properties(kin-render3d-bounds PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180)
    kin_add_test(kin_render3d_fidelity_tests kin-render3d-fidelity SOURCES render3d_fidelity_tests.cpp
        DEFINITIONS KIN_TEST_MODELS="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/models")
    set_tests_properties(kin-render3d-fidelity PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180)
endif()
if(KIN_EXPERIMENTAL_3D AND NOT KIN_ENABLE_SCENE3D_RUNTIME)
    kin_add_test(kin_render3d_cpu_tests kin-render3d-cpu SOURCES render3d_cpu_tests.cpp)
    kin_add_test(kin_mesh_cluster_tests kin-mesh-clusters SOURCES mesh_cluster_tests.cpp)
    if(KIN_ENABLE_PHYSICS3D)
        kin_add_test(kin_skeleton_tests kin-skeleton SOURCES skeleton_tests.cpp)
    endif()
endif()
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
    # ui2 text on the CPU: labels measured and drawn, ASCII and beyond.
    add_executable(kin_text_bench text_bench.cpp)
    target_link_libraries(kin_text_bench PRIVATE kin::engine)
    # The 3D reference scenes (docs/3d_plan.md, G0): pictures and times.
    if(KIN_EXPERIMENTAL_3D)
        if(KIN_ENABLE_PHYSICS3D)
            add_executable(kin_scene_work_bench scene_work_bench.cpp)
            target_link_libraries(kin_scene_work_bench PRIVATE kin::engine)
            add_executable(kin_reference ${PROJECT_SOURCE_DIR}/tools/reference/kin_reference.cpp)
            target_link_libraries(kin_reference PRIVATE kin::engine)
            target_include_directories(kin_reference PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
            # kin/physics3d step times: piles and stacks of boxes, 1 to 8 workers (docs/physics_3d.md).
            add_executable(kin_physics3d_bench physics3d_bench.cpp)
            target_link_libraries(kin_physics3d_bench PRIVATE kin::engine)
        endif()
        # kin/render3d CPU costs that need no window: instance batch edits, particle wind (docs/rendering_3d.md).
        add_executable(kin_render3d_bench render3d_bench.cpp)
        target_link_libraries(kin_render3d_bench PRIVATE kin::engine)
    endif()
    # SDL_GPU quads: draw_texture, fill_rect and draw_sprites, many a frame.
    add_executable(kin_draw_bench draw_bench.cpp)
    target_link_libraries(kin_draw_bench PRIVATE kin::engine)
    target_compile_definitions(kin_draw_bench PRIVATE KIN_TEST_SHADER_DIR="${CMAKE_CURRENT_BINARY_DIR}/shaders"
        KIN_ENGINE_SHADER_SOURCE_DIR="${PROJECT_SOURCE_DIR}/engine/shaders")
    if(TARGET kin_test_shaders)
        add_dependencies(kin_draw_bench kin_test_shaders)
    endif()

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
kin_add_test(kin_owned_scene_session_tests kin-owned-scene-session SOURCES owned_scene_session_tests.cpp)

kin_add_component_test(RENDER_PROBE kin_render_probe_tests kin-render-probe SOURCES render_probe_tests.cpp)
kin_add_component_test(DETERMINISM_CHECK kin_determinism_tests kin-determinism SOURCES determinism_tests.cpp)

kin_add_component_test(SCENE_SERVER kin_server_tests kin-server SOURCES server_tests.cpp)

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

kin_add_component_test(SCRIPTING kin_scripting_tests kin-scripting
    SOURCES scripting_tests.cpp LINK_TARGET kin::scripting_scenes
    DEFINITIONS KIN_TEST_LUA_RELEASE_NUMERICS=$<NOT:$<CONFIG:Debug>>)


add_executable(kin_renderer_startup renderer_startup.cpp)
target_link_libraries(kin_renderer_startup PRIVATE kin::engine)
add_test(NAME kin-renderer-policy
    COMMAND "${CMAKE_COMMAND}" "-DEXECUTABLE=$<TARGET_FILE:kin_renderer_startup>"
        "-DRENDERER=${KIN_RENDERER}" -P "${CMAKE_CURRENT_SOURCE_DIR}/renderer_policy_tests.cmake")
