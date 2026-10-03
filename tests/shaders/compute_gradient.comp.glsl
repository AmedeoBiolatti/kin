#version 450 core
// Test compute shader for renderer_tests: writes red = x / width, green =
// y / height and blue = a uniform, plus a value from a read-only buffer.
layout(local_size_x = 8, local_size_y = 8) in;
layout(std430, set = 0, binding = 0) readonly buffer Extra { vec4 extra; };
layout(set = 1, binding = 0, rgba8) uniform writeonly image2D outImage;
layout(set = 2, binding = 0) uniform Params { vec4 p; };

void main() {
    const ivec2 xy = ivec2(gl_GlobalInvocationID.xy);
    const ivec2 size = imageSize(outImage);
    if (xy.x >= size.x || xy.y >= size.y) {
        return;
    }
    imageStore(outImage, xy, vec4(float(xy.x) / float(size.x), float(xy.y) / float(size.y), p.x, extra.a));
}
