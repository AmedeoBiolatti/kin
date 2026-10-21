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

layout(set = 1, binding = 0) uniform ParticleDraw {
    mat4 view_projection; // jittered: where the pixel is drawn
    vec4 right;           // xyz: the camera's right,
    vec4 up;              //      up
    vec4 eye;             // xyz: where it is, w: 1 for an orthographic camera
    vec4 forward;         // xyz: where it looks
    vec4 axis;            // xyz: the Axis and Flat facings' axis (unit), w: the facing (0 camera, 1 stretched, 2 axis, 3 flat)
    vec4 atlas;           // x: columns, y: rows, z: frames, w: 1 to mix two frames
    vec4 stretch;         // x: seconds a streak spans, y: its least length
    uvec4 first;          // x: this batch's first sprite, y: 1 when fogged
    vec4 sun_direction;   // xyz: toward the sun
    vec4 sun_light;       // rgb: its light
    KinFog fog;
    KinSky sky;
} draw;
layout(set = 1, binding = 1) uniform FogTable {
    vec4 rows[KIN_FOG_ROWS * 4];
} fog_table;
KIN_FOG_TABLE_READER(particle_fog_sky, draw.sky, fog_table.rows)
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
struct PreviousQuad { vec4 corners[4]; };
layout(std430, set = 0, binding = 1) readonly buffer PreviousParticles { PreviousQuad quads[]; };
layout(set = 1, binding = 3) uniform Temporal { mat4 current; mat4 prior; } temporal;
layout(location = 10) out vec3 vCurrent;
layout(location = 11) out vec3 vPrevious;
layout(location = 12) flat out float vFresh;
#endif

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
    const Particle p = particles[draw.first.x + uint(gl_InstanceIndex)];
    const vec2 c = CORNERS[CORNER_OF[gl_VertexIndex]];
    const vec3 centre = p.position_size.xyz;
    const float half_size = p.position_size.w * 0.5;
    const bool ortho = draw.eye.w > 0.5;
    const vec3 to_eye = ortho ? -draw.forward.xyz : normalize(draw.eye.xyz - centre);
    const int facing = int(draw.axis.w + 0.5);
    vec3 world;
    if (facing == 1) {
        // Along the velocity: the head at the particle, the tail a streak behind; broad toward the viewer.
        vec3 streak = p.axis_rotation.xyz * draw.stretch.x;
        const float l = length(streak);
        if (l < draw.stretch.y) {
            streak = l > 1e-6 ? streak * (draw.stretch.y / l) : vec3(0.0, draw.stretch.y, 0.0);
        }
        const float len = length(streak);
        const vec3 dir = len > 1e-6 ? streak / len : vec3(0.0, 1.0, 0.0);
        vec3 side = cross(dir, to_eye);
        side = dot(side, side) > 1e-12 ? normalize(side) : draw.right.xyz;
        const float along = c.y * 0.5 + 0.5;
        world = centre - streak * (1.0 - along) + side * (c.x * half_size);
    } else if (facing == 2) {
        const vec3 up = draw.axis.xyz;
        vec3 right = cross(up, to_eye);
        right = dot(right, right) > 1e-12 ? normalize(right) : draw.right.xyz;
        world = centre + right * (c.x * half_size) + up * (c.y * half_size);
    } else if (facing == 3) {
        const vec3 n = draw.axis.xyz;
        const vec3 helper = abs(n.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
        const vec3 t = normalize(cross(n, helper));
        const vec3 b = cross(n, t);
        const float cs = cos(p.axis_rotation.w), sn = sin(p.axis_rotation.w);
        world = centre + (t * cs + b * sn) * (c.x * half_size) + (t * -sn + b * cs) * (c.y * half_size);
    } else {
        const float cs = cos(p.axis_rotation.w), sn = sin(p.axis_rotation.w);
        world = centre + (draw.right.xyz * cs + draw.up.xyz * sn) * (c.x * half_size) +
                (draw.right.xyz * -sn + draw.up.xyz * cs) * (c.y * half_size);
    }
    gl_Position = draw.view_projection * vec4(world, 1.0);
#ifdef KIN_PARTICLE_TEMPORAL
    const vec4 previous = quads[draw.first.x + uint(gl_InstanceIndex)].corners[CORNER_OF[gl_VertexIndex]];
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
            vFog = vec4(kin_fog_sun(draw.fog, colour, dir, draw.sun_direction.xyz, draw.sun_light.rgb), opacity);
        }
    }
}
