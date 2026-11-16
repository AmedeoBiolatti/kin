#version 450 core
// The lens' ghosts and halo (Glow3D), as Chapman's screen-space lens flare ("Pseudo Lens Flare", 2013): the bright light
// mirrored through the middle of the view, read again at steps toward the middle (the ghosts, fading toward the
// view's edges) and at a fixed distance out along the line from the middle (the halo, a ring), each with red and blue
// read a little apart along that line (a lens' colour fringe).
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0) uniform sampler2D uBright; // a small level of the glow's chain, down (linear sampler)
layout(set = 1, binding = 0, rgba16f) uniform writeonly image2D uFlare;
layout(set = 2, binding = 0) uniform Params {
    vec4 ghosts; // x: strength, y: how many, z: spacing
    vec4 halo;   // x: strength, y: width, z: the view's width over height
}
params;

vec3 fringed(vec2 uv, vec2 direction) {
    const vec2 split = direction * 0.006;
    return vec3(textureLod(uBright, uv - split, 0.0).r, textureLod(uBright, uv, 0.0).g,
                textureLod(uBright, uv + split, 0.0).b);
}

void main() {
    const ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    const ivec2 size = imageSize(uFlare);
    if (any(greaterThanEqual(p, size))) {
        return;
    }
    const vec2 uv = vec2(1.0) - (vec2(p) + 0.5) / vec2(size); // mirrored through the middle
    const vec2 toward = (vec2(0.5) - uv) * params.ghosts.z;
    const vec2 direction = length(toward) > 1e-5 ? normalize(toward) : vec2(0.0);
    vec3 light = vec3(0.0);
    if (params.ghosts.x > 0.0) {
        const int count = int(params.ghosts.y);
        for (int i = 0; i < count; ++i) {
            const vec2 at = fract(uv + toward * float(i));
            const float fade = pow(max(1.0 - length(vec2(0.5) - at) / 0.70710678, 0.0), 10.0);
            light += fringed(at, direction) * fade;
        }
        light *= params.ghosts.x;
    }
    if (params.halo.x > 0.0) {
        // Along the line from the middle, the same distance out at every pixel (round on the screen).
        vec2 out_to = direction * vec2(1.0, params.halo.z);
        out_to = length(out_to) > 1e-5 ? normalize(out_to) * vec2(1.0, 1.0 / params.halo.z) : out_to;
        const vec2 at = uv + out_to * params.halo.y;
        const float ring = pow(max(1.0 - length(vec2(0.5) - fract(at)) / 0.70710678, 0.0), 5.0);
        light += fringed(at, direction) * ring * params.halo.x;
    }
    imageStore(uFlare, p, vec4(light, 1.0));
}
