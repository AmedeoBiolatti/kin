// kin's 3D particle vertex shader (Scene3D::draw_particles, Scene3D::weather; P3c): one instanced draw is a batch of
// sprites, six vertices each, read from a storage buffer. The quad is turned in the shader (camera-facing, stretched along the
// velocity, locked to an axis, or flat), and the fog is worked out here, once for each corner, and interpolated: a particle
// is small, the fog's closed form is smooth, and the sky's colour uses the same cached table as the meshes. The same
// arithmetic runs on the CPU in particles3d.cpp (particle_quad) for the tests.
#include "kin_fog.inc.glsl"

// A sprite as the CPU writes it (GpuParticle3D in gpu_renderer3d.cpp: change them together).
struct Particle {
    vec4 position_size; // xyz: the centre (world), w: the width
    vec4 colour;        // linear rgb, straight alpha
    vec4 axis_rotation; // xyz: velocity (Stretched), w: rotation (radians)
    vec4 frame;         // x: the flipbook frame (fractional)
};
layout(std430, set = 0, binding = 0) readonly buffer Particles {
    Particle particles[];
};

#ifdef KIN_GPU_PARTICLES
#ifdef KIN_PARTICLE_TEMPORAL
layout(std430,set=0,binding=2) readonly buffer ParticleOrder { uvec2 order[]; };
#else
layout(std430,set=0,binding=1) readonly buffer ParticleOrder { uvec2 order[]; };
#endif
#endif

#include "kin_particle_draw.inc.glsl"
#ifdef KIN_PARTICLE_LIGHTS
layout(set = 1, binding = 2) uniform ParticleLights {
    vec4 counts; // points, spots, lit fraction
    vec4 points[16 * 2]; // position/radius, colour/size squared
    vec4 spots[16 * 4];  // the mesh lights' position, colour, cone axis and falloff
} particle_lights;
#endif

layout(location = 0) out vec2 vUV0;
layout(location = 1) out vec2 vUV1;
layout(location = 2) out vec4 vColour;
layout(location = 3) out vec2 vLocal;
layout(location = 4) out float vDepth; // along the camera's forward
layout(location = 5) out vec4 vFog;    // rgb: the fog's colour, w: its opacity
layout(location = 6) out float vMix;   // the second frame's share
#ifdef KIN_PARTICLE_SHADOWS
layout(location = 8) out vec3 vWorld;
layout(location = 9) flat out vec3 vCentre;
#endif
#ifdef KIN_PARTICLE_LIGHTS
layout(location = 7) out vec3 vLocalLight;
#endif

#ifdef KIN_PARTICLE_TEMPORAL
#ifdef KIN_GPU_PARTICLES
layout(std430, set = 0, binding = 1) readonly buffer PreviousParticles { Particle prior_particles[]; };
#else
struct PreviousQuad { vec4 corners[4]; };
layout(std430, set = 0, binding = 1) readonly buffer PreviousParticles { PreviousQuad quads[]; };
#endif
layout(set = 1, binding = 3) uniform Temporal {
    mat4 current; mat4 prior;
#ifdef KIN_GPU_PARTICLES
    vec4 right, up, eye, forward;
    uvec4 history; // x: valid previous slots, y: forced reset, z: blends frames
#endif
} temporal;
layout(location = 10) out vec3 vCurrent;
layout(location = 11) out vec3 vPrevious;
layout(location = 12) flat out float vFresh;
#endif

#include "kin_particle_quad.inc.glsl"

const vec2 CORNERS[4] = vec2[4](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(1.0, 1.0), vec2(-1.0, 1.0));
const int CORNER_OF[6] = int[6](0, 1, 2, 0, 2, 3);

vec2 frame_uv(float frame, vec2 uv) {
    const float columns = max(draw.atlas.x, 1.0);
    const float rows = max(draw.atlas.y, 1.0);
    const float column = mod(frame, columns);
    const float row = floor(frame / columns);
    return (vec2(column, row) + uv) / vec2(columns, rows);
}

void main() {
    uint slot = draw.first.x + uint(gl_InstanceIndex);
#ifdef KIN_GPU_PARTICLES
    bool visible = true;
    if (draw.first.z != 0u) {
        uvec2 key = order[slot];
        slot = key.y;
        visible = key.x != 0u;
    }
#endif
    Particle p = particles[slot];
#ifdef KIN_GPU_PARTICLES
    if (!visible) { p.colour = vec4(0); p.position_size.w = 0.0; }
#endif
    const vec2 c = CORNERS[CORNER_OF[gl_VertexIndex]];
    const vec3 centre = p.position_size.xyz;
    const bool ortho = draw.eye.w > 0.5;
    const vec3 world = kin_particle_corner(p, c, draw.right, draw.up, draw.eye, draw.forward, draw.axis, draw.stretch);
    gl_Position = draw.view_projection * vec4(world, 1.0);
    gl_ClipDistance[0] = dot(draw.clip.xyz, world) + draw.clip.w;
#ifdef KIN_PARTICLE_TEMPORAL
#ifdef KIN_GPU_PARTICLES
    bool known = temporal.history.y == 0u && slot < temporal.history.x;
    Particle before = p;
    if (known) {
        before = prior_particles[slot];
        known = all(equal(floatBitsToUint(before.frame.yzw), floatBitsToUint(p.frame.yzw))) &&
                before.colour.a > 0.0 && abs(before.colour.a - p.colour.a) < 0.15 &&
                all(lessThan(abs(before.colour.rgb - p.colour.rgb), vec3(0.15))) &&
                abs(before.frame.x - p.frame.x) < 1.0 &&
                (temporal.history.z != 0u || floor(before.frame.x + 0.5) == floor(p.frame.x + 0.5)) &&
                dot(before.position_size.xyz - centre, before.position_size.xyz - centre) <
                    max(1.0, p.position_size.w * p.position_size.w * 16.0);
    }
    const vec4 previous = known
        ? vec4(kin_particle_corner(before, c, temporal.right, temporal.up, temporal.eye, temporal.forward,
                                   draw.axis, draw.stretch), 0.0)
        : vec4(world, 1.0);
#else
    const vec4 previous = quads[draw.first.x + uint(gl_InstanceIndex)].corners[CORNER_OF[gl_VertexIndex]];
#endif
    vCurrent = (temporal.current * vec4(world, 1.0)).xyw;
    vPrevious = (temporal.prior * vec4(previous.xyz, 1.0)).xyw;
    vFresh = previous.w;
#endif
#ifdef KIN_PARTICLE_SHADOWS
    vWorld = world;
    vCentre = centre;
#endif
    vColour = p.colour;
#ifdef KIN_PARTICLE_LIGHTS
    vLocalLight = vec3(0.0);
    if (particle_lights.counts.z > 0.0) {
        for (int i = 0; i < int(particle_lights.counts.x); ++i) {
            const vec4 at = particle_lights.points[2 * i];
            const vec4 colour = particle_lights.points[2 * i + 1];
            const vec3 to = at.xyz - centre;
            const float d2 = dot(to, to), r2 = d2 / (at.w * at.w);
            const float window = clamp(1.0 - r2 * r2, 0.0, 1.0);
            vLocalLight += colour.rgb * (window * window / max(d2, colour.w));
        }
        for (int i = 0; i < int(particle_lights.counts.y); ++i) {
            const vec4 at = particle_lights.spots[4 * i];
            const vec4 colour = particle_lights.spots[4 * i + 1];
            const vec4 axis = particle_lights.spots[4 * i + 2];
            const vec4 more = particle_lights.spots[4 * i + 3];
            const vec3 to = at.xyz - centre;
            const float d2 = dot(to, to), r2 = d2 / (at.w * at.w);
            const float window = clamp(1.0 - r2 * r2, 0.0, 1.0);
            const vec3 toward = to * inversesqrt(max(d2, 1e-8));
            const float cone = clamp((dot(-toward, axis.xyz) - axis.w) * more.x, 0.0, 1.0);
            vLocalLight += colour.rgb * (window * window * cone * cone / max(d2, colour.w));
        }
        // The same isotropic approximation as the directional-light tint.
        vLocalLight *= 0.5 * particle_lights.counts.z;
    }
#endif
    vLocal = c;
    vDepth = dot(world - draw.eye.xyz, draw.forward.xyz);
    const vec2 uv = vec2(c.x * 0.5 + 0.5, 0.5 - c.y * 0.5);
    const float frames = max(draw.atlas.z, 1.0);
    const float f = clamp(p.frame.x, 0.0, frames - 1.0);
    if (draw.atlas.w > 0.5) {
        const float f0 = floor(f);
        vUV0 = frame_uv(f0, uv);
        vUV1 = frame_uv(min(f0 + 1.0, frames - 1.0), uv);
        vMix = f - f0;
    } else {
        vUV0 = frame_uv(floor(f + 0.5), uv);
        vUV1 = vUV0;
        vMix = 0.0;
    }
    vFog = vec4(0.0);
    if (draw.first.y != 0u) {
        const vec3 to = world - draw.eye.xyz;
        float dist = length(to);
        vec3 dir = dist > 1e-6 ? to / dist : vec3(0.0, 0.0, -1.0);
        if (ortho) {
            dir = draw.forward.xyz;
            dist = dot(to, dir);
        }
        const float tau = kin_fog_optical_depth(draw.fog, draw.eye.xyz, dir, dist, false);
        const float opacity = kin_fog_opacity(draw.fog, tau);
        if (opacity > 0.0) {
            const vec3 colour = draw.fog.colour.w > 0.5 ? particle_fog_sky(dir) : draw.fog.colour.rgb;
            // (With Fog3D::sun_shadows the sun's share is added after the main pass: fog_sun_apply.frag.)
            vFog = vec4(draw.fog.view.z > 0.5 ? colour
                                              : kin_fog_sun(draw.fog, colour, dir, draw.sun_direction.xyz, draw.sun_light.rgb),
                        opacity);
        }
    }
}
