#version 450 core
// Depth of field (DepthOfField3D): the blur at half size, as Gustafsson's "Bokeh depth of field in a single pass"
// (2018): samples on a golden-angle spiral out to the widest radius that can reach the texel (its own, or a blurred
// thing in front of it, from the tiles); each counts as far as its own disc reaches back to the texel, a sample behind
// the texel no wider than twice the texel's own (so a sharp thing does not spill over what is behind it, and a
// blurred thing in front spreads over what is in focus behind it). The spiral starts at the same angle for every
// texel: a per-texel turn would be noise that nothing after this pass averages away. Writes the colour, and how wide
// the blur that reached the texel was (full pixels) with the texel's distance, for the combine.
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0) uniform sampler2D uHalf;     // dof_prepare's colour (linear sampler)
layout(set = 0, binding = 1) uniform sampler2D uHalfBlur; // and its radius and distance
layout(set = 0, binding = 2) uniform sampler2D uSpread;   // dof_spread's
layout(set = 1, binding = 0, rgba16f) uniform writeonly image2D uBlurred;
layout(set = 1, binding = 1, rgba16f) uniform writeonly image2D uReach; // x: radius that reached it, y: distance
layout(set = 2, binding = 0) uniform Params {
    vec4 shape; // x: the spiral's step (half texels; smaller: more samples), y: the widest radius (full pixels)
}
params;

const float golden_angle = 2.39996323;

void main() {
    const ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    const ivec2 extent = textureSize(uHalf, 0);
    if (any(greaterThanEqual(p, extent))) {
        return;
    }
    const vec4 centre = texelFetch(uHalf, p, 0);
    const vec2 own = texelFetch(uHalfBlur, p, 0).xy;
    const float own_size = abs(own.x) * 0.5; // half texels
    const float near = texelFetch(uSpread, p / 8, 0).x * 0.5;
    const float reach = min(max(near, own_size) + 1.0, params.shape.y * 0.5 + 1.0);
    if (reach < 1.5) {
        imageStore(uBlurred, p, centre);
        imageStore(uReach, p, vec4(abs(own.x), own.y, 0.0, 0.0));
        return;
    }
    const vec2 texel = 1.0 / vec2(extent);
    const vec2 at = (vec2(p) + 0.5) * texel;
    const float step_size = params.shape.x;
    vec4 colour = centre;
    float total = 1.0;
    float reached = abs(own.x);
    float radius = step_size;
    float angle = 0.0;
    for (int i = 0; i < 1024 && radius < reach; ++i) {
        const vec2 uv = at + vec2(cos(angle), sin(angle)) * radius * texel;
        const vec4 sample_colour = textureLod(uHalf, uv, 0.0);
        const vec2 sample_blur = textureLod(uHalfBlur, uv, 0.0).xy;
        float size = abs(sample_blur.x) * 0.5;
        if (sample_blur.y > own.y) {
            size = min(size, own_size * 2.0);
        }
        const float m = smoothstep(radius - 0.5, radius + 0.5, size);
        colour += mix(colour / total, sample_colour, m);
        total += 1.0;
        if (sample_blur.y < own.y) {
            reached = max(reached, abs(sample_blur.x) * m);
        }
        radius += step_size / radius;
        angle += golden_angle;
    }
    imageStore(uBlurred, p, colour / total);
    imageStore(uReach, p, vec4(reached, own.y, 0.0, 0.0));
}
