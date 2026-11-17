// std430 records shared by the composition gather, keys and graphics shaders.
struct Particle { vec4 position_size, colour, axis_rotation, frame; };
struct ParticleLook {
    vec4 axis, atlas, stretch, tint, disc, flags;
    uvec4 range; // first, end, compatible history, GPU source
    vec4 right, up, eye, forward; // this source's previous view camera
};
