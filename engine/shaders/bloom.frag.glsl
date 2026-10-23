#version 450 core
// kin's 3D view bloom: the light past a threshold, taken at a smaller size;
// then blurred along one direction at a time.
layout(location = 0) in vec2 vUV;
layout(set = 2, binding = 0) uniform sampler2D uSource;
layout(set = 3, binding = 0) uniform Params {
    vec4 a; // x: 0 bright pass, 1 blur; y: threshold; zw: a source texel (bright pass) or the blur's step
} params;
layout(location = 0) out vec4 FragColor;

void main() {
    if (params.a.x < 0.5) {
        // Four taps a half texel apart: a smooth average while shrinking.
        const vec2 h = params.a.zw * 0.5;
        vec3 c = texture(uSource, vUV + vec2(-h.x, -h.y)).rgb + texture(uSource, vUV + vec2(h.x, -h.y)).rgb +
                 texture(uSource, vUV + vec2(-h.x, h.y)).rgb + texture(uSource, vUV + vec2(h.x, h.y)).rgb;
        c *= 0.25;
        const float luma = dot(c, vec3(0.2126, 0.7152, 0.0722));
        // A soft knee: what is brighter than the threshold, in its colour.
        const float over = max(luma - params.a.y, 0.0);
        FragColor = vec4(c * (over / max(luma, 1e-4)), 1.0);
        return;
    }
    // Nine taps, Gaussian weights.
    const float w[5] = float[](0.2270270270, 0.1945945946, 0.1216216216, 0.0540540541, 0.0162162162);
    vec3 c = texture(uSource, vUV).rgb * w[0];
    for (int i = 1; i < 5; ++i) {
        c += texture(uSource, vUV + params.a.zw * float(i)).rgb * w[i];
        c += texture(uSource, vUV - params.a.zw * float(i)).rgb * w[i];
    }
    FragColor = vec4(c, 1.0);
}
