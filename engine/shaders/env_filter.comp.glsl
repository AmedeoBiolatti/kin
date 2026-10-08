#version 450 core
// kin's image-based light (render3d/environment.hpp): one level of an
// environment's filtered copies, from the picture and its mips. Each texel of
// the equirectangular result looks along its direction: the picture itself
// (mode 0, the sharpest level), the picture blurred by a GGX lobe of the
// level's roughness (mode 1), or the cosine-weighted average of the whole sky,
// what a diffuse surface facing it receives over pi (mode 2). Importance
// sampled, each sample read from the mip that matches how far apart the
// samples are (so a few hundred are smooth, not speckled).
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0) uniform sampler2D uSource;
layout(set = 1, binding = 0, rgba16f) uniform writeonly image2D uResult;
layout(set = 2, binding = 0) uniform Params {
    vec4 result; // xy: the level's size, z: its roughness, w: the mode
    vec4 source; // xy: the picture's size, z: samples
} params;

const float PI = 3.14159265358979;

// The equirectangular picture: the centre is +x, the top +y, right goes toward +z.
vec2 direction_to_uv(vec3 d) {
    return vec2(0.5 + atan(d.z, d.x) / (2.0 * PI), 0.5 - asin(clamp(d.y, -1.0, 1.0)) / PI);
}

vec3 uv_to_direction(vec2 uv) {
    const float phi = (uv.x - 0.5) * 2.0 * PI;
    const float theta = (0.5 - uv.y) * PI;
    return vec3(cos(phi) * cos(theta), sin(theta), sin(phi) * cos(theta));
}

float radical_inverse(uint bits) {
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10;
}

// A sample of the source along `d`, from the mip whose texels are as wide as the
// solid angle each sample stands for (`pdf` the density of the lobe).
vec3 radiance(vec3 d, float pdf, float samples) {
    const float texel = 4.0 * PI / (params.source.x * params.source.y);
    const float lod = max(0.5 * log2(1.0 / (samples * pdf * texel)) + 1.0, 0.0);
    return min(textureLod(uSource, direction_to_uv(d), lod).rgb, vec3(1e4));
}

void main() {
    const ivec2 xy = ivec2(gl_GlobalInvocationID.xy);
    const vec2 size = params.result.xy;
    if (xy.x >= int(size.x) || xy.y >= int(size.y)) {
        return;
    }
    const vec3 n = uv_to_direction((vec2(xy) + 0.5) / size);
    const int mode = int(params.result.w);
    if (mode == 0) {
        imageStore(uResult, xy, vec4(min(textureLod(uSource, direction_to_uv(n), 0.0).rgb, vec3(65000.0)), 1.0));
        return;
    }
    const uint count = uint(params.source.z);
    const vec3 up = abs(n.y) < 0.999 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    const vec3 tx = normalize(cross(up, n));
    const vec3 ty = cross(n, tx);
    const float alpha = max(params.result.z * params.result.z, 1e-3);
    vec3 sum = vec3(0.0);
    float weight = 0.0;
    for (uint i = 0u; i < count; ++i) {
        const vec2 xi = vec2((float(i) + 0.5) / float(count), radical_inverse(i));
        if (mode == 2) {
            // Cosine-weighted: the average radiance is E / pi.
            const float r = sqrt(xi.x), phi = 2.0 * PI * xi.y;
            const vec3 l = tx * (r * cos(phi)) + ty * (r * sin(phi)) + n * sqrt(1.0 - xi.x);
            sum += radiance(l, max(dot(n, l), 1e-3) / PI, float(count));
            weight += 1.0;
        } else {
            // GGX's half vector, the view along the normal (n = v = r).
            const float phi = 2.0 * PI * xi.y;
            const float cos_theta = sqrt((1.0 - xi.x) / (1.0 + (alpha * alpha - 1.0) * xi.x));
            const float sin_theta = sqrt(1.0 - cos_theta * cos_theta);
            const vec3 h = tx * (sin_theta * cos(phi)) + ty * (sin_theta * sin(phi)) + n * cos_theta;
            const vec3 l = 2.0 * dot(n, h) * h - n;
            const float n_l = dot(n, l);
            if (n_l > 0.0) {
                const float a2 = alpha * alpha;
                const float d = cos_theta * cos_theta * (a2 - 1.0) + 1.0;
                const float pdf = a2 / (PI * d * d) / 4.0; // D * n.h / (4 v.h), with v = n
                sum += radiance(l, pdf, float(count)) * n_l;
                weight += n_l;
            }
        }
    }
    imageStore(uResult, xy, vec4(sum / max(weight, 1e-4), 1.0));
}
