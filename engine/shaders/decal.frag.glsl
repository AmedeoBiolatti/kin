#version 450 core
// Decals (Scene3D::draw_decal; docs/rendering_3d.md, Decals): a picture projected along its box's -Y onto the opaque
// surfaces inside the box, lit as a custom material is (kin_material.inc.glsl: the sun and its shadows, the fill light,
// the ambient and environment light with the ambient occlusion, point and spot lights and their shadows, fog), and
// blended over them. The surface comes from the pre-pass: its depth gives the point (the world position under the
// pixel), its normals the normal (the mesh's, before its normal map). The picture's colour and alpha (uBase, binding 0)
// and its optional normal map (uNormalMap, binding 3, tangent space, green up) take the material's places.
#define KIN_DECAL
#include "kin_material.inc.glsl"

layout(set = 2, binding = 12) uniform sampler2D uDecalDepth;   // the pre-pass's depth (reverse Z: 0 is the sky)
layout(set = 2, binding = 13) uniform sampler2D uDecalNormals; // and its world normals, octahedral in RG

layout(location = 0) flat in mat4 dToDecal;
layout(location = 4) flat in mat4 dToWorld;
layout(location = 8) flat in vec4 dColour;
layout(location = 9) flat in vec4 dSurface;
layout(location = 10) flat in vec4 dFade;
layout(location = 11) flat in vec3 dAxisX;
layout(location = 12) flat in vec3 dAxisY;
layout(location = 13) flat in vec3 dAxisZ;

// As kin_depth.inc.glsl's: the pre-pass's normal (its velocity variant marks independent motion with y + 4).
vec3 decal_normal(vec2 e) {
    if (e.y > 2.0)
        e.y -= 4.0;
    vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0) {
        n.xy = (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    }
    return normalize(n);
}

// The world position the pre-pass saw at pixel `p` (SDL_GPU's clip space: y up, rows down), and its depth.
vec3 decal_world(ivec2 p, out float depth) {
    depth = texelFetch(uDecalDepth, p, 0).r;
    const vec2 uv = (vec2(p) + 0.5) * frame.occlusion.zw;
    const vec4 w = dToWorld * vec4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0);
    return w.xyz / w.w;
}

void main() {
    const ivec2 size = textureSize(uDecalDepth, 0);
    const ivec2 p = clamp(ivec2(gl_FragCoord.xy), ivec2(0), size - 1);
    float depth;
    const vec3 world = decal_world(p, depth);
    if (depth <= 0.0) {
        discard; // the sky
    }
    const vec3 local = (dToDecal * vec4(world, 1.0)).xyz;
    if (any(greaterThan(abs(local), vec3(0.5)))) {
        discard;
    }
    // The texture's gradients from the neighbours on the same surface (the nearer in depth of each pair), not from the
    // 2 x 2 quad's: those jump at the edges of what is in front and would take the smallest level there.
    float d0, d1, d2, d3;
    const vec3 right = decal_world(min(p + ivec2(1, 0), size - 1), d0), left = decal_world(max(p - ivec2(1, 0), 0), d1);
    const vec3 down = decal_world(min(p + ivec2(0, 1), size - 1), d2), up = decal_world(max(p - ivec2(0, 1), 0), d3);
    const vec3 dx = abs(d0 - depth) < abs(depth - d1) ? right - world : world - left;
    const vec3 dy = abs(d2 - depth) < abs(depth - d3) ? down - world : world - up;
    const vec2 uv = local.xz + 0.5;
    const vec2 uv_dx = (mat3(dToDecal) * dx).xz, uv_dy = (mat3(dToDecal) * dy).xz;

    vec3 n = decal_normal(texelFetch(uDecalNormals, p, 0).rg);
    // Fades: where the surface turns away from the way the decal is projected from (dFade.x: the cosine below which it
    // fades out; none at 0, but never onto a surface facing away), and toward the box's top and bottom (dFade.y: the
    // share of its half depth).
    const float facing = dot(n, dAxisY);
    float fade = facing > 0.0 ? (dFade.x > 0.0 ? clamp(facing / dFade.x, 0.0, 1.0) : 1.0) : 0.0;
    if (dFade.y > 0.0) {
        fade *= clamp((0.5 - abs(local.y)) / (0.5 * dFade.y), 0.0, 1.0);
    }
    vec4 base = textureGrad(uBase, uv, uv_dx, uv_dy);
    if (frame.flags.y > 0.5) {
        base.rgb = decode(base.rgb);
    }
    base *= dColour;
    const float alpha = base.a * fade;
    if (alpha < 1.0 / 512.0) {
        discard;
    }
    // The surface's varyings, for the lighting's shadows and views (shadow_offset_normal reads vNormal).
    vWorld = world;
    vNormal = n;
    vColor = vec4(1.0);
    vUV = uv;
    if (dSurface.z > 0.0) {
        // The picture's frame on the surface: u along the box's X, the map's green up toward the picture's top (-Z).
        const vec3 t = normalize(dAxisX - n * dot(n, dAxisX));
        const vec3 b = normalize(-dAxisZ - n * dot(n, -dAxisZ));
        vec3 m = textureGrad(uNormalMap, uv, uv_dx, uv_dy).xyz * 2.0 - 1.0;
        if (m.z < 0.0) {
            m.z = sqrt(max(1.0 - dot(m.xy, m.xy), 0.0));
        }
        m.xy *= dSurface.z;
        n = normalize(t * m.x + b * m.y + n * m.z);
    }
    const vec3 lit = kin_lit(base.rgb, n, dSurface.y, clamp(dSurface.x, 0.045, 1.0), 1.0) + base.rgb * dSurface.w;
    FragColor = kin_output(lit, alpha);
}
