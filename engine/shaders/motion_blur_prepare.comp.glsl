#version 450 core
// Motion blur (MotionBlur3D), first of four: each output pixel's smear and distance, for the tiles and the gather.
// The velocity is the one the temporal resolve took (the nearest surface's round the pixel, unjittered, in UV a
// frame: kin_velocity), read at the history's density. The smear is half of it scaled by the shutter (a radius in
// output pixels, the pixel at its middle), at most `blur.y` long. With camera_scale under 1 the camera's own part
// (where the pixel's point was on last frame's screen, had only the camera moved) is taken out by that much.
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0) uniform usampler2D uDilated; // pack_dilated (kin_taa.inc.glsl), history-sized
layout(set = 0, binding = 1) uniform sampler2D uDepth;    // the main pass's depth (reverse Z), shading-sized
layout(set = 1, binding = 0, rgba16f) uniform writeonly image2D uPrepared; // xy: smear (pixels), z: distance, w: length
layout(set = 2, binding = 0) uniform Params {
    mat4 inverse_now; // this frame's clip to world (no jitter)
    mat4 before;      // world to last frame's clip
    vec4 size;        // the output's extent and reciprocals
    vec4 blur;        // x: UV a frame to radius pixels (with size), y: longest radius, z: camera's share, w: 1 to separate it
    vec4 depth;       // x: 1 when uDepth holds the depth, y: where the overlay's slice of it starts (2: none)
    vec4 unproject_zw; // e, f, g, h: a view-space z of a depth is (e depth + f) / (g depth + h)
}
params;

vec2 dilated_velocity(uint packed) {
    return unpackHalf2x16(packed & ~0x00030003u);
}

void main() {
    const ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    if (any(greaterThanEqual(p, ivec2(params.size.xy)))) {
        return;
    }
    const vec2 uv = (vec2(p) + 0.5) * params.size.zw;
    const ivec2 history = textureSize(uDilated, 0);
    vec2 velocity = dilated_velocity(texelFetch(uDilated, min(ivec2(uv * vec2(history)), history - 1), 0).r);
    // A copy with no past (kin_velocity's sentinel, far off the screen) has no known motion.
    if (any(greaterThan(abs(velocity), vec2(2.0))) || any(isnan(velocity))) {
        velocity = vec2(0.0);
    }
    float distance = 1.0;
    if (params.depth.x > 0.5) {
        float d = textureLod(uDepth, uv, 0.0).r;
        const bool overlay = params.depth.y <= 1.0 && d >= params.depth.y;
        if (overlay) {
            distance = 0.0; // in front of everything, and moving with the camera: its own motion stays
        } else {
            d = params.depth.y <= 1.0 ? d / params.depth.y : d;
            const float z = -(params.unproject_zw.x * d + params.unproject_zw.y) /
                            (params.unproject_zw.z * d + params.unproject_zw.w);
            distance = z > 0.0 && z < 60000.0 ? z : 60000.0; // the sky (or past a half's range): far
            if (params.blur.w > 0.5) {
                // Homogeneous all the way: the sky (depth 0, a point at infinity under an infinite far plane) turns
                // with the camera and does not move with it.
                const vec2 ndc = vec2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
                const vec4 world = params.inverse_now * vec4(ndc, d, 1.0);
                const vec4 then = params.before * world;
                if (then.w > 1e-6) {
                    const vec2 camera = (ndc - then.xy / then.w) * vec2(0.5, -0.5);
                    velocity -= camera * (1.0 - params.blur.z);
                }
            }
        }
    }
    vec2 smear = velocity * params.size.xy * params.blur.x;
    const float length_now = length(smear);
    if (length_now > params.blur.y) {
        smear *= params.blur.y / length_now;
    }
    imageStore(uPrepared, p, vec4(smear, distance, min(length_now, params.blur.y)));
}
