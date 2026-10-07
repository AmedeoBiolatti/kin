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
    src/audio/audio_analysis.cpp
    src/audio/audio_catalog.cpp
    src/audio/audio_clip.cpp
    src/audio/audio_decoder.cpp
    src/audio/audio_effects.cpp
    src/audio/audio_engine.cpp
    src/audio/audio_settings.cpp
    src/audio/sdl_audio_backend.cpp
    src/audio/spatial_audio.cpp
    src/assets/asset_manager.cpp
    src/assets/content.cpp
    src/assets/asset_server.cpp
    src/assets/file_watcher.cpp
    src/assets/image.cpp
    src/core/bidi.cpp
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
    src/l10n/gettext.cpp
    src/l10n/language_file.cpp
    src/l10n/localization.cpp
    src/l10n/message_format.cpp
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
    src/renderer/cached_target.cpp
    src/renderer/render_graph.cpp
    src/renderer/render_profile.cpp
    src/renderer/render_queue.cpp
    src/renderer/render_view.cpp
    src/renderer/lighting.cpp
    src/renderer/post_blur.cpp
    src/renderer/path.cpp
    src/renderer/shape.cpp
    src/renderer/svg.cpp
    src/renderer/renderer2d.cpp
    src/renderer/renderer2d_shapes.cpp
    src/renderer/clip.cpp
    src/renderer/color.cpp
    src/renderer/color_grading.cpp
    src/renderer/static_texture_layer.cpp
    src/renderer/sprite_catalog.cpp
    src/renderer/sprite_sheet.cpp
    src/renderer/sdl_renderer2d_backend.cpp
    src/renderer/rounded_rect_loop.cpp
    src/renderer/shader_compiler.cpp
    src/renderer/shader_reflect.cpp
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
    src/scripting/lua_audio.cpp
    src/scripting/lua_l10n.cpp
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
    src/ui2/text_script.cpp
    src/ui2/text_ttf.cpp
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
    target_sources(kin_engine PRIVATE src/renderer/draw_trace.cpp src/runtime/render_probe.cpp)
    target_compile_definitions(kin_engine PUBLIC KIN_ENABLE_RENDER_PROBE)
endif()
if(KIN_ENABLE_AGENT_SERVER)
    target_compile_definitions(kin_engine PUBLIC KIN_ENABLE_AGENT_SERVER)
endif()
if(KIN_SHIPPING)
    target_compile_definitions(kin_engine PUBLIC KIN_SHIPPING)
endif()
if(KIN_ENABLE_DETERMINISM_CHECK)
    target_sources(kin_engine PRIVATE src/runtime/determinism_check.cpp src/runtime/state_hash.cpp)
    target_compile_definitions(kin_engine PUBLIC KIN_ENABLE_DETERMINISM_CHECK)
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
target_link_libraries(kin_engine PRIVATE earcut_hpp::earcut_hpp) # used only inside the engine

# Third-party audio decoders, built as C without kin's warnings.
add_library(kin_audio_codecs STATIC src/audio/codecs.c)
target_include_directories(kin_audio_codecs SYSTEM PUBLIC "${dr_libs_SOURCE_DIR}" "${stb_SOURCE_DIR}")
target_compile_options(kin_audio_codecs PRIVATE "$<$<C_COMPILER_ID:GNU,Clang>:-w>" "$<$<C_COMPILER_ID:MSVC>:/w>")
target_link_libraries(kin_engine PRIVATE kin_audio_codecs)

# SDL_GPU backend shaders: GLSL -> SPIR-V via glslc (Vulkan SDK), built into
# the engine (kin_embed_files), so a game needs no shader files beside it.
# Without glslc the engine still builds; the GPU backend then has no shaders and
# kin falls back to SDL_Renderer. Shipping builds require them (KIN_REQUIRE_GPU_SHADERS).
set(_kin_gpu_shader_sources)
foreach(_name textured_quad.vert textured_quad.frag sprite_instanced.vert shader_geometry.vert
              overdraw_count.frag overdraw_heat.frag shape.frag shape.vert sdf_shape.frag sdf_shape.vert
              distance_field.frag mask_composite.frag mask_stencil.frag color_output.frag
              pp_vignette.frag pp_color_grade.frag pp_scanline.frag pp_chroma.frag
              pp_bloom_bright.frag pp_bloom_blur.frag pp_bloom_combine.frag
              transition_dissolve.frag transition_pixelate.frag transition_wipe.frag
              transition_iris.frag transition_crossfade.frag)
    list(APPEND _kin_gpu_shader_sources "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${_name}.glsl")
endforeach()
# The .spv files stay in the build too, for tests that read every engine shader.
kin_compile_glsl(_kin_gpu_spv OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders" SOURCES ${_kin_gpu_shader_sources})
kin_embed_files(kin_engine NAME embedded_gpu_shader NAMESPACE kin::gpu FILES ${_kin_gpu_spv})
if(KIN_GLSLC AND NOT KIN_SHIPPING)
    # For runtime compiles of a game's shaders (kin/renderer/shader_compiler.hpp) on this machine.
    target_compile_definitions(kin_engine PRIVATE KIN_GLSLC_PATH="${KIN_GLSLC}")
endif()
list(LENGTH _kin_gpu_shader_sources _kin_gpu_wanted)
list(LENGTH _kin_gpu_spv _kin_gpu_built)
if(NOT _kin_gpu_built EQUAL _kin_gpu_wanted)
    if(KIN_REQUIRE_GPU_SHADERS)
        message(FATAL_ERROR "The GPU backend has ${_kin_gpu_built} of its ${_kin_gpu_wanted} shaders: install "
                            "the Vulkan SDK (or your distribution's glslc), or pass KIN_SPIRV_DIR made by "
                            "cmake/kin_spirv.cmake; or set KIN_REQUIRE_GPU_SHADERS=OFF to ship without them")
    endif()
    message(STATUS "kin_engine: ${_kin_gpu_built} of ${_kin_gpu_wanted} SDL_GPU backend shaders (glslc not found)")
endif()
