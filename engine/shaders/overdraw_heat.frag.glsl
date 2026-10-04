#version 450 core
// The overdraw view's last pass: the scene's layer counts (red channel, 1/32
// each) as colours: black none, blue 1, green 2, yellow 4, red 8, white 16+.
layout(location = 0) out vec4 fColor;
layout(set = 2, binding = 0) uniform sampler2D sCounts;
layout(location = 0) in struct { vec4 Color; vec2 UV; } In;

void main() {
    const float layers = texture(sCounts, In.UV).r * 32.0;
    const vec3 ramp[6] = vec3[6](vec3(0.0), vec3(0.1, 0.2, 0.9), vec3(0.1, 0.8, 0.2), vec3(0.95, 0.9, 0.1),
                                 vec3(0.95, 0.15, 0.1), vec3(1.0));
    // 0, 1, 2, 4, 8, 16 layers at the ramp's stops (log2 between them).
    const float at = layers < 1.0 ? layers : 1.0 + log2(layers);
    const int i = int(clamp(floor(at), 0.0, 4.0));
    fColor = vec4(mix(ramp[i], ramp[i + 1], clamp(at - float(i), 0.0, 1.0)), 1.0);
}
