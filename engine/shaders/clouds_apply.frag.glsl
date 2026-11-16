#version 450 core
// The clouds (Scene3D::clouds) over the picture, in the main pass after the sky and the opaque meshes: the blend
// clouds_resolve.comp made, read linearly at the pixel, drawn over what is behind it (the share of it still seen is the
// blend's a: premultiplied, "over") at the depth of the clouds' mean distance along the pixel's ray, tested against the
// meshes' depth: a near wall hides the clouds, a far mountain is seen through them.
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 FragColor;

layout(set = 2, binding = 0) uniform sampler2D uClouds;   // rgb the light, a the share behind still seen
layout(set = 2, binding = 1) uniform sampler2D uDistance; // the clouds' distance (gathered)

layout(std140, set = 3, binding = 0) uniform Params {
    mat4 inverse_view_projection; // clip space (as the meshes are drawn: jittered) to world
    mat4 view_projection;         // and back
    vec4 eye;                     // xyz: the camera
    vec4 target;                  // xy: the picture's size, z: 1 when it holds sRGB-encoded colour (an 8-bit view)
} params;

vec3 kin_encode(vec3 c) {
    c = clamp(c, 0.0, 1.0);
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c));
}

void main() {
    const vec2 uv = gl_FragCoord.xy / params.target.xy;
    const vec4 c = textureLod(uClouds, uv, 0.0);
    if (c.a >= 0.999) {
        discard;
    }
    // The farthest distance of the four texels the light was filtered from: a texel whose ray missed the layer holds
    // 0 (and is filtered in at a cloud's edge), and a cloud is never nearer than it is.
    const vec4 around = textureGather(uDistance, uv, 0);
    const float dist = max(max(around.x, around.y), max(around.z, around.w));
    const vec4 near = params.inverse_view_projection * vec4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 1.0, 1.0);
    const vec3 dir = normalize(near.xyz / near.w - params.eye.xyz);
    const vec4 clip = params.view_projection * vec4(params.eye.xyz + dir * max(dist, 1.0), 1.0);
    // Reverse Z: nearer is greater; past a finite far plane the clouds are at the far plane (behind every mesh).
    gl_FragDepth = clamp(clip.z / max(clip.w, 1e-6), 0.0, 1.0);
    const float cover = 1.0 - c.a;
    // An encoded picture: the clouds' own colour encoded, over it by their cover (as the blended meshes are drawn).
    const vec3 light = params.target.z > 0.5 ? kin_encode(c.rgb / max(cover, 1e-4)) * cover : c.rgb;
    FragColor = vec4(light, cover);
}
