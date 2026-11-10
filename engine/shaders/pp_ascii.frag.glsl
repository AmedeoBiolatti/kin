#version 450 core
// GPU-only ASCII glyph conversion. Params: (columns, rows, coloured, intensity).
// A compact 5x7 glyph table; no readback, font texture or per-cell CPU work.
layout(location = 0) out vec4 fColor;
layout(set = 2, binding = 0) uniform sampler2D sTexture;
layout(set = 3, binding = 0) uniform Params { vec4 grid; vec4 unused_b; vec4 unused_c; vec4 unused_d; };
layout(location = 0) in struct { vec4 Color; vec2 UV; } In;
const uint glyphs[9] = uint[9](0u, 0x66000u, 0x06060u, 0x00F00u, 0x06F60u, 0x96F69u, 0xAFAFAu, 0x98429u, 0x7AB96u);
void main() {
    vec2 cells = max(grid.xy, vec2(1.0));
    vec2 cell = floor(In.UV * cells);
    vec3 colour = texture(sTexture, (cell + 0.5) / cells).rgb;
    float luminance = dot(colour, vec3(0.2126, 0.7152, 0.0722));
    uint glyph = uint(clamp(floor(luminance * 8.999), 0.0, 8.0));
    // 4x5 strokes in a 5x7 cell, with a clear column and two clear rows.
    ivec2 pixel = ivec2(floor(fract(In.UV * cells) * vec2(5,7)));
    float ink = pixel.x < 4 && pixel.y < 5 ? float((glyphs[glyph] >> uint(pixel.y * 4 + pixel.x)) & 1u) : 0.0;
    vec3 tint = grid.z > 0.5 ? colour : vec3(1.0);
    fColor = vec4(tint * ink * max(grid.w, 0.0), 1.0);
}
