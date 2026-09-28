#version 450 core
// Engine post-fx: bloom combine (additive). TWO-INPUT:
//   sTexture  (set=2 b=0) = blurred bright-pass (the previous chain result)
//   sTexture2 (set=2 b=1) = the ORIGINAL scene (pass.sample_original = true)
// Params: color_a = (intensity, _, _, _).
layout(location = 0) out vec4 fColor;
layout(set = 2, binding = 0) uniform sampler2D sTexture;
layout(set = 2, binding = 1) uniform sampler2D sTexture2;
layout(set = 3, binding = 0) uniform Params {
    vec4 color_a; // x=intensity
    vec4 color_b;
    vec4 misc;
    vec4 _pad;
};
layout(location = 0) in struct { vec4 Color; vec2 UV; } In;

void main() {
    vec3 orig  = texture(sTexture2, In.UV).rgb;
    vec3 bloom = texture(sTexture, In.UV).rgb;
    float intensity = color_a.x > 0.0 ? color_a.x : 1.0;
    fColor = vec4(orig + bloom * intensity, 1.0);
}
