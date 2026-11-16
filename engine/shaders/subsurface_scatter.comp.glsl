#version 450 core
// Subsurface scattering (Material3D::subsurface), the spread: the light the subsurface pass wrote (the scattering
// share of each subsurface pixel's diffuse light, its profile's number + 1 in alpha) spread as Burley's normalised
// diffusion profile spreads it under the surface, R(r) = (e^(-r/d) + e^(-r/3d)) / (8 pi d r), d a channel's radius.
// As Unity HDRP's and Unreal 5's: samples on a disc round the pixel, their radii importance-sampled from the profile of
// the widest channel (params.radii: for d = 1, from its distribution F(r) = 1 - e^(-r)/4 - 3 e^(-r/3)/4), turned by a
// per-pixel noise (and per frame under temporal anti-aliasing, which averages it), projected to pixels by the depth.
// Each sample counts per channel by the channel's profile at its distance, sqrt(r^2 + dz^2) (r the radius it was drawn
// at, dz how far it lies in front or behind: a depth gap weighs nothing), over the density it was drawn with (as
// HDRP: not the distance between the pixels it lands on, which is 0 for a sample rounded onto the pixel itself, where
// the profile's 1 / r has no bound); samples of another profile, or off the material, count nothing; normalised per
// channel, so the light is kept, only moved.
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0) uniform sampler2D uLight; // rgb: the light to spread; a: the profile's number + 1 (0: none)
layout(set = 0, binding = 1) uniform sampler2D uDepth; // the pre-pass's depth (reverse Z)
layout(set = 1, binding = 0, rgba16f) uniform writeonly image2D uScattered;
layout(set = 2, binding = 0) uniform Params {
    vec4 unproject_xy; // the inverse projection's a, b, c, d (see gtao.comp)
    vec4 unproject_zw; // e, f, g, h
    vec4 size;         // xy: the extent, zw: its reciprocals
    vec4 shape;        // x: samples (up to 32), y: this frame's turn (radians), z: pixels a world unit spans at a
                       // distance of 1 (orthographic: at any), w: 1 for an orthographic camera
    vec4 limits;       // x: the widest reach in pixels
    vec4 profiles[16]; // rgb: each channel's radius d (world units), w: the widest
    vec4 radii[8];     // the samples' radii for d = 1, four a vec4
    vec4 density[8];   // and the profile there for d = 1 ((e^-r + e^-r/3) / r: the density they were drawn with)
}
params;

const float pi = 3.14159265;
const float golden_angle = 2.39996323;

float noise(vec2 p) {
    return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}

vec3 view_position(ivec2 p) {
    const float z = texelFetch(uDepth, p, 0).r;
    const vec2 ndc = vec2((float(p.x) + 0.5) * params.size.z * 2.0 - 1.0, 1.0 - (float(p.y) + 0.5) * params.size.w * 2.0);
    const float w = params.unproject_zw.z * z + params.unproject_zw.w;
    return vec3(params.unproject_xy.x * ndc.x + params.unproject_xy.z, params.unproject_xy.y * ndc.y + params.unproject_xy.w,
                params.unproject_zw.x * z + params.unproject_zw.y) /
           w;
}

// Burley's profile (without its 1 / (8 pi), which the normalisation drops) at `r` for a radius `d` a channel.
vec3 burley(float r, vec3 d) {
    const vec3 dd = max(d, vec3(1e-6));
    return (exp(-r / dd) + exp(-r / (3.0 * dd))) / (dd * max(r, 1e-6));
}

void main() {
    const ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    const ivec2 extent = ivec2(params.size.xy);
    if (any(greaterThanEqual(p, extent))) {
        return;
    }
    const vec4 own = texelFetch(uLight, p, 0);
    if (own.a < 0.5) {
        imageStore(uScattered, p, vec4(0.0));
        return;
    }
    const vec4 profile = params.profiles[clamp(int(own.a - 0.5), 0, 15)];
    const vec3 here = view_position(p);
    const float distance = max(-here.z, 1e-4);
    const float pixels_a_unit = params.shape.w > 0.5 ? params.shape.z : params.shape.z / distance;
    // Too far to spread over a pixel: as it is.
    if (profile.w * pixels_a_unit < 0.25) {
        imageStore(uScattered, p, vec4(own.rgb, 1.0));
        return;
    }
    // As many samples as the spread is wide on the screen (four for a pixel or two, up to the most at eight or more):
    // they are drawn from the same sequence, so fewer are its first ones, still spread over the whole disc.
    const int samples = int(clamp(ceil(profile.w * pixels_a_unit * 4.0), 4.0, params.shape.x));
    const float turn = noise(vec2(p)) * 2.0 * pi + params.shape.y;
    vec3 sum = vec3(0.0), weights = vec3(0.0);
    for (int i = 0; i < samples; ++i) {
        const int pick = samples < int(params.shape.x) ? (i * int(params.shape.x)) / samples : i;
        const float r = params.radii[pick >> 2][pick & 3] * profile.w; // world units
        const float reach = min(r * pixels_a_unit, params.limits.x);
        const float angle = turn + float(i) * golden_angle;
        const ivec2 q = p + ivec2(round(vec2(cos(angle), sin(angle)) * reach));
        if (any(lessThan(q, ivec2(0))) || any(greaterThanEqual(q, extent))) {
            continue;
        }
        const vec4 there = texelFetch(uLight, q, 0);
        if (abs(there.a - own.a) > 0.5) {
            continue; // another profile, or not the material
        }
        // The profile at the sample's distance, over the density it was drawn with (the widest channel's at its radius).
        const float dz = view_position(q).z - here.z;
        const float apart = sqrt(r * r + dz * dz);
        // (The density, Burley's of the widest channel at r: d = 1's, over d^2.)
        const vec3 w = burley(apart, profile.rgb) * (profile.w * profile.w) / params.density[pick >> 2][pick & 3];
        sum += there.rgb * w;
        weights += w;
    }
    // (The pixel itself where no sample stayed on the material.)
    const vec3 spread = mix(own.rgb, sum / max(weights, vec3(1e-12)), step(vec3(1e-12), weights));
    imageStore(uScattered, p, vec4(spread, 1.0));
}
