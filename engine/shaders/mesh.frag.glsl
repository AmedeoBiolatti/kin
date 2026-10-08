#version 450 core
// kin's 3D meshes (kin/render3d): a base colour (vertex, instance and
// material colours, times a texture), lit in linear light by a sun (through
// its shadow map), ambient light and up to 16 point lights, plus the
// material's own light; or unlit. Output premultiplied, as kin's targets are.
layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec4 vColor; // linear, straight alpha
layout(location = 2) in vec2 vUV;
layout(location = 3) in vec3 vWorld;

layout(set = 2, binding = 0) uniform sampler2D uBase;         // white when the material has none
layout(set = 2, binding = 1) uniform sampler2DShadow uShadow; // depth from the sun, compared

const int MAX_LIGHTS = 16;
// The frame's, pushed once a pass.
layout(set = 3, binding = 0) uniform Frame {
    vec4 sun_direction; // xyz: toward the sun, length 1
    vec4 sun_color;     // linear
    vec4 ambient;       // linear
    vec4 flags;         // x: encode to sRGB (a gamma target), y: decode the texture, z: shadows, w: point lights
    mat4 shadow_matrix; // world to the shadow map's clip space
    vec4 shadow;        // x: depth bias, y: normal offset (world units), z: a texel (uv)
    vec4 lights[MAX_LIGHTS * 2]; // position.xyz and radius; colour.rgb (linear)
} frame;
// The draw's.
layout(set = 3, binding = 1) uniform Material {
    vec4 params; // x: emissive, y: unlit
} material;

layout(location = 0) out vec4 FragColor;

vec3 decode(vec3 c) {
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c));
}

vec3 encode(vec3 c) {
    c = clamp(c, 0.0, 1.0);
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c));
}

// How much of the sun reaches here: 3x3 comparisons against the shadow map,
// from a point pushed a little off the surface (no self-shadowing acne).
// Reverse Z: the map keeps the depth nearest the sun, the greatest.
float sunlight(vec3 n) {
    if (frame.flags.z < 0.5) {
        return 1.0;
    }
    const vec4 clip = frame.shadow_matrix * vec4(vWorld + n * frame.shadow.y, 1.0);
    const vec3 ndc = clip.xyz / clip.w;
    const vec2 uv = vec2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    if (uv.x < 0.0 || uv.y < 0.0 || uv.x > 1.0 || uv.y > 1.0 || ndc.z < 0.0 || ndc.z > 1.0) {
        return 1.0;
    }
    const float ref = ndc.z + frame.shadow.x;
    float lit = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            lit += texture(uShadow, vec3(uv + vec2(x, y) * frame.shadow.z, ref));
        }
    }
    return lit / 9.0;
}

void main() {
    vec4 tex = texture(uBase, vUV);
    if (frame.flags.y > 0.5) {
        tex.rgb = decode(tex.rgb);
    }
    const vec3 base = vColor.rgb * tex.rgb;
    const float alpha = vColor.a * tex.a;
    vec3 light;
    if (material.params.y > 0.5) {
        light = vec3(1.0);
    } else {
        // Seen from behind (double-sided), the face turns its normal round.
        const vec3 n = normalize(gl_FrontFacing ? vNormal : -vNormal);
        const float sun = max(dot(n, frame.sun_direction.xyz), 0.0);
        light = frame.ambient.rgb + frame.sun_color.rgb * sun * (sun > 0.0 ? sunlight(n) : 1.0);
        const int count = int(frame.flags.w);
        for (int i = 0; i < count; ++i) {
            const vec4 at = frame.lights[2 * i];
            const vec3 to = at.xyz - vWorld;
            const float d2 = dot(to, to);
            // Smoothly to nothing at the radius.
            float k = clamp(1.0 - d2 / (at.w * at.w), 0.0, 1.0);
            k *= k;
            light += frame.lights[2 * i + 1].rgb * k * max(dot(n, to * inversesqrt(max(d2, 1e-8))), 0.0);
        }
    }
    vec3 c = base * (light + vec3(material.params.x));
    if (frame.flags.x > 0.5) {
        c = encode(c);
    }
    FragColor = vec4(c * alpha, alpha);
}
