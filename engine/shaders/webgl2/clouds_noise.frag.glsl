#version 300 es
// Volumetric clouds (Scene3D::clouds) in WebGL2: their tiling noise (native cloud_noise.comp and cloud_weather.comp,
// as fragments), made once: a draw fills one slice of the shape or the detail volume, or the coverage map (one
// kin_cloud_texel a fragment: a shader the browser compiles quickly).
precision highp float;
precision highp int;
#include "kin_cloud_noise.inc.glsl"
uniform int uNoise; // 0 the shape volume, 1 the detail volume, 2 the coverage map
uniform int uSize;  // its texels across
uniform int uSlice; // the slice this draw fills
out vec4 outColor;
void main() {
    outColor = kin_cloud_texel(uint(uNoise), uvec3(uvec2(gl_FragCoord.xy), uint(uSlice)), uint(uSize));
}
