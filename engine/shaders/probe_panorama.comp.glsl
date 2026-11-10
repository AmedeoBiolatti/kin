#version 450 core
// A reflection probe's picture (Renderer3D::capture_reflection, render3d/reflection_probes3d.hpp): its six square
// 90-degree views, layers of uFaces, into one equirectangular panorama in world space, as kin's environments are
// (render3d/environment3d.hpp; its mips and the filter follow, as an environment's do). Each texel looks along its
// direction through the view whose forward is nearest it.
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0) uniform sampler2DArray uFaces;
layout(set = 1, binding = 0, rgba16f) uniform writeonly image2D uResult;
layout(set = 2, binding = 0) uniform Params {
    vec4 size;       // xy: the panorama's
    vec4 right[6];   // each view's camera: its right, up and forward (world space)
    vec4 up[6];
    vec4 forward[6];
} params;

const float PI = 3.14159265358979;

// The equirectangular picture: the centre is +x, the top +y, right goes toward +z.
vec3 uv_to_direction(vec2 uv) {
    const float phi = (uv.x - 0.5) * 2.0 * PI;
    const float theta = (0.5 - uv.y) * PI;
    return vec3(cos(phi) * cos(theta), sin(theta), sin(phi) * cos(theta));
}

void main() {
    const ivec2 xy = ivec2(gl_GlobalInvocationID.xy);
    if (xy.x >= int(params.size.x) || xy.y >= int(params.size.y)) {
        return;
    }
    const vec3 d = uv_to_direction((vec2(xy) + 0.5) / params.size.xy);
    int face = 0;
    float most = -2.0;
    for (int f = 0; f < 6; ++f) {
        const float c = dot(d, params.forward[f].xyz);
        if (c > most) {
            most = c;
            face = f;
        }
    }
    // Where the view shows `d`: its normalized device coordinates, the top row first.
    const vec2 ndc = vec2(dot(d, params.right[face].xyz), dot(d, params.up[face].xyz)) / most;
    const vec2 uv = vec2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    const vec3 c = textureLod(uFaces, vec3(uv, float(face)), 0.0).rgb;
    imageStore(uResult, xy, vec4(any(isnan(c)) || any(isinf(c)) ? vec3(0.0) : c, 1.0));
}
