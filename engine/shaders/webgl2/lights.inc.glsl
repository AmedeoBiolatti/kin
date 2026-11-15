// The pass's point and spot lights (WebGL2): rows of a float texture, not uniform arrays, which the fragment budget
// (224 vectors at least) could not hold beside a surface's. Row i (0 to 15) a point light: texel 0 its position and
// radius, 1 its colour and size squared, 2 x its first shadow cube layer (-1: none). Row 16 + i a spot: 0 position and
// radius, 1 colour and size squared, 2 axis and cos(outer), 3 x 1 / (cos inner - cos outer), y its shadow layer.
uniform highp sampler2D uLightData;
uniform int uPointCount, uSpotCount;
vec4 point_light(int i, int texel) {
    return texelFetch(uLightData, ivec2(texel, i), 0);
}
vec4 spot_light(int i, int texel) {
    return texelFetch(uLightData, ivec2(texel, 16 + i), 0);
}
