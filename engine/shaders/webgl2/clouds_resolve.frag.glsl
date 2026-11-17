#version 300 es
// Volumetric clouds in WebGL2: native clouds_resolve.comp as a fragment: this frame's walk blended with the last blend
// where its clouds were then, held within this frame's 3 x 3 neighbourhood; out0 the blend, out1 r its distance.
precision highp float;
precision highp int;
uniform highp sampler2D uCurrent, uCurrentDistance, uHistory, uHistoryDistance;
uniform mat4 uToWorld, uPrevious;
uniform vec3 uEye;
uniform vec4 uSize; // xy: the texels, z: the history's share, w: 1 with a history
uniform bool uOrthographic;
layout(location = 0) out vec4 out0;
layout(location = 1) out vec4 out1;
void main() {
    ivec2 xy = ivec2(gl_FragCoord.xy);
    ivec2 size = ivec2(uSize.xy);
    vec4 current = texelFetch(uCurrent, xy, 0);
    float dist = texelFetch(uCurrentDistance, xy, 0).r;
    out0 = current;
    out1 = vec4(dist, 0.0, 0.0, 1.0);
    if (uSize.w < 0.5) return;
    vec4 lo = current, hi = current;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x) {
            vec4 n = texelFetch(uCurrent, clamp(xy + ivec2(x, y), ivec2(0), size - 1), 0);
            lo = min(lo, n);
            hi = max(hi, n);
        }
    vec2 ndc = gl_FragCoord.xy / uSize.xy * 2.0 - 1.0;
    vec4 far_point = uToWorld * vec4(ndc, 0.0, 1.0);
    vec3 dir = normalize(far_point.xyz - uEye * far_point.w);
    vec4 before = dist > 0.0 ? uPrevious * vec4(uEye + dir * dist, 1.0) : uPrevious * vec4(dir, 0.0);
    if (uOrthographic) {
        vec4 near_point = uToWorld * vec4(ndc, 1.0, 1.0);
        vec3 from = near_point.xyz / near_point.w;
        before = uPrevious * vec4(from + normalize(far_point.xyz / far_point.w - from) * dist, 1.0);
    }
    if (before.w <= 0.0) return;
    vec2 then = before.xy / before.w * 0.5 + 0.5;
    if (any(lessThan(then, vec2(0.0))) || any(greaterThan(then, vec2(1.0)))) return;
    out0 = mix(current, clamp(textureLod(uHistory, then, 0.0), lo, hi), uSize.z);
    float d = texelFetch(uHistoryDistance, clamp(ivec2(then * uSize.xy), ivec2(0), size - 1), 0).r;
    if (dist > 0.0 && d > 0.0) out1 = vec4(mix(dist, d, uSize.z), 0.0, 0.0, 1.0);
}
