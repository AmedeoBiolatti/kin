#version 450 core
// kin's GI scene seen from the camera (RenderDebugOptions3D::gi_view; docs/3d_plan.md, G5): a full-screen triangle
// over the main pass. Each pixel's ray walks the voxels (kin_gi_march) to the first one a surface is in, and shows its
// colour, normal, emission, the light leaving it (the lit scene), or the probes' light where it is; or the walk's cost.
layout(location = 0) out vec4 FragColor;

layout(set = 2, binding = 0) uniform sampler3D uGiDistance;
layout(set = 2, binding = 1) uniform sampler3D uGiAlbedo;
layout(set = 2, binding = 2) uniform sampler3D uNormal;
layout(set = 2, binding = 3) uniform sampler3D uEmission;
layout(set = 2, binding = 4) uniform sampler3D uRadiance;
layout(set = 2, binding = 5) uniform sampler3D uGiIrradiance;
layout(set = 2, binding = 6) uniform sampler3D uGiMoments;
layout(set = 2, binding = 7) uniform sampler3D uGiProbes;
layout(set = 2, binding = 8) uniform sampler3D uRadianceBack;

#define KIN_GI_SCENE
#define KIN_GI_PROBES
#include "kin_gi.inc.glsl"

layout(std140, set = 3, binding = 0) uniform View {
    mat4 inverse_view_projection;
    vec4 camera;  // xyz: the eye; w: 1 orthographic
    vec4 forward; // xyz: the way the camera looks; w: what is shown (GiView3D: 1 colour, 2 normal, 3 emission, 4 steps,
                  // 5 the light leaving, 6 the probes' light)
    vec4 target;  // xy: the picture's size; z: 1 to encode to sRGB; w: 1 with probes
    KinGiScene scene;
    KinGiProbes probes;
} u;

vec3 encode(vec3 c) {
    c = clamp(c, 0.0, 1.0);
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c));
}

vec3 heat(float t) {
    t = clamp(t, 0.0, 1.0);
    return clamp(vec3(1.5 - abs(4.0 * t - 3.0), 1.5 - abs(4.0 * t - 2.0), 1.5 - abs(4.0 * t - 1.0)), 0.0, 1.0);
}

void main() {
    const vec2 uv = gl_FragCoord.xy / u.target.xy;
    const vec2 ndc = vec2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    const vec4 near = u.inverse_view_projection * vec4(ndc, 1.0, 1.0);
    vec3 from = u.camera.xyz, way = normalize(near.xyz / near.w - u.camera.xyz);
    if (u.camera.w > 0.5) {
        from = near.xyz / near.w;
        way = normalize(u.forward.xyz);
    }
    const int mode = int(u.forward.w + 0.5);
    vec3 c = vec3(0.04, 0.045, 0.06); // nothing hit
    int steps = 0;
    ivec3 voxel;
    int level;
    float t;
    if (kin_gi_march(u.scene, from, way, 1e30, 1024, voxel, level, t, steps)) {
        const vec4 albedo = texelFetch(uGiAlbedo, voxel, 0);
        const vec3 n = texelFetch(uNormal, voxel, 0).xyz * 2.0 - 1.0;
        if (mode == 1) {
            c = albedo.rgb * (0.55 + 0.45 * abs(dot(n, way)));
        } else if (mode == 2) {
            c = n * 0.5 + 0.5;
        } else if (mode == 3) {
            c = texelFetch(uEmission, voxel, 0).rgb;
        } else if (mode == 5) {
            c = (dot(n, way) <= 0.0 ? texelFetch(uRadiance, voxel, 0) : texelFetch(uRadianceBack, voxel, 0)).rgb;
        } else if (mode == 6 && u.target.w > 0.5) {
            // On the side the camera sees.
            const vec3 facing = dot(n, way) > 0.0 ? -n : n;
            vec3 light;
            if (kin_gi_irradiance(u.probes, from + way * t, facing, -way, light)) {
                c = light * u.probes.bias.z;
            }
        }
    }
    if (mode == 4) {
        c = heat(float(steps) / 96.0);
    }
    FragColor = vec4(u.target.z > 0.5 ? encode(c) : c, 1.0);
}
