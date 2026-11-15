#version 300 es
// kin's 3D particles in WebGL2 (Scene3D::draw_particles, Scene3D::weather): native kin_particle_vertex.inc.glsl's quad,
// six vertices an instance, the sprite in instanced attributes (as native's GpuParticle3D) instead of a storage buffer.
precision highp float;
precision highp int;
layout(location = 0) in vec4 aPositionSize; // the centre, the width
layout(location = 1) in vec4 aColour;       // linear, straight alpha
layout(location = 2) in vec4 aAxisRotation; // the velocity (Stretched), the rotation (radians)
layout(location = 3) in vec4 aFrame;        // x: the flipbook frame
uniform mat4 uViewProjection;
uniform vec3 uRight, uUp, uEye, uForward;
uniform bool uOrtho;
uniform vec4 uAxis;    // xyz: the Axis and Flat facings' axis (unit), w: the facing (0 camera, 1 stretched, 2 axis, 3 flat)
uniform vec4 uAtlas;   // x: columns, y: rows, z: frames, w: 1 to mix two frames
uniform vec2 uStretch; // x: seconds a streak spans, y: its least length
uniform float uLit;    // ParticleLook3D::lit: the local lights' share at the sprite's centre (0: none)
#include "lights.inc.glsl"
out vec2 vUV0;
out vec2 vUV1;
out vec4 vColour;
out vec2 vLocal;
out float vDepth; // along the camera's forward
out float vMix;   // the second frame's share
out vec3 vLocalLight;
out vec3 vWorld;
flat out vec3 vCentre;
const vec2 CORNERS[4] = vec2[4](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(1.0, 1.0), vec2(-1.0, 1.0));
const int CORNER_OF[6] = int[6](0, 1, 2, 0, 2, 3);

vec2 frame_uv(float frame, vec2 uv) {
    float columns = max(uAtlas.x, 1.0), rows = max(uAtlas.y, 1.0);
    float column = mod(frame, columns), row = floor(frame / columns);
    return (vec2(column, row) + uv) / vec2(columns, rows);
}

void main() {
    vec2 c = CORNERS[CORNER_OF[gl_VertexID]];
    vec3 centre = aPositionSize.xyz;
    float half_size = aPositionSize.w * 0.5;
    vec3 to_eye = uOrtho ? -uForward : normalize(uEye - centre);
    int facing = int(uAxis.w + 0.5);
    vec3 world;
    if (facing == 1) {
        vec3 streak = aAxisRotation.xyz * uStretch.x;
        float l = length(streak);
        if (l < uStretch.y) streak = l > 1e-6 ? streak * (uStretch.y / l) : vec3(0.0, uStretch.y, 0.0);
        float len = length(streak);
        vec3 dir = len > 1e-6 ? streak / len : vec3(0.0, 1.0, 0.0);
        vec3 side = cross(dir, to_eye);
        side = dot(side, side) > 1e-12 ? normalize(side) : uRight;
        float along = c.y * 0.5 + 0.5;
        world = centre - streak * (1.0 - along) + side * (c.x * half_size);
    } else if (facing == 2) {
        vec3 up = uAxis.xyz;
        vec3 right = cross(up, to_eye);
        right = dot(right, right) > 1e-12 ? normalize(right) : uRight;
        world = centre + right * (c.x * half_size) + up * (c.y * half_size);
    } else if (facing == 3) {
        vec3 n = uAxis.xyz;
        vec3 helper = abs(n.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
        vec3 t = normalize(cross(n, helper));
        vec3 b = cross(n, t);
        float cs = cos(aAxisRotation.w), sn = sin(aAxisRotation.w);
        world = centre + (t * cs + b * sn) * (c.x * half_size) + (t * -sn + b * cs) * (c.y * half_size);
    } else {
        float cs = cos(aAxisRotation.w), sn = sin(aAxisRotation.w);
        world = centre + (uRight * cs + uUp * sn) * (c.x * half_size) + (uRight * -sn + uUp * cs) * (c.y * half_size);
    }
    gl_Position = uViewProjection * vec4(world, 1.0);
    // Kin's reverse-Z projection maps depth to 0..1. WebGL clips -w..w.
    gl_Position.z = 2.0 * gl_Position.z - gl_Position.w;
    vWorld = world;
    vCentre = centre;
    vColour = aColour;
    vLocalLight = vec3(0.0);
    if (uLit > 0.0) {
        for (int i = 0; i < uPointCount; ++i) {
            vec4 at = point_light(i, 0), colour = point_light(i, 1);
            vec3 to = at.xyz - centre;
            float d2 = dot(to, to), r2 = d2 / (at.w * at.w);
            float window = clamp(1.0 - r2 * r2, 0.0, 1.0);
            vLocalLight += colour.rgb * (window * window / max(d2, colour.w));
        }
        for (int i = 0; i < uSpotCount; ++i) {
            vec4 at = spot_light(i, 0), colour = spot_light(i, 1), axis = spot_light(i, 2), more = spot_light(i, 3);
            vec3 to = at.xyz - centre;
            float d2 = dot(to, to), r2 = d2 / (at.w * at.w);
            float window = clamp(1.0 - r2 * r2, 0.0, 1.0);
            vec3 toward = to * inversesqrt(max(d2, 1e-8));
            float cone = clamp((dot(-toward, axis.xyz) - axis.w) * more.x, 0.0, 1.0);
            vLocalLight += colour.rgb * (window * window * cone * cone / max(d2, colour.w));
        }
        vLocalLight *= 0.5 * uLit;
    }
    vLocal = c;
    vDepth = dot(world - uEye, uForward);
    vec2 uv = vec2(c.x * 0.5 + 0.5, 0.5 - c.y * 0.5);
    float frames = max(uAtlas.z, 1.0);
    float f = clamp(aFrame.x, 0.0, frames - 1.0);
    if (uAtlas.w > 0.5) {
        float f0 = floor(f);
        vUV0 = frame_uv(f0, uv);
        vUV1 = frame_uv(min(f0 + 1.0, frames - 1.0), uv);
        vMix = f - f0;
    } else {
        vUV0 = frame_uv(floor(f + 0.5), uv);
        vUV1 = vUV0;
        vMix = 0.0;
    }
}
