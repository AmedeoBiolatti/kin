# kin_pack: makes, lists, checks and unpacks .kinpak content packs
# (docs/shipping.md). kin_game's <game>_content targets run it.
add_executable(kin_pack kin_pack/main.cpp)
target_link_libraries(kin_pack PRIVATE kin::engine)
target_compile_options(kin_pack PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
)
kin_enable_release_optimizations(kin_pack)

if(KIN_EXPERIMENTAL_3D)
    # Standalone experiment: solver/history/shaders stay under tools/slowlight3d.
    add_executable(kin_slowlight3d slowlight3d/main.cpp)
    target_link_libraries(kin_slowlight3d PRIVATE kin::engine)
    kin_compile_glsl(_slowlight_spv OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/slowlight3d/shaders"
        SOURCES slowlight3d/transport.comp.glsl slowlight3d/receiver.frag.glsl slowlight3d/probe.frag.glsl)
    kin_embed_files(kin_slowlight3d NAME slowlight3d_shader FILES ${_slowlight_spv})
    if(BUILD_TESTING)
        add_executable(kin_slowlight3d_tests slowlight3d/simulation_tests.cpp)
        target_compile_features(kin_slowlight3d_tests PRIVATE cxx_std_23)
        add_test(NAME kin-slowlight3d-cpu COMMAND kin_slowlight3d_tests)
        if(KIN_HAS_GPU_RENDERER AND _slowlight_spv)
            add_test(NAME kin-slowlight3d-graph COMMAND kin_slowlight3d --test)
            set_tests_properties(kin-slowlight3d-graph PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180)
        endif()
    endif()
    add_executable(kin_outline3d outline3d/main.cpp)
    target_link_libraries(kin_outline3d PRIVATE kin::engine)
    kin_compile_glsl(_outline_spv OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/outline3d/shaders"
        SOURCES outline3d/outline.frag.glsl)
    kin_embed_files(kin_outline3d NAME outline3d_shader FILES ${_outline_spv})
    if(BUILD_TESTING AND KIN_HAS_GPU_RENDERER AND _outline_spv)
        add_test(NAME kin-outline3d-graph COMMAND kin_outline3d --test)
        set_tests_properties(kin-outline3d-graph PROPERTIES SKIP_RETURN_CODE 77)
    endif()
    add_executable(kin_surface3d surface3d/main.cpp)
    target_link_libraries(kin_surface3d PRIVATE kin::engine)
    kin_compile_glsl(_surface_spv OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/surface3d/shaders"
        SOURCES surface3d/toon.frag.glsl surface3d/field.comp.glsl)
    kin_embed_files(kin_surface3d NAME surface3d_shader FILES ${_surface_spv})
    if(BUILD_TESTING AND KIN_HAS_GPU_RENDERER)
        add_test(NAME kin-surface3d-graph COMMAND kin_surface3d --test)
        set_tests_properties(kin-surface3d-graph PROPERTIES SKIP_RETURN_CODE 77)
    endif()
    add_executable(kin_ascii3d ascii3d/main.cpp)
    target_link_libraries(kin_ascii3d PRIVATE kin::engine)
    if(BUILD_TESTING AND KIN_HAS_GPU_RENDERER)
        add_test(NAME kin-ascii3d-graph COMMAND kin_ascii3d --test)
        set_tests_properties(kin-ascii3d-graph PROPERTIES SKIP_RETURN_CODE 77)
    endif()
    add_executable(kin_asset_preview asset_preview/main.cpp)
    target_link_libraries(kin_asset_preview PRIVATE kin::engine)
    kin_compile_glsl(_preview_spv OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/asset_preview/shaders"
        SOURCES asset_preview/inspect.frag.glsl)
    kin_embed_files(kin_asset_preview NAME asset_preview_shader FILES ${_preview_spv})
    kin_enable_release_optimizations(kin_asset_preview)
    if(BUILD_TESTING AND KIN_ENABLE_AGENT_SERVER AND _preview_spv AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
        find_package(Python3 QUIET COMPONENTS Interpreter)
        if(Python3_Interpreter_FOUND)
            add_test(NAME kin-asset-preview
                COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/asset_preview/smoke.py"
                    --binary $<TARGET_FILE:kin_asset_preview>
                    --model "${CMAKE_CURRENT_SOURCE_DIR}/../examples/content/models/crate.glb"
                    --output "${CMAKE_CURRENT_BINARY_DIR}/asset-preview-smoke")
            set_tests_properties(kin-asset-preview PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77 RUN_SERIAL TRUE ENVIRONMENT "KIN_RENDER_BACKEND=gpu")
        endif()
    endif()
endif()
