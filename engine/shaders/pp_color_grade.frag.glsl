#version 450 core
// Engine post-fx: color grade (brightness / contrast / saturation / tint). Single-input.
// Params: color_a = (brightness, contrast, saturation, _); color_b = tint rgb, a = tint strength.
layout(location = 0) out vec4 fColor;
layout(set = 2, binding = 0) uniform sampler2D sTexture;
layout(set = 3, binding = 0) uniform Params {
    vec4 color_a; // x=brightness y=contrast z=saturation
    vec4 color_b; // rgb=tint a=tint strength
    vec4 misc;
    vec4 _pad;
};
layout(location = 0) in struct { vec4 Color; vec2 UV; } In;

void main() {
    vec3 c = texture(sTexture, In.UV).rgb;
    float brightness = color_a.x > 0.0 ? color_a.x : 1.0;
    float contrast   = color_a.y > 0.0 ? color_a.y : 1.0;
    float saturation = color_a.z >= 0.0 ? color_a.z : 1.0;

    c *= brightness;
    c = (c - 0.5) * contrast + 0.5;                      // contrast about mid-gray
    float luma = dot(c, vec3(0.299, 0.587, 0.114));
    c = mix(vec3(luma), c, saturation);                  // saturation
    c = mix(c, c * color_b.rgb, clamp(color_b.a, 0.0, 1.0)); // tint
    fColor = vec4(clamp(c, 0.0, 1.0), 1.0);
}
