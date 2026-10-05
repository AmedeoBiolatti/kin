#version 450 core
// kin GPU backend: the image on its way to the display (Renderer2D::set_color_output).
// A linear scene is exposed, tonemapped and encoded to sRGB; then any scene is
// graded through a LUT strip (size² x size: blue slices, red across, green down,
// read trilinearly), cross-faded to a second, and dithered by half an 8-bit step.
layout(location = 0) out vec4 fColor;
layout(set = 2, binding = 0) uniform sampler2D sScene;
layout(set = 2, binding = 1) uniform sampler2D sLut;
layout(set = 2, binding = 2) uniform sampler2D sLutTo;
layout(set = 3, binding = 0) uniform Output {
    vec4 a; // exposure, tonemap (0 none, 1 Reinhard, 2 ACES), dither, linear scene
    vec4 b; // lut strength, lut mix, lut size, lut_to size
    vec4 c; // has lut, has lut_to
} o;
layout(location = 0) in struct {
    vec4 Color;
    vec2 UV;
} In;

vec3 linear_to_srgb(vec3 c) {
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c));
}

vec3 aces(vec3 x) {
    return (x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14);
}

vec3 lut(sampler2D t, vec3 c, float n) {
    const float b = c.b * (n - 1.0);
    const float b0 = floor(b);
    const float b1 = min(b0 + 1.0, n - 1.0);
    const float x = c.r * (n - 1.0) + 0.5;
    const float y = (c.g * (n - 1.0) + 0.5) / n;
    const vec3 lo = texture(t, vec2((b0 * n + x) / (n * n), y)).rgb;
    const vec3 hi = texture(t, vec2((b1 * n + x) / (n * n), y)).rgb;
    return mix(lo, hi, b - b0);
}

// Interleaved gradient noise (Jimenez): even, cheap, no texture.
float noise(vec2 p) {
    return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}

void main() {
    vec3 c = texture(sScene, In.UV).rgb;
    if (o.a.w > 0.5) {
        c = max(c * o.a.x, vec3(0.0));
        if (o.a.y > 1.5) {
            c = aces(c);
        } else if (o.a.y > 0.5) {
            c = c / (1.0 + c);
        }
        c = linear_to_srgb(clamp(c, 0.0, 1.0));
    }
    if (o.c.x > 0.5) {
        vec3 graded = lut(sLut, clamp(c, 0.0, 1.0), o.b.z);
        if (o.c.y > 0.5) {
            graded = mix(graded, lut(sLutTo, clamp(c, 0.0, 1.0), o.b.w), o.b.y);
        }
        c = mix(c, graded, o.b.x);
    }
    if (o.a.z > 0.5) {
        c += (noise(gl_FragCoord.xy) - 0.5) / 255.0;
    }
    fColor = vec4(clamp(c, 0.0, 1.0), 1.0);
}
