// Shared LocalLightUniforms ABI; the caller chooses KIN_LIGHTS_BINDING.
const int MAX_SPOTS = 16;
const int MAX_LOCAL = 20; // layers: 8 spots' maps, then two cubes of six faces
layout(set = 3, binding = KIN_LIGHTS_BINDING) uniform LocalLights {
    vec4 counts;      // x: spot lights, y: the moon's cascades (0: no shadow), z: the moon's shadow strength, w: the sun's
    vec4 local;       // x: 1 / the local array's side (a texel in uv)
    vec4 moon;        // x: a texel of the moon's map (uv)
    mat4 moon_matrix[4];
    vec4 moon_bias[4]; // per cascade: x: depth bias, y: normal offset (world units)
    vec4 moon_end;     // the view depth where each cascade ends
    vec4 moon_blend;   // and where it starts to fade into the next
    vec4 spots[MAX_SPOTS * 4]; // per spot: position.xyz radius; colour.rgb size^2; direction.xyz cos(outer); x: 1 / (cos inner - cos outer), y: its layer (-1: no shadow)
    vec4 point_shadow[MAX_LIGHTS]; // per point light (as frame.lights): x: its cube's first layer (-1: no shadow)
    mat4 local_matrix[MAX_LOCAL];  // world to the layer's clip space
    vec4 local_a[MAX_LOCAL];       // x: the map's share of the array's side, y: a texel's world size per unit of depth, z: near, w: far
    vec4 local_b[MAX_LOCAL];       // x: bias (texels), y: normal offset (texels), z: filter spacing (texels), w: where the shadow has faded to nothing (distance)
} locals;
