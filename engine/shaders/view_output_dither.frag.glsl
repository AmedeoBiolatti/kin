#version 450 core
// view_output.frag with a dither of one 8-bit step, for a view with a sky (Scene3D::sky): a night sky's whole range
// is a few levels of 8 bits, and without it a smooth gradient comes out in rings. Triangular noise from interleaved
// gradient noise, a function of the pixel (so it holds still under temporal anti-aliasing), added after the tonemap
// and the encode, where the 8 bits are. Only when the output is 8-bit, encoded here (modes 0 and 3).
layout(location = 0) in vec2 vUV;
layout(set = 2, binding = 0) uniform sampler2D uScene;
layout(set = 2, binding = 1) uniform sampler2D uBloom;
layout(set = 3, binding = 0) uniform Params {
    vec4 a; // as view_output.frag's
} params;
layout(location = 0) out vec4 FragColor;

vec3 aces(vec3 x) {
    return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}

vec3 encode(vec3 c) {
    c = clamp(c, 0.0, 1.0);
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c));
}

float noise(vec2 p) {
    return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}

void main() {
    const vec4 scene = texture(uScene, vUV);
    const vec3 bloom = params.a.x > 0.0 ? texture(uBloom, vUV).rgb * params.a.x : vec3(0.0);
    vec3 c = scene.rgb + bloom;
    const float alpha = clamp(scene.a + dot(bloom, vec3(0.2126, 0.7152, 0.0722)), 0.0, 1.0);
    // As view_output.frag's: a partly covered pixel is exposed and tonemapped out of its alpha.
    const bool partial = params.a.z < 1.5 && alpha > 0.0 && alpha < 1.0;
    if (partial) {
        c /= alpha;
    }
    if (params.a.z < 1.5) {
        c = aces(c * params.a.y);
    }
    if ((params.a.z < 0.5 || params.a.z > 2.5) && alpha > 0.0) {
        c = encode(partial ? c : c / alpha) * alpha;
        c += (noise(gl_FragCoord.xy) + noise(gl_FragCoord.xy + vec2(5.0, 3.0)) - 1.0) * (alpha / 255.0);
    } else if (partial) {
        c *= alpha;
    }
    FragColor = vec4(c, alpha);
}
