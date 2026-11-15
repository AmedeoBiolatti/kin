#version 300 es
// An HDR view's way to the 2D frame (WebGL2): native bloom.frag's bloom, auto exposure's metering and adaptation
// (exposure_histogram.comp and exposure_adapt.comp's histogram, made by one fragment from a small metered picture: no
// compute here) and kin_view_output.inc.glsl's output. Every pass draws a full-screen triangle into its target;
// `uTarget` is that target's size.
precision highp float;
precision highp int;
uniform int uMode; // 0 bright pass, 1 blur, 2 meter, 3 adapt, 4 output
uniform vec2 uTarget;
uniform highp sampler2D uSource; // the picture read (the scene, or the bloom's other target)
uniform highp sampler2D uBloom;  // the output's bloom
uniform highp sampler2D uState;  // auto exposure's last state: x ev, y exposure, z target
uniform vec4 uA;                 // bright pass: y threshold, zw a source texel; blur: zw the step
                                 // meter: x centre weight; output: x bloom, y exposure, z 1 with auto exposure, w dither
uniform vec4 uB, uC;             // adapt: native exposure_adapt.comp's P.a and P.b (P.c in uC)
out vec4 outColor;

const int BINS = 128;
const float LOWEST_LOG = -14.0, BINS_PER_STOP = 127.0 / 28.0; // as native meter_exposure's

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
    vec2 uv = gl_FragCoord.xy / uTarget;
    if (uMode == 0) {
        vec2 h = uA.zw * 0.5;
        vec3 c = texture(uSource, uv + vec2(-h.x, -h.y)).rgb + texture(uSource, uv + vec2(h.x, -h.y)).rgb +
                 texture(uSource, uv + vec2(-h.x, h.y)).rgb + texture(uSource, uv + vec2(h.x, h.y)).rgb;
        c *= 0.25;
        float luma = dot(c, vec3(0.2126, 0.7152, 0.0722));
        float over = max(luma - uA.y, 0.0);
        outColor = vec4(c * (over / max(luma, 1e-4)), 1.0);
        return;
    }
    if (uMode == 1) {
        float w[5] = float[](0.2270270270, 0.1945945946, 0.1216216216, 0.0540540541, 0.0162162162);
        vec3 c = texture(uSource, uv).rgb * w[0];
        for (int i = 1; i < 5; ++i) {
            c += texture(uSource, uv + uA.zw * float(i)).rgb * w[i];
            c += texture(uSource, uv - uA.zw * float(i)).rgb * w[i];
        }
        outColor = vec4(c, 1.0);
        return;
    }
    if (uMode == 2) {
        // One texel of the metered picture: the scene's mean over its footprint (mipmapped), its luminance out of its
        // coverage, and its weight (centre-weighted as native's histogram: 0 where uncovered).
        vec4 c = textureLod(uSource, uv, uA.y);
        if (c.a <= 0.0) {
            outColor = vec4(0.0);
            return;
        }
        float luminance = dot(c.rgb / c.a, vec3(0.2126, 0.7152, 0.0722));
        float weight = 4.0;
        if (uA.x > 0.0) {
            vec2 d = uv * 2.0 - 1.0;
            float centre = exp(-2.3 * dot(d, d));
            weight = floor(4.0 * mix(1.0, 4.0 * centre, uA.x) + 0.5) + 1.0;
        }
        outColor = vec4(luminance, weight, 0.0, 1.0);
        return;
    }
    if (uMode == 3) {
        // exposure_adapt.comp: the histogram of the metered picture, the mean log2 luminance of the pixels between the
        // low and high shares, and the exposure moved toward what brings it to the key.
        float counts[BINS];
        for (int i = 0; i < BINS; ++i) counts[i] = 0.0;
        ivec2 size = textureSize(uSource, 0);
        for (int y = 0; y < size.y; ++y)
            for (int x = 0; x < size.x; ++x) {
                vec4 m = texelFetch(uSource, ivec2(x, y), 0);
                if (m.y <= 0.0) continue;
                int bin = 0;
                if (m.x > 0.0) bin = int(clamp((log2(m.x) - LOWEST_LOG) * BINS_PER_STOP, 1.0, float(BINS - 1)));
                counts[bin] += m.y;
            }
        vec4 last = texelFetch(uState, ivec2(0), 0);
        float total = 0.0;
        for (int i = 1; i < BINS; ++i) total += counts[i];
        bool reset = uB.w > 0.5;
        float target = reset ? clamp(uC.w, uB.y, uB.z) : last.z;
        if (total > 0.0) {
            float low = total * uA.z, high = total * max(uA.w, uA.z);
            float below = 0.0, sum = 0.0, weight = 0.0;
            for (int i = 1; i < BINS; ++i) {
                float n = counts[i];
                float kept = max(min(below + n, high) - max(below, low), 0.0);
                sum += kept * (LOWEST_LOG + (float(i) + 0.5) / BINS_PER_STOP);
                weight += kept;
                below += n;
            }
            if (weight > 0.0) target = clamp(uB.x - sum / weight + uC.w, uB.y, uB.z);
        }
        float ev = last.x;
        if (reset || isnan(ev) || isinf(ev)) ev = target;
        else ev += (target - ev) * (target > ev ? uC.x : uC.y);
        outColor = vec4(ev, exp2(ev), target, 1.0);
        return;
    }
    // The output (kin_view_output.inc.glsl, mode 0: exposed, tonemapped and encoded for the 8-bit gamma 2D frame).
    vec4 scene = texture(uSource, uv);
    vec3 bloom = uA.x > 0.0 ? texture(uBloom, uv).rgb * uA.x : vec3(0.0);
    vec3 c = scene.rgb + bloom;
    float alpha = clamp(scene.a + dot(bloom, vec3(0.2126, 0.7152, 0.0722)), 0.0, 1.0);
    bool partial = alpha > 0.0 && alpha < 1.0;
    if (partial) c /= alpha;
    float exposure = uA.y * (uA.z > 0.5 ? texelFetch(uState, ivec2(0), 0).y : 1.0);
    c = aces(c * exposure);
    c = alpha > 0.0 ? encode(partial ? c : c / alpha) * alpha : encode(c);
    if (uA.w > 0.5) c += (noise(gl_FragCoord.xy) + noise(gl_FragCoord.xy + vec2(5.0, 3.0)) - 1.0) * (alpha / 255.0);
    outColor = vec4(c, alpha);
}
