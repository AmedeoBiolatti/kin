#version 450 core
// kin's 3D view out to what 2D draws, with the camera's lens, film and grade (CameraEffects3D::lens/film/grade): the
// variant of view_output.frag (kin_view_output.inc.glsl, whose steps it keeps) used only when one of them is on, so a
// view without them is drawn exactly as before. In order: the scene and its bloom, read with red and blue pulled apart
// toward the edges (chromatic aberration); the vignette; the exposure (manual, times the adapted one with auto
// exposure); white balance, contrast and saturation in linear light; the tone curve (ACES, AgX or Khronos PBR
// Neutral); the sRGB encode; the look (a LUT strip, cross-faded to a second); the grain and the dither, in the 8-bit
// steps. Premultiplied in and out, a partly covered pixel graded as its own colour (as view_output does).
layout(location = 0) in vec2 vUV;
layout(set = 2, binding = 0) uniform sampler2D uScene;
layout(set = 2, binding = 1) uniform sampler2D uBloom;
layout(set = 2, binding = 2) uniform sampler2D uLut;
layout(set = 2, binding = 3) uniform sampler2D uLutTo;
layout(std430, set = 2, binding = 4) readonly buffer Exposure {
    uint bins[128];
    float ev;
    float exposure;
} adapted;
layout(set = 3, binding = 0) uniform Params {
    vec4 a;          // x: bloom strength (0: none), y: exposure, z: mode (as view_output's), w: 0
    vec4 flags;      // x: 1 to multiply by the adapted exposure, y: 1 to dither, z: tone curve (0 ACES, 1 AgX,
                     // 2 Neutral), w: the render's number (the grain moves with it)
    vec4 lens;       // x: vignette, y: its smoothness, z: its roundness, w: chromatic aberration (of the height)
    vec4 film;       // x: grain, y: its size (pixels), z: the view's width over height, w: LUTs (0, 1, 2)
    vec4 balance[3]; // the white balance's rows (linear sRGB to linear sRGB)
    vec4 grade;      // x: contrast, y: saturation, z: LUT strength, w: the mix toward the second LUT
    vec4 lut_size;   // x: the first LUT's size, y: the second's
} params;
layout(location = 0) out vec4 FragColor;

const vec3 luma_weights = vec3(0.2126, 0.7152, 0.0722);

vec3 aces(vec3 x) {
    return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}

// AgX's base curve (Troy Sobotka's; the polynomial fit and matrices as in Blender's and three.js' ports): into a log
// space between -12.47 and +4.03 stops, a sigmoid, back to linear sRGB.
vec3 agx(vec3 c) {
    const mat3 inset = mat3(0.842479062253094, 0.0423282422610123, 0.0423756549057051, 0.0784335999999992,
                            0.878468636469772, 0.0784336, 0.0792237451477643, 0.0791661274605434, 0.879142973793104);
    const mat3 outset = mat3(1.19687900512017, -0.0528968517574562, -0.0529716355144438, -0.0980208811401368,
                             1.15190312990417, -0.0980434501171241, -0.0990297440797205, -0.0989611768448433,
                             1.15107367264116);
    const float low = -12.47393, high = 4.026069;
    vec3 x = clamp(log2(max(inset * c, vec3(1e-10))), low, high);
    x = (x - low) / (high - low);
    const vec3 x2 = x * x, x4 = x2 * x2;
    x = 15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4 - 6.868 * x2 * x + 0.4298 * x2 + 0.1191 * x - 0.00232;
    return clamp(pow(max(outset * x, vec3(0.0)), vec3(2.2)), 0.0, 1.0);
}

// Khronos PBR Neutral (2024).
vec3 neutral(vec3 c) {
    const float start = 0.76, desaturation = 0.15;
    const float low = min(c.r, min(c.g, c.b));
    c -= low < 0.08 ? low - 6.25 * low * low : 0.04;
    const float peak = max(c.r, max(c.g, c.b));
    if (peak < start) {
        return clamp(c, 0.0, 1.0);
    }
    const float d = 1.0 - start;
    const float new_peak = 1.0 - d * d / (peak + d - start);
    c *= new_peak / peak;
    const float g = 1.0 - 1.0 / (desaturation * (peak - new_peak) + 1.0);
    return clamp(mix(c, vec3(new_peak), g), 0.0, 1.0);
}

vec3 encode(vec3 c) {
    c = clamp(c, 0.0, 1.0);
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c));
}

vec3 decode(vec3 c) {
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c));
}

// A LUT strip (size² x size: blue slices left to right, red across each, green down), read trilinearly, as 2D's.
vec3 look(sampler2D t, vec3 c, float n) {
    const float b = c.b * (n - 1.0);
    const float b0 = floor(b);
    const float b1 = min(b0 + 1.0, n - 1.0);
    const float x = c.r * (n - 1.0) + 0.5;
    const float y = (c.g * (n - 1.0) + 0.5) / n;
    const vec3 lo = texture(t, vec2((b0 * n + x) / (n * n), y)).rgb;
    const vec3 hi = texture(t, vec2((b1 * n + x) / (n * n), y)).rgb;
    return mix(lo, hi, b - b0);
}

float noise(vec2 p) {
    return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}

// A hash of a cell and the render's number (PCG), 0 to 1: the grain, different each render.
float grain_noise(uvec2 cell, uint frame) {
    uint v = cell.x * 1973u + cell.y * 9277u + frame * 26699u;
    v = v * 747796405u + 2891336453u;
    v = ((v >> ((v >> 28u) + 4u)) ^ v) * 277803737u;
    return float((v >> 22u) ^ v) / 4294967295.0;
}

void main() {
    // Chromatic aberration: the red image larger and the blue smaller, about the middle (red read nearer the middle,
    // blue farther out), so red fringes the outer side of an edge and blue the inner.
    const vec2 from_middle = vUV - 0.5;
    const vec2 split = from_middle * vec2(params.film.z, 1.0) * params.lens.w /
                       max(length(vec2(0.5 * params.film.z, 0.5)), 1e-4) * vec2(1.0 / params.film.z, 1.0);
    vec4 scene = texture(uScene, vUV);
    vec3 bloom = params.a.x > 0.0 ? texture(uBloom, vUV).rgb * params.a.x : vec3(0.0);
    if (params.lens.w > 0.0) {
        scene.r = texture(uScene, vUV - split).r;
        scene.b = texture(uScene, vUV + split).b;
        if (params.a.x > 0.0) {
            bloom.r = texture(uBloom, vUV - split).r * params.a.x;
            bloom.b = texture(uBloom, vUV + split).b * params.a.x;
        }
    }
    vec3 c = scene.rgb + bloom;
    const float alpha = clamp(scene.a + dot(bloom, luma_weights), 0.0, 1.0);
    // A partly covered pixel is graded as its own colour and given its coverage again (view_output's partial pixel and
    // its 8-bit encode of c / alpha); an HDR 2D pipeline (mode 2) gets it as it is.
    const bool straight = (params.a.z < 1.5 || params.a.z > 2.5) && alpha > 0.0 && alpha < 1.0;
    if (straight) {
        c /= alpha;
    }
    // The vignette: 0 in the middle to 1 at the corners, round (as wide as the view is high) or the rectangle's.
    if (params.lens.x > 0.0) {
        const vec2 round_shape = from_middle * vec2(params.film.z, 1.0) / length(vec2(0.5 * params.film.z, 0.5));
        const vec2 rectangle = from_middle / length(vec2(0.5, 0.5));
        const float r = length(mix(rectangle, round_shape, params.lens.z));
        c *= 1.0 - params.lens.x * smoothstep(1.0 - params.lens.y, 1.0 + 0.25 * params.lens.y, r);
    }
    if (params.a.z < 2.5) {
        // Exposed (not in the 8-bit TAA mode, whose picture is already the view's light as shown).
        c *= params.a.y * (params.flags.x > 0.5 ? adapted.exposure : 1.0);
    }
    c = vec3(dot(params.balance[0].xyz, c), dot(params.balance[1].xyz, c), dot(params.balance[2].xyz, c));
    c = max(c, vec3(0.0));
    if (params.grade.x != 1.0) {
        c = 0.18 * pow(c / 0.18 + 1e-6, vec3(params.grade.x)); // contrast round mid-grey, in stops
    }
    const float luma = dot(c, luma_weights);
    c = max(mix(vec3(luma), c, params.grade.y), vec3(0.0));
    // 0 tonemaps and encodes, 1 tonemaps (an sRGB target encodes), 2 neither (an HDR 2D pipeline), 3 encodes only.
    if (params.a.z < 1.5) {
        c = params.flags.z > 1.5 ? neutral(c) : params.flags.z > 0.5 ? agx(c) : aces(c);
    }
    if (params.a.z < 1.5 || params.a.z > 2.5) {
        vec3 shown = encode(c);
        if (params.film.w > 0.5) {
            vec3 graded = look(uLut, shown, params.lut_size.x);
            if (params.film.w > 1.5) {
                graded = mix(graded, look(uLutTo, shown, params.lut_size.y), params.grade.w);
            }
            shown = mix(shown, graded, params.grade.z);
        }
        if (params.film.x > 0.0) {
            // Strongest in the mid-tones, none in black or white, as film's.
            const float tone = dot(shown, luma_weights);
            const uvec2 cell = uvec2(gl_FragCoord.xy / max(params.film.y, 1.0));
            shown += (grain_noise(cell, uint(params.flags.w)) - 0.5) * params.film.x * tone * (1.0 - tone);
        }
        if (params.flags.y > 0.5 && (params.a.z < 0.5 || params.a.z > 2.5)) {
            shown += (noise(gl_FragCoord.xy) + noise(gl_FragCoord.xy + vec2(5.0, 3.0)) - 1.0) * (alpha / 255.0);
        }
        shown = clamp(shown, 0.0, 1.0);
        // Mode 1 hands linear light to an sRGB target, which encodes it.
        c = params.a.z > 0.5 && params.a.z < 1.5 ? decode(shown) : shown;
    }
    if (straight) {
        c *= alpha;
    }
    FragColor = vec4(c, alpha);
}
