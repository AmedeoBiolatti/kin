// Stock unlit shading, preserving colour conversion, emission and premultiplied output.
// Separate masked variants keep discard out of opaque and blended pipelines.
layout(location = 1) in vec4 vColor;
#ifdef KIN_UNLIT_TEXTURED
layout(location = 2) in vec2 vUV;
layout(set = 2, binding = 0) uniform sampler2D uBase;
#endif
layout(location = 0) out vec4 FragColor;
#include "kin_material_uniforms.inc.glsl"
#include "kin_srgb.inc.glsl"
void main() {
    vec4 color = vColor;
#ifdef KIN_UNLIT_TEXTURED
    vec4 texel = texture(uBase, vUV);
    if (frame.flags.y > 0.5) {
        texel.rgb = decode(texel.rgb);
    }
    color = vec4(vColor.rgb * texel.rgb, vColor.a * texel.a);
#endif
    float alpha = color.a;
#ifdef KIN_UNLIT_MASK
    if (alpha < material.params.w) {
        discard;
    }
    alpha = 1.0;
#endif
    vec3 c = color.rgb * (vec3(1.0) + vec3(material.params.x));
    if (frame.flags.x > 0.5) {
        c = encode(c);
    }
    FragColor = vec4(c * alpha, alpha);
}
