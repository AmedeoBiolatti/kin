#version 300 es
// Volumetric clouds in WebGL2: native clouds_apply.frag: the blend over the picture after the opaque meshes, at the
// depth of the clouds' mean distance (the farthest of the four texels the light is filtered from), tested against
// the meshes' (reverse Z, kin's 0 to 1 as the window's depth); "over" in encoded light for an 8-bit view.
precision highp float;
precision highp int;
uniform highp sampler2D uClouds, uDistance;
uniform mat4 uInverseViewProjection, uViewProjection;
uniform vec3 uEye;
uniform vec2 uTarget;
uniform bool uLinear;
out vec4 outColor;
vec3 encode(vec3 c) {
    c = clamp(c, 0.0, 1.0);
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c));
}
void main() {
    vec2 uv = gl_FragCoord.xy / uTarget;
    vec4 c = textureLod(uClouds, uv, 0.0);
    if (c.a >= 0.999) discard;
    ivec2 size = textureSize(uDistance, 0);
    ivec2 base = ivec2(floor(uv * vec2(size) - 0.5));
    float dist = 0.0;
    for (int i = 0; i < 4; ++i)
        dist = max(dist, texelFetch(uDistance, clamp(base + ivec2(i & 1, i >> 1), ivec2(0), size - 1), 0).r);
    vec4 far_point = uInverseViewProjection * vec4(uv * 2.0 - 1.0, 0.0, 1.0);
    vec3 dir = normalize(far_point.xyz - uEye * far_point.w);
    vec4 clip = uViewProjection * vec4(uEye + dir * max(dist, 1.0), 1.0);
    // (The meshes' vertex shaders map kin's 0 to 1 onto GL's -1 to 1, so the window's depth is kin's own.)
    gl_FragDepth = clamp(clip.z / max(clip.w, 1e-6), 0.0, 1.0);
    float cover = 1.0 - c.a;
    outColor = vec4(uLinear ? c.rgb : encode(c.rgb / max(cover, 1e-4)) * cover, cover);
}
