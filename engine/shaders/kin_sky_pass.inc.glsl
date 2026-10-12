// kin's sky pass (Scene3D::sky): one full-screen triangle at the far plane, drawn after the opaque meshes with the depth
// test of "far only", so it shows exactly where nothing was drawn. Shared by sky.frag (the colour), sky_velocity.frag (the
// colour and, in a second target, the camera's turn since the last frame, for temporal anti-aliasing with no pre-pass) and
// sky_velocity_only.frag (just the velocity, drawn into the pre-pass's target).
#include "kin_fog.inc.glsl"
#include "kin_velocity.inc.glsl"

#if defined(KIN_SKY_VELOCITY_ONLY)
// The velocity-only pass reads nothing but its own block (slot 0), so that it needs no frame uniforms and no texture.
#define KIN_SKY_BLOCK_BINDING 0
#else
#define KIN_SKY_BLOCK_BINDING 1
layout(set = 2, binding = 0) uniform sampler2D uEnvSpecular; // Image mode: the panorama (any texture else)

// The frame's uniforms (kin_material.inc.glsl's Frame, the part this shader reads: a prefix of the same block).
layout(set = 3, binding = 0) uniform Frame {
    vec4 sun_direction;
    vec4 sun_color;
    vec4 ambient;
    vec4 ground_ambient;
    vec4 fill_direction;
    vec4 fill_color;
    vec4 flags;       // x: encode to sRGB
    vec4 eye;
    vec4 environment; // x: intensity, w: 1 with an environment
    mat4 environment_matrix;
} frame;
#endif

layout(set = 3, binding = KIN_SKY_BLOCK_BINDING) uniform SkyBlock {
    mat4 inverse_view_projection; // clip space (jittered, as the meshes are drawn) to world
    mat4 view_projection;         // this frame's, without the jitter
    mat4 prior_view_projection;   // the last frame's (this one's when there is no history)
    vec4 target;                  // xy: the picture's size, z: 1 to show the panorama, w: 1 when the camera is orthographic
    vec4 camera;                  // xyz: where it is
    vec4 forward;                 // xyz: where it looks
    vec4 disc;                    // x: the sun disc's angular radius (radians), y: the moon's, z: the sun disc's radiance over the sun's light, w: the star field's brightness (0: none)
    vec4 moon_color;              // rgb: the moon disc's radiance, w: 1 when it is drawn
    KinSky atmosphere;
    KinFog fog;
} sky;

#if defined(KIN_SKY_VELOCITY_ONLY)
layout(location = 0) out vec4 FragVelocity;
#else
layout(location = 0) out vec4 FragColor;
#if defined(KIN_SKY_VELOCITY)
layout(location = 1) out vec4 FragVelocity;
#endif
#endif

// A hash of an integer cell (PCG3D, Jarzynski and Olano), on integers: the same bits on every GPU, so a star is where it is
// everywhere (a float hash flips stars at its threshold between drivers). `salt` makes others of the same cell.
float kin_hash(vec3 cell, uint salt) {
    uvec3 v = uvec3(ivec3(cell)) * 1664525u + uvec3(1013904223u) + uvec3(salt * 2654435761u, salt * 2246822519u, salt * 3266489917u);
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    v ^= v >> 16u;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    return float(v.x >> 8u) * (1.0 / 16777216.0);
}

vec3 kin_encode(vec3 c) {
    c = clamp(c, 0.0, 1.0);
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c));
}

// Stars: small soft discs on a grid of cells over the sphere of directions, one cell in thirty or so holding one, each its
// own brightness and place in its cell (0.15 to 0.85), about a pixel and a quarter wide at least (so a star is seen, and
// does not twinkle by falling between pixel centres). When that is more than a sixth of a cell (a coarse view) the
// neighbouring cells are looked in too. Fixed in the world's directions.
vec3 kin_stars(vec3 d, float pixel) {
    const float scale = 48.0;
    const vec3 p = d * scale;
    const vec3 cell = floor(p);
    const float radius = max(pixel * scale * 1.25, 0.05);
    const int span = radius > 0.15 ? 1 : 0;
    vec3 sum = vec3(0.0);
    for (int z = -span; z <= span; ++z) {
        for (int y = -span; y <= span; ++y) {
            for (int x = -span; x <= span; ++x) {
                const vec3 c = cell + vec3(float(x), float(y), float(z));
                const float h = kin_hash(c, 0u);
                if (h < 0.965) {
                    continue;
                }
                const vec3 jitter = vec3(kin_hash(c, 1u), kin_hash(c, 2u), kin_hash(c, 3u)) * 0.7 + 0.15;
                const float brightness = (h - 0.965) / 0.035;
                const float k = 1.0 - smoothstep(0.0, radius, length(p - (c + jitter)));
                const vec3 tint = mix(vec3(0.75, 0.85, 1.0), vec3(1.0, 0.85, 0.7), kin_hash(c, 4u));
                sum += tint * (k * k * (0.25 + 0.75 * brightness * brightness));
            }
        }
    }
    return sum;
}

void main() {
    const vec2 uv = gl_FragCoord.xy / sky.target.xy;
    const vec2 ndc = vec2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    vec3 d;
    if (sky.target.w > 0.5) {
        d = normalize(sky.forward.xyz);
    } else {
        const vec4 p = sky.inverse_view_projection * vec4(ndc, 1.0, 1.0);
        d = normalize(p.xyz / p.w - sky.camera.xyz);
    }
#if defined(KIN_SKY_VELOCITY_ONLY)
    // The camera's turn: the direction at infinity, in this frame's and the last frame's clip space.
    const vec4 now = sky.view_projection * vec4(d, 0.0);
    const vec4 before = sky.prior_view_projection * vec4(d, 0.0);
    FragVelocity = vec4(0.0, 0.0, kin_velocity(now.xyw, before.xyw, 0.0));
#else
    const float pixel = max(length(dFdx(d)), length(dFdy(d)));
    vec3 c;
    if (sky.target.z > 0.5) {
        // The light's own turn (Lights3D::environment_rotation), as the surfaces take it.
        const vec3 e = mat3(frame.environment_matrix) * d;
        const vec2 st = vec2(0.5 + atan(e.z, e.x) / (2.0 * KIN_PI), 0.5 - asin(clamp(e.y, -1.0, 1.0)) / KIN_PI);
        c = textureLod(uEnvSpecular, st, 0.0).rgb * frame.environment.x;
    } else {
        c = kin_sky_color(sky.atmosphere, d);
        if (d.y > -0.1) {
            // The sun's disc, a pixel-wide edge: coverage by the angle to its centre (atan of the cross over the dot
            // is exact for small angles, where an arccosine is not).
            const vec3 sun = sky.atmosphere.sun.xyz;
            const float angle = atan(length(cross(d, sun)), dot(d, sun));
            const float cover = clamp((sky.disc.x - angle) / max(pixel, 1e-6) + 0.5, 0.0, 1.0);
            // Under the horizon the ground hides it: the same one-pixel edge.
            const float above = clamp(d.y / max(pixel, 1e-6) + 0.5, 0.0, 1.0);
            const float limb = 0.75 + 0.25 * sqrt(max(1.0 - (angle * angle) / (sky.disc.x * sky.disc.x), 0.0));
            c += sky.atmosphere.sun.w * kin_sun_transmittance(sky.atmosphere, sky.atmosphere.sun.y) * sky.disc.z * (cover * above * limb);
            if (sky.moon_color.w > 0.5) {
                const vec3 moon = sky.atmosphere.moon.xyz;
                const float m_angle = atan(length(cross(d, moon)), dot(d, moon));
                const float m_cover = clamp((sky.disc.y - m_angle) / max(pixel, 1e-6) + 0.5, 0.0, 1.0);
                c += sky.moon_color.rgb * (m_cover * above);
            }
            if (sky.disc.w > 0.0) {
                // The stars come out as the sun sinks (none above -3 degrees, all below -12), dimmed by the air
                // near the horizon.
                const float night = clamp((-sky.atmosphere.sun.y - 0.05) / 0.15, 0.0, 1.0);
                if (night > 0.0) {
                    c += kin_stars(d, pixel) * (sky.disc.w * night * above * exp(-0.12 * kin_air_mass(d.y)));
                }
            }
        }
    }
    // Height fog on the way to the sky: the same optical depth as for geometry, to infinity, over the horizon.
    const vec3 sun_dir = frame.sun_direction.xyz;
    const float opacity = kin_fog_opacity(sky.fog, kin_fog_optical_depth(sky.fog, sky.camera.xyz, d, 0.0, true));
    if (sky.fog.limits.y > 0.0 && opacity > 0.0 && (sky.fog.density.y > 0.0)) {
        c = mix(c, kin_fog_colour(sky.fog, sky.atmosphere, d, sun_dir, frame.sun_color.rgb), opacity);
    }
    if (frame.flags.x > 0.5) {
        c = kin_encode(c);
    }
    FragColor = vec4(c, 1.0);
#if defined(KIN_SKY_VELOCITY)
    const vec4 now = sky.view_projection * vec4(d, 0.0);
    const vec4 before = sky.prior_view_projection * vec4(d, 0.0);
    FragVelocity = vec4(0.0, 0.0, kin_velocity(now.xyw, before.xyw, 0.0));
#endif
#endif
}
