# Shared built-in shader manifest for engine builds and offline SPIR-V packaging.
# Put 3D-only shaders in the second list so a 2D build never compiles/embeds them.
set(_kin_gpu_shader_names_2D
    textured_quad.vert textured_quad.frag sprite_instanced.vert shader_geometry.vert normal_sprite.frag overdraw_count.frag
    overdraw_heat.frag shape.frag shape.vert sdf_shape.frag sdf_shape.vert distance_field.frag mask_composite.frag
    mask_stencil.frag color_output.frag pp_vignette.frag pp_color_grade.frag pp_scanline.frag pp_chroma.frag pp_ascii.frag
    pp_bloom_bright.frag pp_bloom_blur.frag pp_bloom_combine.frag transition_dissolve.frag
    transition_pixelate.frag transition_wipe.frag transition_iris.frag transition_crossfade.frag)
set(_kin_gpu_shader_names_3D
    mesh_material.vert mesh_material.frag mesh_material_fog.frag mesh_material_light.frag mesh_material_light_fog.frag
    mesh_material_opaque.frag mesh_material_opaque_fog.frag mesh_material_opaque_light.frag mesh_material_opaque_light_fog.frag
    mesh_indexed.vert mesh_unlit_indexed.vert mesh_unlit_textured_indexed.vert prepass_indexed.vert mesh.vert
    mesh.frag shadow.vert shadow.frag shadow_restore.frag portal_shadow.frag shadow_mask.vert shadow_mask.frag pick_mask.frag visibility.frag visibility_mask.frag mesh_unlit.vert
    mesh_unlit_textured.vert mesh_unlit.frag mesh_unlit_textured.frag mesh_unlit_mask.frag
    mesh_unlit_textured_mask.frag fullscreen.vert bloom.frag view_output.frag pick.frag env_filter.comp probe_panorama.comp ssr_trace.comp ssr_pyramid.comp
    prepass.vert prepass.frag prepass_mask.frag gtao.comp gtao_chain.comp ao_depth_chain.comp ao_blur.comp ao_upsample.comp
    ao_blur_chain.comp ao_blur_both.comp ao_blur_both_chain.comp contact_blur.comp contact_blur_chain.comp contact.comp prepass_velocity.vert
    prepass_velocity.frag prepass_velocity_mask.frag taa.comp taau.comp taau_output.comp mesh_velocity.vert mesh_velocity.frag
    cluster_cull.comp cluster_cull_occ.comp cluster_scan.comp cluster_args.comp cluster_gather.comp hiz_depth.comp
    hiz_reduce.comp mesh_skinned.vert mesh_skinned_velocity.vert shadow_skinned.vert shadow_mask_skinned.vert
    prepass_skinned.vert prepass_velocity_skinned.vert mesh_fog.frag mesh_velocity_fog.frag mesh_light.frag
    mesh_light_fog.frag mesh_velocity_light.frag mesh_velocity_light_fog.frag sky.frag sky_velocity.frag
    sky_velocity_only.frag sky_env.frag view_output_dither.frag particle.vert particle.frag particle_soft.frag
    particle_temporal.vert particle_gpu_temporal.vert particle_gpu.vert particle_gpu_light.vert particle_gpu_shadow.vert
    particle_order_init.comp particle_order_merge.comp particle_simulate.comp
    particle_composition_gather.comp particle_composition_keys.comp particle_composition_args.comp
    particle_composition.vert particle_composition.frag particle_composition_reactive.frag particle_atlas.frag taa_reactive.frag particle_reactive.frag particle_soft_reactive.frag particle_light.vert
    particle_light.frag particle_soft_light.frag particle_reactive_light.frag particle_soft_reactive_light.frag particle_shadow.vert particle_shadow.frag
    particle_soft_shadow.frag particle_reactive_shadow.frag particle_soft_reactive_shadow.frag particle_cast.frag
    gi_clear.comp gi_voxelize.comp gi_voxelize_attributes.comp gi_flood.comp gi_view.frag gi_proxy.frag
    gi_light.comp gi_probe_place.comp gi_probe_trace.comp gi_probe_update.comp gi_resolve.comp gi_surface.comp gi_area.comp gi_area_fill.comp fog_sun.comp fog_sun_apply.frag
    exposure_histogram.comp exposure_adapt.comp view_output_auto.frag view_output_dither_auto.frag decal.vert decal.frag morph_blend.comp
    prepass_velocity_morph.vert prepass_velocity_morph_skinned.vert
    fog_volume.comp fog_volume_integrate.comp fog_volume_apply.frag fog_volume_depth.comp
    motion_blur_prepare.comp motion_blur_tiles.comp motion_blur_neighbours.comp motion_blur.comp
    dof_focus.comp dof_prepare.comp dof_tiles.comp dof_spread.comp dof_gather.comp dof_combine.comp
    view_output_effects.frag glow_down.comp glow_up.comp glow_flare.comp glow_streak.comp glow_compose.comp
    subsurface_scatter.comp subsurface_combine.frag
    cloud_noise.comp cloud_weather.comp clouds_trace.comp clouds_resolve.comp clouds_apply.frag cloud_shadow.comp
    cloud_shadow_screen.comp clouds_env.frag
    mesh_lightmap.vert mesh_lightmap_indexed.vert mesh_velocity_lightmap.vert mesh_baked.frag mesh_baked_fog.frag mesh_velocity_baked.frag
    mesh_velocity_baked_fog.frag)

set(_kin_gpu_shader_names ${_kin_gpu_shader_names_2D})
if(KIN_EXPERIMENTAL_3D)
    list(APPEND _kin_gpu_shader_names ${_kin_gpu_shader_names_3D})
endif()
set(KIN_GPU_SHADER_SOURCES)
foreach(_name IN LISTS _kin_gpu_shader_names)
    list(APPEND KIN_GPU_SHADER_SOURCES "${CMAKE_CURRENT_LIST_DIR}/../engine/shaders/${_name}.glsl")
endforeach()
