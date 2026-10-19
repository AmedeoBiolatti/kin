#version 450 core
// kin's 3D view, HDR, out to what 2D draws: the scene plus its bloom, then
// (for an 8-bit 2D pipeline) exposed and tonemapped, and encoded to sRGB for
// a gamma one. Premultiplied in and out.
layout(location = 0) in vec2 vUV;
layout(set = 2, binding = 0) uniform sampler2D uScene;
layout(set = 2, binding = 1) uniform sampler2D uBloom;
layout(set = 3, binding = 0) uniform Params {
    vec4 a; // x: bloom strength (0: none), y: exposure, z: 0 tonemap and encode, 1 tonemap, 2 neither (an HDR 2D pipeline), 3 encode only (a temporally anti-aliased 8-bit view: no exposure or tonemap)
} params;
layout(location = 0) out vec4 FragColor;

vec3 aces(vec3 x) {
    return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}

vec3 encode(vec3 c) {
    c = clamp(c, 0.0, 1.0);
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c));
}

void main() {
    const vec4 scene = texture(uScene, vUV);
    const vec3 bloom = params.a.x > 0.0 ? texture(uBloom, vUV).rgb * params.a.x : vec3(0.0);
    vec3 c = scene.rgb + bloom;
    // The glow shows over a transparent background too.
    const float alpha = clamp(scene.a + dot(bloom, vec3(0.2126, 0.7152, 0.0722)), 0.0, 1.0);
    // The exposure and the tonemap are of the colour, not of the colour times its coverage: a partly covered pixel (an
    // edge over a transparent background) is taken out of its alpha first and put back in it last. An opaque or empty
    // one runs as it always did.
    const bool partial = params.a.z < 1.5 && alpha > 0.0 && alpha < 1.0;
    if (partial) {
        c /= alpha;
    }
    if (params.a.z < 1.5) {
        c = aces(c * params.a.y);
    }
    if ((params.a.z < 0.5 || params.a.z > 2.5) && alpha > 0.0) {
        c = encode(partial ? c : c / alpha) * alpha;
    } else if (partial) {
        c *= alpha;
    }
    FragColor = vec4(c, alpha);
}
