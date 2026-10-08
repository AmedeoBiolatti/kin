#version 450 core
// kin's 3D meshes (kin/render3d): lit by a sun and ambient light, plus the
// material's own (emissive), in linear light; or unlit.
layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec4 vColor; // linear, straight alpha
layout(location = 2) in vec2 vUV;

layout(set = 3, binding = 0) uniform Material {
    vec4 sun_direction; // xyz: toward the sun, length 1
    vec4 sun_color;     // linear
    vec4 ambient;       // linear
    vec4 params;        // x: emissive, y: unlit (1), z: encode to sRGB (a gamma target, 1)
} material;

layout(location = 0) out vec4 FragColor;

vec3 encode(vec3 c) {
    c = clamp(c, 0.0, 1.0);
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c));
}

void main() {
    vec3 base = vColor.rgb;
    vec3 light;
    if (material.params.y > 0.5) {
        light = vec3(1.0);
    } else {
        // Seen from behind (double-sided), the face turns its normal round.
        vec3 n = normalize(gl_FrontFacing ? vNormal : -vNormal);
        light = material.ambient.rgb + material.sun_color.rgb * max(dot(n, material.sun_direction.xyz), 0.0);
    }
    vec3 c = base * (light + vec3(material.params.x));
    if (material.params.z > 0.5) {
        c = encode(c);
    }
    // Premultiplied, as kin's render targets store colour.
    FragColor = vec4(c * vColor.a, vColor.a);
}
