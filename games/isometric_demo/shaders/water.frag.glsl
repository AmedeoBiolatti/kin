#version 450 core

layout(location = 0) out vec4 fColor;

layout(set = 2, binding = 0) uniform sampler2D sTexture;

layout(set = 3, binding = 0) uniform Params {
    vec4 color_a;
    vec4 color_b;
    vec4 misc; // x = time, y = cell salt
    vec4 _pad;
};

layout(location = 0) in struct {
    vec4 Color;
    vec2 UV;
} In;

void main() {
    const vec2 uv = In.UV;
    const vec4 tex = texture(sTexture, uv);
    const float diamond = abs(uv.x - 0.5) * 2.0 + abs(uv.y - 0.5) * 2.0;
    const float alpha = (1.0 - smoothstep(0.92, 1.0, diamond)) * color_a.a * tex.a;
    if (alpha <= 0.01) {
        discard;
    }

    const float t = misc.x;
    const float salt = misc.y;
    const float wave_a = sin((uv.x * 9.0 + uv.y * 4.0 + t * 1.8 + salt) * 6.28318);
    const float wave_b = sin((uv.x * -5.0 + uv.y * 11.0 + t * 1.1 + salt * 1.7) * 6.28318);
    const float ripple = wave_a * 0.5 + wave_b * 0.5;
    const float shimmer = smoothstep(0.58, 1.0, ripple);
    const float depth = clamp(uv.y * 0.65 + uv.x * 0.18, 0.0, 1.0);

    vec3 rgb = mix(color_a.rgb, color_b.rgb, depth);
    rgb += shimmer * vec3(0.16, 0.22, 0.24);
    rgb -= diamond * 0.05;

    fColor = vec4(clamp(rgb, 0.0, 1.0), alpha);
}
