// The clouds' shadow (Scene3D::clouds, Clouds3D::shadows): the share of the sun that gets through the cloud layer to
// a world point, read from the map cloud_shadow.comp made: a square of the plane at the layer's middle height, round
// where the camera's ray toward the sun crosses it, each texel the transmittance along the sun's ray through it. Read
// by cloud_shadow_screen.comp (the meshes' sun, with the contact shadows), fog_volume.comp and fog_sun.comp.
//
// `map`: xy the square's middle (world x, z), z 1 / its side, w the plane's height. `toward`: xyz toward the sun (its
// height at least 0.05: the walk's), w the shadow's strength (0: none). Past the square's edge the shadow fades out.
float kin_cloud_shadow(sampler2D shadow, vec4 map, vec4 toward, vec3 p) {
    if (toward.w <= 0.0) {
        return 1.0;
    }
    vec2 q = p.xz + toward.xz * ((map.w - p.y) / toward.y);
    vec2 uv = (q - map.xy) * map.z + 0.5;
    float edge = max(abs(uv.x - 0.5), abs(uv.y - 0.5)) * 2.0;
    float through = textureLod(shadow, uv, 0.0).r;
    return mix(1.0, mix(through, 1.0, smoothstep(0.85, 1.0, edge)), toward.w);
}
