# Shared support has no dependency on optional components. In particular, the
# default asset-loader registration in kin_engine may depend on audio, while
# audio's logging/localization only depend on this lower layer.
add_library(kin_foundation STATIC
    src/assets/file_watcher.cpp
    src/assets/content.cpp
    src/assets/asset_types.cpp
    src/core/json.cpp
    src/core/hash.cpp
    src/core/jobs.cpp
    src/core/json_value.cpp
    src/core/script_debugger.cpp
    src/core/utf8.cpp
    src/l10n/gettext.cpp
    src/l10n/language_file.cpp
    src/l10n/localization.cpp
    src/l10n/message_format.cpp
    src/platform/log.cpp
)
target_compile_features(kin_foundation PUBLIC cxx_std_23)
target_include_directories(kin_foundation PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(kin_foundation PUBLIC kin::components SDL3::SDL3-static)
target_compile_definitions(kin_foundation PUBLIC KIN_ENABLE_THREADS=$<BOOL:${KIN_ENABLE_THREADS}>)
if(KIN_ENABLE_THREADS)
    find_package(Threads REQUIRED)
    target_link_libraries(kin_foundation PUBLIC Threads::Threads)
endif()
kin_enable_release_optimizations(kin_foundation)

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
    src/assets/asset_manager.cpp
    src/assets/asset_server.cpp
    src/assets/bake_cache.cpp
    src/assets/image.cpp
    src/assets/texture_file.cpp
    src/core/bidi.cpp
    src/core/hex.cpp
    src/core/instrumentation.cpp
    src/core/profile.cpp
    src/dialogue/dialogue.cpp
    src/dialogue/dialogue_graph.cpp
    src/ecs/assets2d.cpp
    src/ecs/component.cpp
    src/ecs/component_schema.cpp
    src/ecs/editor.cpp
    src/ecs/entity.cpp
    src/ecs/event.cpp
    src/ecs/ecs_scene.cpp
    src/ecs/particles.cpp
    src/ecs/render.cpp
    src/ecs/animator.cpp
    src/ecs/camera.cpp
    src/ecs/lighting.cpp
    src/ecs/shader_effect.cpp
    src/ecs/ui_screens.cpp
    src/ecs/dialogue.cpp
    src/ecs/scene_flow.cpp
    src/ecs/sprite_animator.cpp
    src/ecs/text.cpp
    src/ecs/system.cpp
    src/ecs/time.cpp
    src/ecs/ui2.cpp
    src/ecs/ui2_sync.cpp
    src/platform/app.cpp
    src/platform/input.cpp
    src/platform/file_dialogs.cpp
    src/platform/gamepad.cpp
    src/platform/resource_paths.cpp
    src/platform/process.cpp
    src/platform/user_data.cpp
    src/platform/window.cpp
    src/particles/animation_particles.cpp
    src/particles/particle_catalog.cpp
    src/particles/particle_field.cpp
    src/particles/particle_system.cpp
    src/pathfinding/hex_pathfinding.cpp
    src/pathfinding/navmesh3d.cpp
    src/pathfinding/path_server.cpp
    src/pathfinding/pathfinding.cpp
    src/prefab/prefab_asset.cpp
    src/prefab/prefab_structure.cpp
    src/renderer/animation_import.cpp
    src/renderer/cached_target.cpp
    src/renderer/render_graph.cpp
    src/renderer/resource_graph.cpp
    src/renderer/graph_resources.cpp
    src/renderer/render_profile.cpp
    src/renderer/material.cpp
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
    src/renderer/rounded_rect_loop.cpp
    src/runtime/game_info.cpp
    src/runtime/debug_overlay.cpp
    src/runtime/scene_app.cpp
    src/runtime/windowed_app.cpp
    src/scene/scene_asset.cpp
    src/scene/scene_asset_runtime.cpp
    src/scene/scene_manager.cpp
    src/scene/transitions.cpp
    src/save/save_store.cpp
    src/tilemap/tilemap.cpp
    src/ui2/geometry.cpp
    src/ui2/layout.cpp
    src/ui2/menu_scene.cpp
    src/ui2/state.cpp
    src/ui2/context.cpp
    src/ui2/compose.cpp
    src/ui2/composed.cpp
    src/ui2/value_field.cpp
    src/ui2/drag_value.cpp
    src/ui2/scale.cpp
    src/ui2/charts.cpp
    src/ui2/skin_loader.cpp
    src/ui2/style_internal.cpp
    src/ui2/text_baked.cpp
    src/ui2/text.cpp
    src/ui2/text_script.cpp
    src/ui2/theme.cpp
    src/ui2/theme_file.cpp
    src/ui2/screen.cpp
    src/ui2/widget_internal.cpp
    src/ui2/widgets_core.cpp
    src/ui2/widgets_inputs.cpp
    src/ui2/widgets_menus.cpp
    src/ui2/widgets_collections.cpp
    src/ui2/widgets_editor.cpp
    src/ui2/widgets_text_edit.cpp
)
# Keep the existing raw archive target. The public umbrella composes components
# without static-library cycles: audio depends only on shared foundation code.
add_library(kin_engine_components INTERFACE)
add_library(kin::engine ALIAS kin_engine_components)
target_link_libraries(kin_engine_components INTERFACE kin_engine)
target_link_libraries(kin_engine PUBLIC kin_foundation)

target_sources(kin_engine PRIVATE src/renderer/shader_params.cpp)
if(KIN_HAS_SDL_RENDERER)
    target_sources(kin_engine PRIVATE src/renderer/sdl_renderer2d_backend.cpp)
endif()
if(KIN_HAS_WEBGL2_RENDERER)
    set(_kin_webgl_shaders
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/webgl2/sdf2d.vert.glsl"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/webgl2/sdf2d.frag.glsl")
    target_sources(kin_engine PRIVATE
        src/renderer/webgl2_renderer2d_backend.cpp
        src/renderer/webgl2_device.cpp
        src/runtime/browser_app.cpp)
    target_compile_options(kin_foundation PUBLIC -fexceptions)
    target_link_options(kin_engine INTERFACE -fexceptions -sMIN_WEBGL_VERSION=2 -sMAX_WEBGL_VERSION=2
        -sALLOW_MEMORY_GROWTH=1 -sENVIRONMENT=web -sEXIT_RUNTIME=0)
endif()
if(KIN_HAS_COMPACT_RENDERER)
    target_sources(kin_engine PRIVATE src/renderer/compact_renderer2d_backend.cpp
        src/renderer/sdl_framebuffer_host.cpp)
endif()
if(KIN_HAS_GPU_RENDERER)
    target_sources(kin_engine PRIVATE
        src/renderer/shader_compiler.cpp
        src/renderer/shader_reflect.cpp
        src/renderer/gpu/gpu_buffer.cpp
        src/renderer/gpu/gpu_device.cpp
        src/renderer/gpu/gpu_memory.cpp
        src/renderer/gpu/gpu_frame_timer.cpp
        src/renderer/gpu/gpu_geometry_batch.cpp
        src/renderer/gpu/gpu_pipeline_cache.cpp
        src/renderer/gpu/gpu_renderer2d_backend.cpp
        src/renderer/gpu/gpu_shader.cpp
        src/renderer/gpu/gpu_texture.cpp
    )
else()
    target_sources(kin_engine PRIVATE src/renderer/shader_disabled.cpp)
endif()
target_compile_definitions(kin_engine PUBLIC
    KIN_ENABLE_PHYSICS3D=$<BOOL:${KIN_ENABLE_PHYSICS3D}>
    KIN_ENABLE_SCENE3D_RUNTIME=$<BOOL:${KIN_ENABLE_SCENE3D_RUNTIME}>
    KIN_HAS_GPU_RENDERER=$<BOOL:${KIN_HAS_GPU_RENDERER}>
    KIN_HAS_WEBGL2_RENDERER=$<BOOL:${KIN_HAS_WEBGL2_RENDERER}>
    KIN_HAS_COMPACT_RENDERER=$<BOOL:${KIN_HAS_COMPACT_RENDERER}>
    KIN_HAS_SDL_RENDERER=$<BOOL:${KIN_HAS_SDL_RENDERER}>)

if(KIN_ENABLE_AUDIO)
    add_library(kin_audio STATIC
        src/audio/audio_analysis.cpp
        src/audio/audio_catalog.cpp
        src/audio/audio_clip.cpp
        src/audio/audio_decoder.cpp
        src/audio/audio_effects.cpp
        src/audio/audio_engine.cpp
        src/audio/audio_settings.cpp
        src/audio/sdl_audio_backend.cpp
        src/audio/spatial_audio.cpp
)
    add_library(kin::audio ALIAS kin_audio)
    target_link_libraries(kin_audio PUBLIC kin_foundation)
    target_link_libraries(kin_engine PUBLIC kin_audio)
    kin_enable_release_optimizations(kin_audio)
    add_library(kin_audio_ecs STATIC src/ecs/audio.cpp)
    add_library(kin::audio_ecs ALIAS kin_audio_ecs)
    target_link_libraries(kin_audio_ecs PUBLIC kin_audio kin_engine)
    kin_enable_release_optimizations(kin_audio_ecs)
    target_link_libraries(kin_engine_components INTERFACE kin_audio_ecs)
endif()
if(KIN_ENABLE_PHYSICS)
    add_library(kin_physics STATIC src/physics/physics.cpp)
    add_library(kin::physics ALIAS kin_physics)
    target_compile_features(kin_physics PUBLIC cxx_std_23)
    target_include_directories(kin_physics PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_link_libraries(kin_physics PUBLIC kin::components box2d)
    target_link_libraries(kin_physics PRIVATE earcut_hpp::earcut_hpp) # concave polygons, in triangles
    kin_enable_release_optimizations(kin_physics)

    add_library(kin_physics_render STATIC src/physics/debug_render.cpp)
    add_library(kin::physics_render ALIAS kin_physics_render)
    target_link_libraries(kin_physics_render PUBLIC kin_physics kin_engine)
    kin_enable_release_optimizations(kin_physics_render)

    add_library(kin_physics_ecs STATIC src/ecs/physics.cpp)
    add_library(kin::physics_ecs ALIAS kin_physics_ecs)
    target_link_libraries(kin_physics_ecs PUBLIC kin_physics kin_engine)
    kin_enable_release_optimizations(kin_physics_ecs)
    target_link_libraries(kin_engine_components INTERFACE kin_physics_render kin_physics_ecs)
endif()
if(KIN_ENABLE_SCRIPTING)
    add_library(kin_scripting STATIC
        src/scripting/lua_l10n.cpp
        src/scripting/lua_paths.cpp
        src/scripting/lua_script.cpp)
    add_library(kin::scripting ALIAS kin_scripting)
    target_link_libraries(kin_scripting PUBLIC kin_foundation Lua::Lua sol2::sol2)
    kin_enable_release_optimizations(kin_scripting)

    add_library(kin_scripting_scenes STATIC
        src/scripting/script_engine.cpp
        src/scripting/script_scene.cpp
        src/scripting/script_runtime.cpp
        src/scripting/entity_scripts.cpp)
    if(KIN_ENABLE_AUDIO)
        target_sources(kin_scripting_scenes PRIVATE src/scripting/lua_audio.cpp)
    endif()
    add_library(kin::scripting_scenes ALIAS kin_scripting_scenes)
    target_link_libraries(kin_scripting_scenes PUBLIC kin_scripting kin_engine)
    kin_enable_release_optimizations(kin_scripting_scenes)
    target_link_libraries(kin_engine_components INTERFACE kin_scripting_scenes)
endif()
if(KIN_ENABLE_SCENE_SERVER)
    target_sources(kin_engine PRIVATE
        src/runtime/scene_server.cpp
    )
endif()

# These usage requirements also reach game sources and their final link.
if(KIN_ENABLE_SIZE_OPTIMIZATIONS)
    target_compile_options(kin_engine PUBLIC
        "$<$<AND:$<NOT:$<CONFIG:Debug>>,$<CXX_COMPILER_ID:GNU,Clang>>:-ffunction-sections;-fdata-sections>"
        "$<$<AND:$<NOT:$<CONFIG:Debug>>,$<CXX_COMPILER_ID:MSVC>>:/Gy;/Gw>")
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_link_options(kin_engine INTERFACE "$<$<NOT:$<CONFIG:Debug>>:LINKER:--gc-sections>")
    elseif(MSVC)
        target_link_options(kin_engine INTERFACE "$<$<NOT:$<CONFIG:Debug>>:/OPT:REF;/OPT:ICF>")
    endif()
endif()

kin_enable_release_optimizations(kin_engine)

target_compile_features(kin_engine PUBLIC cxx_std_23)
if(MSVC)
    set_source_files_properties(src/scripting/script_engine.cpp PROPERTIES COMPILE_OPTIONS "/bigobj")
    set_source_files_properties(src/ecs/ui2.cpp PROPERTIES COMPILE_OPTIONS "/bigobj")
    # Every built-in component's registration, and the physics systems' queries.
    set_source_files_properties(src/scene/scene_asset.cpp src/ecs/physics.cpp PROPERTIES COMPILE_OPTIONS "/bigobj")
endif()
target_compile_definitions(kin_engine PUBLIC
    "$<$<OR:$<BOOL:${KIN_ENABLE_PROFILING}>,$<AND:$<CONFIG:Release>,$<BOOL:${KIN_ENABLE_RELEASE_PROFILING}>>>:KIN_ENABLE_PROFILING>"
)
target_include_directories(kin_engine PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
if(KIN_ENABLE_RENDER_PROBE)
    target_sources(kin_engine PRIVATE src/renderer/draw_trace.cpp src/runtime/render_probe.cpp)
endif()
if(KIN_ENABLE_AGENT_SERVER)
    target_compile_definitions(kin_engine PUBLIC KIN_ENABLE_AGENT_SERVER)
endif()
if(KIN_EXPERIMENTAL_3D)
    # glTF 2.0 models (kin/render3d/model3d.hpp). A single C header, MIT.
    FetchContent_Declare(cgltf
        URL https://github.com/jkuhlmann/cgltf/archive/refs/tags/v1.14.tar.gz
        URL_HASH SHA256=2f3c97a6b989943f50e7d7f228688f6558fe37b1411c13a350e3560d061707d8
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_GetProperties(cgltf)
    if(NOT cgltf_POPULATED)
        FetchContent_Populate(cgltf)
    endif()
    target_include_directories(kin_engine SYSTEM PRIVATE "${cgltf_SOURCE_DIR}")
    set(KIN_THIRD_PARTY_LICENSES "${KIN_THIRD_PARTY_LICENSES};cgltf.txt|${cgltf_SOURCE_DIR}/LICENSE" CACHE INTERNAL "")
    # Levels of detail (kin/render3d/mesh_lod3d.hpp) and clusters (mesh_clusters3d.hpp): the simplifier, the vertex-cache
    # optimiser, the meshlet builder and the cluster partitioner of meshoptimizer (MIT), compiled into the engine;
    # its other sources (codecs, rasteriser, stripifier) are not used.
    FetchContent_Declare(meshoptimizer
        URL https://github.com/zeux/meshoptimizer/archive/refs/tags/v0.25.tar.gz
        URL_HASH SHA256=68b2fef4e4eaad98e00c657c1e7f8982a7176e61dd7efdeaec67a025b8519be9
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_GetProperties(meshoptimizer)
    if(NOT meshoptimizer_POPULATED)
        FetchContent_Populate(meshoptimizer)
    endif()
    set(_kin_meshopt_sources allocator clusterizer indexgenerator partition simplifier vcacheoptimizer)
    list(TRANSFORM _kin_meshopt_sources PREPEND "${meshoptimizer_SOURCE_DIR}/src/")
    list(TRANSFORM _kin_meshopt_sources APPEND ".cpp")
    target_sources(kin_engine PRIVATE ${_kin_meshopt_sources})
    set_source_files_properties(${_kin_meshopt_sources} PROPERTIES COMPILE_OPTIONS
        "$<$<CXX_COMPILER_ID:GNU,Clang>:-w>;$<$<CXX_COMPILER_ID:MSVC>:/w>")
    target_include_directories(kin_engine SYSTEM PRIVATE "${meshoptimizer_SOURCE_DIR}/src")
    set(KIN_THIRD_PARTY_LICENSES "${KIN_THIRD_PARTY_LICENSES};meshoptimizer.txt|${meshoptimizer_SOURCE_DIR}/LICENSE.md" CACHE INTERNAL "")
    target_sources(kin_engine PRIVATE src/render3d/model.cpp src/render3d/assets3d.cpp src/render3d/material_asset3d.cpp src/render3d/model_batch3d.cpp src/render3d/mesh_lod.cpp src/render3d/mesh_clusters.cpp
                                      src/render3d/skeleton.cpp src/render3d/animation3d.cpp src/render3d/ik.cpp)
    target_sources(kin_engine PRIVATE
        src/render3d/camera3d.cpp src/render3d/mesh.cpp src/render3d/renderer3d.cpp
        src/render3d/scene3d.cpp src/render3d/shadow_cascades.cpp src/render3d/local_lights.cpp
        src/render3d/taa.cpp src/render3d/sky3d.cpp src/render3d/particles3d.cpp
        src/render3d/environment.cpp src/render3d/hdr_decode.c)
    if(KIN_HAS_GPU_RENDERER)
        target_sources(kin_engine PRIVATE src/renderer/gpu/gpu_renderer3d.cpp src/renderer/gpu/gpu_gi3d.cpp
                                          src/renderer/gpu/gpu_mesh_pool.cpp)
    elseif(KIN_HAS_WEBGL2_RENDERER)
        target_sources(kin_engine PRIVATE src/renderer/webgl2_renderer3d.cpp)
        list(APPEND _kin_webgl_shaders
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/webgl2/mesh.vert.glsl"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/webgl2/mesh.frag.glsl"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/webgl2/sky.vert.glsl"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/webgl2/sky.frag.glsl")
    endif()
    if(KIN_ENABLE_PHYSICS3D)
        target_sources(kin_engine PRIVATE
            src/ecs/render3d.cpp src/ecs/joint_attachment3d.cpp src/ecs/scene3d.cpp
            src/ecs/scene3d_settings.cpp src/ecs/animator3d_asset.cpp src/ecs/particles3d_asset.cpp
            src/scene/scene_loader3d.cpp src/scene/scene_entity3d.cpp src/scene/camera_rig3d.cpp
            src/scene/interaction3d.cpp src/scene/joint_attachment3d.cpp
            src/ecs/physics3d.cpp src/ecs/physics3d_asset.cpp src/physics3d/assets3d.cpp)
    endif()
    option(KIN_RENDERER3D_O3 "Use -O3 for the GPU 3D renderer in Release (GCC/Clang only)" OFF)
    # Source options follow the target's -O2. Keep this experiment to one
    # translation unit; other engine sources and floating-point flags stay put.
    set_property(SOURCE src/renderer/gpu/gpu_renderer3d.cpp APPEND PROPERTY COMPILE_OPTIONS
        "$<$<AND:$<BOOL:${KIN_RENDERER3D_O3}>,$<CONFIG:Release>,$<CXX_COMPILER_ID:GNU,Clang>>:-O3>")
    set_source_files_properties(src/render3d/hdr_decode.c PROPERTIES COMPILE_OPTIONS
        "$<$<C_COMPILER_ID:GNU,Clang>:-w>;$<$<C_COMPILER_ID:MSVC>:/w>")
    target_include_directories(kin_engine SYSTEM PRIVATE "${stb_SOURCE_DIR}")
    target_compile_definitions(kin_engine PUBLIC KIN_EXPERIMENTAL_3D)
    if(KIN_ENABLE_SCENE3D_RUNTIME)
        # Scene/frame orchestration sits above the engine and its adapters. The
        # base renderer must not depend back on scripting or audio ECS archives.
        add_library(kin_scene3d_runtime STATIC
            src/scene/scene_session3d.cpp
            src/scene/scene_frame3d.cpp
            src/scripting/lua_scene3d.cpp
            src/scripting/lua_scene3d_physics.cpp)
        add_library(kin::scene3d_runtime ALIAS kin_scene3d_runtime)
        target_link_libraries(kin_scene3d_runtime PUBLIC kin_engine kin_scripting_scenes)
        if(KIN_ENABLE_AUDIO)
            target_link_libraries(kin_scene3d_runtime PUBLIC kin_audio_ecs)
        endif()
        kin_enable_release_optimizations(kin_scene3d_runtime)
        target_link_libraries(kin_engine_components INTERFACE kin_scene3d_runtime)
    endif()
    # Sources that compute what the simulation starts from or the particles' randomness (body poses and kinematic targets,
    # the scene session, the sparks of a seed) get the physics glue's -ffp-contract=off below: a fused multiply-add rounds
    # once where two operations round twice, so a target with FMA (aarch64, or x86 with -mfma) would give other bits.
    foreach(_kin_fp_source src/ecs/physics3d.cpp src/scene/scene_session3d.cpp src/scene/scene_frame3d.cpp src/render3d/particles3d.cpp)
        set_property(SOURCE ${_kin_fp_source} APPEND PROPERTY COMPILE_OPTIONS
            "$<$<CXX_COMPILER_ID:GNU,Clang>:-ffp-contract=off>")
    endforeach()
    if(KIN_ENABLE_PHYSICS3D)
        # kin/physics3d: Jolt Physics behind kin's own types (docs/physics_3d.md). Its sources are compiled apart
        # from the engine's, as an object library: Jolt's PUBLIC compile options (-mavx2, -mpopcnt, ...) and
        # definitions would otherwise reach every engine source and change what the rest of kin compiles to. Only
        # these files see Jolt's headers; no kin header includes them. -ffp-contract=off keeps the compiler from
        # fusing multiply-adds in the glue (determinism); -fno-rtti is Jolt's own build setting, which its classes
        # that these sources derive from need.
        add_library(kin_physics3d_objects OBJECT
            src/physics3d/shape3d.cpp
            src/physics3d/world3d.cpp
            src/physics3d/character3d.cpp
            src/physics3d/traversal3d.cpp
            src/physics3d/traversal_character3d.cpp
            src/physics3d/ragdoll3d.cpp
            src/physics3d/rope3d.cpp
            src/physics3d/debug_draw3d.cpp
        )
        target_compile_features(kin_physics3d_objects PUBLIC cxx_std_23)
        target_include_directories(kin_physics3d_objects PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
        target_link_libraries(kin_physics3d_objects PRIVATE Jolt)
        target_compile_options(kin_physics3d_objects PRIVATE
            "$<$<CXX_COMPILER_ID:GNU,Clang>:-fno-rtti;-ffp-contract=off>")
        kin_enable_release_optimizations(kin_physics3d_objects)
        target_sources(kin_engine PRIVATE $<TARGET_OBJECTS:kin_physics3d_objects>)
        target_link_libraries(kin_engine PRIVATE $<LINK_ONLY:Jolt>)
    endif()
endif()
if(KIN_SHIPPING)
    target_compile_definitions(kin_engine PUBLIC KIN_SHIPPING)
endif()
if(KIN_ENABLE_DETERMINISM_CHECK)
    target_sources(kin_engine PRIVATE src/runtime/determinism_check.cpp src/runtime/state_hash.cpp)
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
    SDL3::SDL3-static
)
# 3D Lua argument checks are local to its bindings; retain sol2 defaults for 2D games.

if(KIN_ENABLE_TTF)
    target_sources(kin_engine PRIVATE src/ui2/text_ttf.cpp)
    target_link_libraries(kin_engine PUBLIC SDL3_ttf::SDL3_ttf)
endif()
if(KIN_ENABLE_IMAGE_IO)
    target_link_libraries(kin_engine PUBLIC SDL3_image::SDL3_image)
endif()

target_link_libraries(kin_engine PRIVATE earcut_hpp::earcut_hpp) # used only inside the engine

if(KIN_ENABLE_AUDIO)
    # Third-party audio decoders, built as C without kin's warnings.
    add_library(kin_audio_codecs STATIC src/audio/codecs.c)
    target_include_directories(kin_audio_codecs SYSTEM PUBLIC "${dr_libs_SOURCE_DIR}" "${stb_SOURCE_DIR}")
    target_compile_options(kin_audio_codecs PRIVATE "$<$<C_COMPILER_ID:GNU,Clang>:-w>" "$<$<C_COMPILER_ID:MSVC>:/w>")
    target_link_libraries(kin_audio PRIVATE kin_audio_codecs)

endif()

if(KIN_HAS_WEBGL2_RENDERER)
    include(kin_webgl_shaders)
    kin_embed_webgl_shaders(kin_engine ${_kin_webgl_shaders})
endif()

if(KIN_HAS_GPU_RENDERER)
    # SDL_GPU backend shaders: GLSL -> SPIR-V via glslc (Vulkan SDK), built into
    # the engine (kin_embed_files), so a game needs no shader files beside it.
    # Without glslc the engine still builds; the GPU backend then has no shaders and
    # kin falls back to SDL_Renderer. Shipping builds require them (KIN_REQUIRE_GPU_SHADERS).
    include(kin_gpu_shaders)
    # The .spv files stay in the build too, for tests that read every engine shader.
    kin_compile_glsl(_kin_gpu_spv OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders" SOURCES ${KIN_GPU_SHADER_SOURCES})
    kin_embed_files(kin_engine NAME embedded_gpu_shader NAMESPACE kin::gpu FILES ${_kin_gpu_spv})
    if(KIN_GLSLC AND NOT KIN_SHIPPING)
        # For runtime compiles of a game's shaders (kin/renderer/shader_compiler.hpp) on this machine.
        target_compile_definitions(kin_engine PRIVATE KIN_GLSLC_PATH="${KIN_GLSLC}" KIN_SHADER_INCLUDE_DIR="${KIN_SHADER_INCLUDE_DIR}")
    endif()
    list(LENGTH KIN_GPU_SHADER_SOURCES _kin_gpu_wanted)
    list(LENGTH _kin_gpu_spv _kin_gpu_built)
    if(NOT _kin_gpu_built EQUAL _kin_gpu_wanted)
        if(KIN_REQUIRE_GPU_SHADERS)
            message(FATAL_ERROR "The GPU backend has ${_kin_gpu_built} of its ${_kin_gpu_wanted} shaders: install "
                                "the Vulkan SDK (or your distribution's glslc), or pass KIN_SPIRV_DIR made by "
                                "cmake/kin_spirv.cmake; or set KIN_REQUIRE_GPU_SHADERS=OFF to ship without them")
        endif()
        message(STATUS "kin_engine: ${_kin_gpu_built} of ${_kin_gpu_wanted} SDL_GPU backend shaders (glslc not found)")
    endif()

endif()
