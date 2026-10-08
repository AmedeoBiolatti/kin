// A mesh instance as the 3D vertex shaders read it, and the buffer of them
// (set 0, binding 0). Mirrors GpuInstance3D in gpu_renderer3d.hpp: change
// them together.
//
// The model matrix, the normals' (its cofactors: they turn normals as the
// model turns the shape, scaled; the first column's w holds the copy's pick
// id, plus one, as bits), and the colour (linear, the material's times the
// tint, straight alpha).
struct Instance {
    mat4 model;
    vec4 normal0;
    vec4 normal1;
    vec4 normal2;
    vec4 color;
};
layout(std430, set = 0, binding = 0) readonly buffer Instances {
    Instance instances[];
};
