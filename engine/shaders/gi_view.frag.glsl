#version 450 core
// kin's GI scene seen from the camera (RenderDebugOptions3D::gi_view; docs/3d_plan.md, G5a): a full-screen triangle
// over the main pass. Each pixel's ray walks the voxels it passes through, one by one where a surface is near and in
// jumps the distances allow elsewhere (from a voxel whose centre is d voxels from the nearest surface voxel's, no
// surface voxel's box is nearer than d - 1.74 to any point of it), to the first voxel a surface is in.
layout(location = 0) out vec4 FragColor;

layout(set = 2, binding = 0) uniform sampler3D uDistance;
layout(set = 2, binding = 1) uniform sampler3D uAlbedo;
layout(set = 2, binding = 2) uniform sampler3D uNormal;
layout(set = 2, binding = 3) uniform sampler3D uEmission;

layout(std140, set = 3, binding = 0) uniform View {
    mat4 inverse_view_projection;
    vec4 camera;  // xyz: the eye; w: 1 orthographic
    vec4 forward; // xyz: the way the camera looks
    vec4 origin;  // xyz: voxel (0, 0, 0)'s low corner; w: voxels a unit
    vec4 size;    // xyz: voxels a side; w: what is shown (GiView3D: 1 colour, 2 normal, 3 emission, 4 steps)
    vec4 target;  // xy: the picture's size; z: 1 to encode to sRGB
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
    // In voxels.
    const vec3 o = (from - u.origin.xyz) * u.origin.w;
    const vec3 size = u.size.xyz;
    const vec3 inv = 1.0 / way;
    const vec3 t0 = (vec3(0.0) - o) * inv, t1 = (size - o) * inv;
    const float enter = max(max(max(min(t0.x, t1.x), min(t0.y, t1.y)), min(t0.z, t1.z)), 0.0);
    const float leave = min(min(max(t0.x, t1.x), max(t0.y, t1.y)), max(t0.z, t1.z));
    vec3 c = vec3(0.04, 0.045, 0.06); // nothing hit
    int steps = 0;
    if (enter < leave) {
        float t = enter + 1e-4;
        const ivec3 last = ivec3(size) - 1;
        for (; steps < 1024 && t < leave; ++steps) {
            const vec3 q = o + way * t;
            const ivec3 voxel = clamp(ivec3(floor(q)), ivec3(0), last);
            const vec4 albedo = texelFetch(uAlbedo, voxel, 0);
            if (albedo.a > 0.0) {
                const vec3 n = texelFetch(uNormal, voxel, 0).xyz * 2.0 - 1.0;
                const int mode = int(u.size.w + 0.5);
                if (mode == 1) {
                    c = albedo.rgb * (0.55 + 0.45 * abs(dot(n, way)));
                } else if (mode == 2) {
                    c = n * 0.5 + 0.5;
                } else if (mode == 3) {
                    c = texelFetch(uEmission, voxel, 0).rgb;
                }
                break;
            }
            const float d = texelFetch(uDistance, voxel, 0).r;
            if (d >= 3.0) {
                t += d - 1.74;
            } else {
                // To the voxel's far side along the ray, and a little past it.
                const vec3 exit = (vec3(voxel) + step(vec3(0.0), way) - o) * inv;
                t = max(min(min(exit.x, exit.y), exit.z), t) + 1e-4;
            }
        }
    }
    if (int(u.size.w + 0.5) == 4) {
        c = heat(float(steps) / 96.0);
    }
    FragColor = vec4(u.target.z > 0.5 ? encode(c) : c, 1.0);
}
