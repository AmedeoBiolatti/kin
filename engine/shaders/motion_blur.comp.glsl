#version 450 core
// Motion blur (MotionBlur3D), last of four: each pixel gathers the colours along the longest smear round it (its tile
// neighbourhood's) and along its own, weighed as McGuire et al. ("A Reconstruction Filter for Plausible Motion Blur",
// 2012) do: a sample in front whose own smear reaches this pixel covers it; a sample behind shows where this pixel's
// own smear uncovers what is behind it; two blurred samples blend. The samples are spread by a per-pixel noise
// (interleaved gradient noise: steady from frame to frame). Premultiplied colour, coverage included (a view over a
// transparent background smears its edges into it). Where nothing round moves over half a pixel, the pixel is copied.
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0) uniform sampler2D uColour;     // the resolved picture, linear
layout(set = 0, binding = 1) uniform sampler2D uPrepared;   // motion_blur_prepare's
layout(set = 0, binding = 2) uniform sampler2D uNeighbours; // motion_blur_neighbours'
layout(set = 1, binding = 0, rgba16f) uniform writeonly image2D uResult;
layout(set = 2, binding = 0) uniform Params {
    vec4 size;  // the output's extent and reciprocals
    vec4 blur;  // x: tile size in pixels, y: samples (even)
}
params;

float noise(vec2 p) {
    return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}

// 1 where `near` is in front of `far` (or level), falling to 0 over a few percent of the distance behind it.
float in_front(float near, float far) {
    return clamp(1.0 - (near - far) / (0.03 * min(near, far) + 0.01), 0.0, 1.0);
}

float cone(float distance, float reach) {
    return clamp(1.0 - distance / reach, 0.0, 1.0);
}

float cylinder(float distance, float reach) {
    return 1.0 - smoothstep(0.95 * reach, 1.05 * reach, distance);
}

void main() {
    const ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    const ivec2 extent = ivec2(params.size.xy);
    if (any(greaterThanEqual(p, extent))) {
        return;
    }
    const vec4 centre = texelFetch(uColour, p, 0);
    const vec2 around = texelFetch(uNeighbours, p / int(params.blur.x), 0).xy;
    if (dot(around, around) < 0.25) {
        imageStore(uResult, p, centre);
        return;
    }
    const vec4 own = texelFetch(uPrepared, p, 0);
    const float own_reach = max(own.w, 0.5);
    // Half the samples follow the pixel's own smear where it moves another way than its neighbourhood's longest
    // (Jimenez, "Next Generation Post Processing in Call of Duty: Advanced Warfare", 2014).
    const vec2 along_own = own.w > 0.5 ? own.xy : around;
    const int samples = int(params.blur.y);
    const float jitter = noise(vec2(p)) - 0.5;
    float total = 1.0 / own_reach;
    vec4 sum = centre * total;
    for (int i = 0; i < samples; ++i) {
        const float t = mix(-1.0, 1.0, (float(i) + jitter + 1.0) / float(samples + 1));
        const vec2 offset = t * ((i & 1) == 0 ? around : along_own);
        const ivec2 s = clamp(ivec2(floor(vec2(p) + 0.5 + offset)), ivec2(0), extent - 1);
        const vec4 sample_info = texelFetch(uPrepared, s, 0);
        const float distance = length(offset);
        const float reach = max(sample_info.w, 0.5);
        const float w = in_front(sample_info.z, own.z) * cone(distance, reach) +
                        in_front(own.z, sample_info.z) * cone(distance, own_reach) +
                        cylinder(distance, reach) * cylinder(distance, own_reach) * 2.0;
        sum += texelFetch(uColour, s, 0) * w;
        total += w;
    }
    imageStore(uResult, p, sum / total);
}
