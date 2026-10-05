#version 450 core
// kin GPU backend: a mask laid over (Renderer2D::push_mask). The content layer
// (premultiplied) times the coverage read from the same pixel of the mask
// layer, as mask_coverage() in kin/renderer/mask.hpp reads it.
layout(location = 0) out vec4 fColor;
layout(set = 2, binding = 0) uniform sampler2D sContent;
layout(set = 2, binding = 1) uniform sampler2D sMask;
layout(set = 3, binding = 0) uniform Mask {
    vec4 params; // x: luminance (else alpha); y: stencil (else alpha); z: threshold; w: inverted
} mask;
layout(location = 0) in struct {
    vec4 Color;
    vec2 UV;
} In;

void main() {
    const vec4 m = texture(sMask, In.UV);
    float k = mask.params.x > 0.5 ? dot(m.rgb, vec3(0.2126, 0.7152, 0.0722)) : m.a;
    if (mask.params.y > 0.5) {
        k = k >= mask.params.z ? 1.0 : 0.0;
    }
    if (mask.params.w > 0.5) {
        k = 1.0 - k;
    }
    fColor = texture(sContent, In.UV) * k * In.Color;
}
