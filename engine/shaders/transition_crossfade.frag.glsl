#version 450 core
// Engine transition: true A<->B cross-dissolve. TWO-INPUT:
//   sTexture  (set=2 b=0) = outgoing scene snapshot (A)
//   sTexture2 (set=2 b=1) = incoming scene snapshot (B)
// Params: misc.x = progress (0..1). Output blends A -> B. Opaque.
layout(location = 0) out vec4 fColor;
layout(set = 2, binding = 0) uniform sampler2D sTexture;
layout(set = 2, binding = 1) uniform sampler2D sTexture2;
layout(set = 3, binding = 0) uniform Params {
    vec4 color_a;
    vec4 color_b;
    vec4 misc;    // x=progress
    vec4 _pad;
};
layout(location = 0) in struct { vec4 Color; vec2 UV; } In;

void main() {
    vec3 a = texture(sTexture, In.UV).rgb;
    vec3 b = texture(sTexture2, In.UV).rgb;
    float p = clamp(misc.x, 0.0, 1.0);
    fColor = vec4(mix(a, b, p), 1.0);
}
