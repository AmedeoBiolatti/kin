add_library(kin_engine STATIC
    src/anim/animation.cpp
    src/anim/anim_format.cpp
    src/anim/binding.cpp
    src/anim/events.cpp
    src/anim/player.cpp
    src/anim/preview.cpp
    src/anim/registry.cpp
    src/anim/state_machine.cpp
    src/anim/track.cpp
    src/anim/value.cpp
    src/audio/audio_catalog.cpp
    src/audio/audio_clip.cpp
    src/audio/audio_engine.cpp
    src/audio/sdl_audio_backend.cpp
    src/audio/spatial_audio.cpp
    src/assets/asset_manager.cpp
    src/assets/asset_server.cpp
    src/assets/file_watcher.cpp
    src/assets/image.cpp
    src/core/hex.cpp
    src/core/instrumentation.cpp
    src/core/jobs.cpp
    src/core/json.cpp
    src/core/json_value.cpp
    src/core/profile.cpp
    src/core/utf8.cpp
    src/dialogue/dialogue.cpp
    src/dialogue/dialogue_graph.cpp
    src/ecs/audio.cpp
    src/ecs/component.cpp
    src/ecs/component_schema.cpp
    src/ecs/editor.cpp
    src/ecs/entity.cpp
    src/ecs/event.cpp
    src/ecs/ecs_scene.cpp
    src/ecs/particles.cpp
    src/ecs/physics.cpp
    src/ecs/render.cpp
    src/ecs/system.cpp
    src/ecs/time.cpp
    src/ecs/ui2.cpp
    src/ecs/ui2_sync.cpp
    src/platform/app.cpp
    src/platform/input.cpp
    src/platform/file_dialogs.cpp
    src/platform/process.cpp
    src/platform/log.cpp
    src/platform/user_data.cpp
    src/platform/window.cpp
    src/particles/animation_particles.cpp
    src/particles/particle_catalog.cpp
    src/particles/particle_field.cpp
    src/particles/particle_system.cpp
    src/pathfinding/hex_pathfinding.cpp
    src/pathfinding/path_server.cpp
    src/pathfinding/pathfinding.cpp
    src/physics/debug_render.cpp
    src/physics/physics.cpp
    src/prefab/prefab_asset.cpp
    src/renderer/animation_import.cpp
    src/renderer/render_graph.cpp
    src/renderer/render_profile.cpp
    src/renderer/render_queue.cpp
    src/renderer/render_view.cpp
    src/renderer/lighting.cpp
    src/renderer/post_blur.cpp
    src/renderer/renderer2d.cpp
    src/renderer/static_texture_layer.cpp
    src/renderer/sprite_catalog.cpp
    src/renderer/sprite_sheet.cpp
    src/renderer/sdl_renderer2d_backend.cpp
    src/renderer/gpu/gpu_buffer.cpp
    src/renderer/gpu/gpu_device.cpp
    src/renderer/gpu/gpu_frame_timer.cpp
    src/renderer/gpu/gpu_geometry_batch.cpp
    src/renderer/gpu/gpu_pipeline_cache.cpp
    src/renderer/gpu/gpu_renderer2d_backend.cpp
    src/renderer/gpu/gpu_shader.cpp
    src/renderer/gpu/gpu_texture.cpp
    src/runtime/game_info.cpp
    src/runtime/debug_overlay.cpp
    src/runtime/scene_app.cpp
    src/runtime/scene_server.cpp
    src/runtime/windowed_app.cpp
    src/scene/scene_asset.cpp
    src/scene/scene_asset_runtime.cpp
    src/scene/scene_manager.cpp
    src/scene/transitions.cpp
    src/save/save_store.cpp
    src/scripting/script_engine.cpp
    src/scripting/lua_paths.cpp
    src/scripting/lua_script.cpp
    src/scripting/script_scene.cpp
    src/tilemap/tilemap.cpp
    src/ui2/geometry.cpp
    src/ui2/layout.cpp
    src/ui2/menu_scene.cpp
    src/ui2/state.cpp
    src/ui2/context.cpp
    src/ui2/skin_loader.cpp
    src/ui2/style_internal.cpp
    src/ui2/text.cpp
    src/ui2/theme.cpp
    src/ui2/theme_file.cpp
    src/ui2/widget_internal.cpp
    src/ui2/widgets_core.cpp
    src/ui2/widgets_inputs.cpp
    src/ui2/widgets_menus.cpp
    src/ui2/widgets_collections.cpp
    src/ui2/widgets_editor.cpp
    src/ui2/widgets_text_edit.cpp
)
add_library(kin::engine ALIAS kin_engine)

kin_enable_release_optimizations(kin_engine)

target_compile_features(kin_engine PUBLIC cxx_std_23)
if(MSVC)
    set_source_files_properties(src/scripting/script_engine.cpp PROPERTIES COMPILE_OPTIONS "/bigobj")
    set_source_files_properties(src/ecs/ui2.cpp PROPERTIES COMPILE_OPTIONS "/bigobj")
endif()
target_compile_definitions(kin_engine PUBLIC
    "$<$<OR:$<BOOL:${KIN_ENABLE_PROFILING}>,$<AND:$<CONFIG:Release>,$<BOOL:${KIN_ENABLE_RELEASE_PROFILING}>>>:KIN_ENABLE_PROFILING>"
)
target_include_directories(kin_engine PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
if(KIN_ENABLE_RENDER_PROBE)
    target_sources(kin_engine PRIVATE src/runtime/render_probe.cpp)
    target_compile_definitions(kin_engine PUBLIC KIN_ENABLE_RENDER_PROBE)
endif()

add_library(kin_core_allocation_instrumentation OBJECT
    src/core/allocation_instrumentation.cpp
)
add_library(kin::core_allocation_instrumentation ALIAS kin_core_allocation_instrumentation)

target_compile_features(kin_core_allocation_instrumentation PUBLIC cxx_std_23)
target_include_directories(kin_core_allocation_instrumentation PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(kin_engine PUBLIC
    flecs::flecs_static
    box2d
    SDL3::SDL3-static
    SDL3_image::SDL3_image
    SDL3_ttf::SDL3_ttf
    Lua::Lua
    sol2::sol2
)

# SDL_GPU backend shaders: GLSL -> SPIR-V via glslc (Vulkan SDK). The compiled
# .spv land in a build dir referenced at runtime via KIN_GPU_SHADER_DIR. Without
# glslc the engine still builds; the GPU backend just won't find its shaders.
find_program(KIN_GLSLC NAMES glslc HINTS "$ENV{VULKAN_SDK}/bin" "$ENV{VULKAN_SDK}/Bin")
set(_kin_gpu_shader_dir "${CMAKE_CURRENT_BINARY_DIR}/shaders")
target_compile_definitions(kin_engine PRIVATE KIN_GPU_SHADER_DIR="${_kin_gpu_shader_dir}")
if(KIN_GLSLC)
    file(MAKE_DIRECTORY "${_kin_gpu_shader_dir}")
    set(_kin_gpu_spv)
    foreach(_pair "textured_quad.vert.glsl:vertex" "textured_quad.frag.glsl:fragment"
                  "sprite_instanced.vert.glsl:vertex"
                  "pp_vignette.frag.glsl:fragment" "pp_color_grade.frag.glsl:fragment"
                  "pp_scanline.frag.glsl:fragment" "pp_chroma.frag.glsl:fragment"
                  "pp_bloom_bright.frag.glsl:fragment" "pp_bloom_blur.frag.glsl:fragment"
                  "pp_bloom_combine.frag.glsl:fragment"
                  "transition_dissolve.frag.glsl:fragment" "transition_pixelate.frag.glsl:fragment"
                  "transition_wipe.frag.glsl:fragment" "transition_iris.frag.glsl:fragment"
                  "transition_crossfade.frag.glsl:fragment")
        string(REPLACE ":" ";" _parts "${_pair}")
        list(GET _parts 0 _src_name)
        list(GET _parts 1 _stage)
        string(REPLACE ".glsl" ".spv" _out_name "${_src_name}")
        add_custom_command(
            OUTPUT "${_kin_gpu_shader_dir}/${_out_name}"
            COMMAND "${KIN_GLSLC}" -fshader-stage=${_stage}
                    -o "${_kin_gpu_shader_dir}/${_out_name}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${_src_name}"
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${_src_name}"
            COMMENT "glslc: ${_src_name} -> ${_out_name}"
            VERBATIM)
        list(APPEND _kin_gpu_spv "${_kin_gpu_shader_dir}/${_out_name}")
    endforeach()
    add_custom_target(kin_engine_gpu_shaders DEPENDS ${_kin_gpu_spv})
    add_dependencies(kin_engine kin_engine_gpu_shaders)
else()
    message(STATUS "kin_engine: glslc not found; SDL_GPU backend shaders not compiled")
endif()
