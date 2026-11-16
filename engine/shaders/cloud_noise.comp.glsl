#version 450 core
// The clouds' tiling noise (Scene3D::clouds, kin/render3d/sky3d.hpp's cloud_model), made once: the shape volume
// (r Perlin-Worley, g b a Worley of 4, 8, 16 cells) or the detail volume (r g b Worley of 2, 4, 8 cells); the coverage
// map is cloud_weather.comp's. The same arithmetic as clouds3d.cpp's noise_texel: change them together.
layout(local_size_x = 4, local_size_y = 4, local_size_z = 4) in;

layout(set = 1, binding = 0, rgba8) uniform writeonly image3D uOut;

layout(std140, set = 2, binding = 0) uniform Params {
    uvec4 noise; // x: 0 shape, 1 detail; y: the texels across (z: deep)
} params;

#include "kin_cloud_noise.inc.glsl"

void main() {
    const uvec3 at = gl_GlobalInvocationID;
    const uint size = params.noise.y;
    const uint deep = params.noise.z;
    if (at.x >= size || at.y >= size || at.z >= deep) {
        return;
    }
    imageStore(uOut, ivec3(at), kin_cloud_texel(params.noise.x, at, size));
}
