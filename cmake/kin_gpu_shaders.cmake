# Shared built-in shader manifest for engine builds and offline SPIR-V packaging.
# Put 3D-only shaders in the second list so a 2D build never compiles/embeds them.
set(_kin_gpu_shader_names_2D
    textured_quad.vert textured_quad.frag sprite_instanced.vert shader_geometry.vert overdraw_count.frag
    overdraw_heat.frag shape.frag shape.vert sdf_shape.frag sdf_shape.vert distance_field.frag mask_composite.frag
    mask_stencil.frag color_output.frag pp_vignette.frag pp_color_grade.frag pp_scanline.frag pp_chroma.frag
    pp_bloom_bright.frag pp_bloom_blur.frag pp_bloom_combine.frag transition_dissolve.frag
    transition_pixelate.frag transition_wipe.frag transition_iris.frag transition_crossfade.frag)
set(_kin_gpu_shader_names_3D
    mesh_indexed.vert mesh_unlit_indexed.vert mesh_unlit_textured_indexed.vert prepass_indexed.vert mesh.vert
    mesh.frag shadow.vert shadow.frag shadow_mask.vert shadow_mask.frag pick_mask.frag mesh_unlit.vert
    mesh_unlit_textured.vert mesh_unlit.frag mesh_unlit_textured.frag mesh_unlit_mask.frag
    mesh_unlit_textured_mask.frag fullscreen.vert bloom.frag view_output.frag pick.frag env_filter.comp
    prepass.vert prepass.frag prepass_mask.frag gtao.comp gtao_chain.comp ao_depth_chain.comp ao_blur.comp
    ao_blur_chain.comp ao_blur_both.comp ao_blur_both_chain.comp contact.comp prepass_velocity.vert
    prepass_velocity.frag prepass_velocity_mask.frag taa.comp mesh_velocity.vert mesh_velocity.frag
    cluster_cull.comp cluster_cull_occ.comp cluster_scan.comp cluster_args.comp cluster_gather.comp hiz_depth.comp
    hiz_reduce.comp mesh_skinned.vert mesh_skinned_velocity.vert shadow_skinned.vert shadow_mask_skinned.vert
    prepass_skinned.vert prepass_velocity_skinned.vert mesh_fog.frag mesh_velocity_fog.frag mesh_light.frag
    mesh_light_fog.frag mesh_velocity_light.frag mesh_velocity_light_fog.frag sky.frag sky_velocity.frag
    sky_velocity_only.frag sky_env.frag view_output_dither.frag particle.vert particle.frag particle_soft.frag
    particle_temporal.vert taa_reactive.frag particle_reactive.frag particle_soft_reactive.frag particle_light.vert
    particle_light.frag particle_soft_light.frag particle_reactive_light.frag particle_soft_reactive_light.frag particle_shadow.vert particle_shadow.frag
    particle_soft_shadow.frag particle_reactive_shadow.frag particle_soft_reactive_shadow.frag particle_cast.frag
    gi_clear.comp gi_voxelize.comp gi_voxelize_attributes.comp gi_flood.comp gi_view.frag
    gi_light.comp gi_probe_place.comp gi_probe_trace.comp gi_probe_update.comp gi_resolve.comp fog_sun.comp fog_sun_apply.frag)

set(_kin_gpu_shader_names ${_kin_gpu_shader_names_2D})
if(KIN_EXPERIMENTAL_3D)
    list(APPEND _kin_gpu_shader_names ${_kin_gpu_shader_names_3D})
endif()
set(KIN_GPU_SHADER_SOURCES)
foreach(_name IN LISTS _kin_gpu_shader_names)
    list(APPEND KIN_GPU_SHADER_SOURCES "${CMAKE_CURRENT_LIST_DIR}/../engine/shaders/${_name}.glsl")
endforeach()
