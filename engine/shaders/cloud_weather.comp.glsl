#version 450 core
// The clouds' default coverage map (Clouds3D::weather_map unset), made once: r the coverage, g the kind, tiling, a plane
// through a tiling volume of Perlin noise. clouds3d.cpp's noise_texel(Noise::Weather): change them together.
layout(local_size_x = 8, local_size_y = 8) in;

layout(set = 1, binding = 0, rgba8) uniform writeonly image2D uOut;

layout(std140, set = 2, binding = 0) uniform Params {
    uvec4 size; // x: the texels across
} params;

#include "kin_cloud_noise.inc.glsl"

void main() {
    const uvec2 at = gl_GlobalInvocationID.xy;
    const uint size = params.size.x;
    if (at.x >= size || at.y >= size) {
        return;
    }
    imageStore(uOut, ivec2(at), kin_cloud_texel(2u, uvec3(at, 0u), size));
}
