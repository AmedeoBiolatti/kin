// Shared FrameUniforms ABI for material and shadowed particle fragments.
#ifndef KIN_FRAME_BINDING
#define KIN_FRAME_BINDING 0
#endif
const int MAX_LIGHTS = 16;
// The frame's, pushed once a pass.
layout(set = 3, binding = KIN_FRAME_BINDING) uniform Frame {
    vec4 sun_direction; // xyz: toward the sun, length 1
    vec4 sun_color;     // linear
    vec4 ambient;       // linear: from above
    vec4 ground_ambient; // linear: from below (the same as ambient: even all round)
    vec4 fill_direction; // a second sun, without shadows
    vec4 fill_color;
    vec4 flags;         // x: encode to sRGB (a gamma target), y: decode the texture, z: shadows, w: point lights
    vec4 eye;           // xyz: the camera, in world space
    vec4 environment;   // x: intensity, y: the specular's last level, w: 1 with an environment (else the hemisphere)
    mat4 environment_matrix; // world to the environment's space
    mat4 shadow_matrix[4]; // world to the shadow map's clip space, a cascade each
    vec4 shadow;        // x: cascades, y: receiver-plane PCF correction, z: a texel (uv)
    vec4 cascade_bias[4]; // per cascade: x: depth bias, y: normal offset (world units)
    vec4 cascade_end;   // the view depth where each cascade ends
    vec4 cascade_blend; // and where it starts to fade into the next
    vec4 cascade_forward; // xyz: the camera's forward, to measure view depth along
    vec4 occlusion;     // x: 1 with ambient occlusion, y: 1 with contact shadows, zw: 1 / the view's size
    vec4 lights[MAX_LIGHTS * 2]; // position.xyz and radius; colour.rgb (linear)
    // Global illumination's probes (kin_gi.inc.glsl's KinGiProbes; G5b): gi_counts.w is 1 when they light the scene.
    vec4 gi_origin;
    vec4 gi_counts;
    vec4 gi_bias;
} frame;
