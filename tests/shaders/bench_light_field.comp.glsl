#version 450 core
// kin_draw_bench: a light field, the sum of each light's smooth falloff at
// every texel (lights: x, y, radius, intensity).
layout(local_size_x = 8, local_size_y = 8) in;
layout(std430, set = 0, binding = 0) readonly buffer Lights { vec4 lights[]; };
layout(set = 1, binding = 0, r32f) uniform writeonly image2D field;
layout(set = 2, binding = 0) uniform Params { vec4 count; };

void main() {
    const ivec2 xy = ivec2(gl_GlobalInvocationID.xy);
    const ivec2 size = imageSize(field);
    if (xy.x >= size.x || xy.y >= size.y) {
        return;
    }
    float sum = 0.0;
    for (int i = 0; i < int(count.x); ++i) {
        const vec4 l = lights[i];
        const float d = length(vec2(xy) - l.xy) / l.z;
        const float f = max(0.0, 1.0 - d * d);
        sum += f * f * l.w;
    }
    imageStore(field, xy, vec4(sum));
}
