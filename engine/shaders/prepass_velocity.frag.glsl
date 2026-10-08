#version 450 core
// The pre-pass with velocity (temporal anti-aliasing): the world normal as
// prepass.frag has it, octahedral, in R and G; in B and A how far this pixel's
// point has moved on the screen since the last frame, in units of the picture
// (0 to 1 across), so that the history is at uv - velocity. A point with no
// past (a new copy, or one behind the camera last frame) gets a velocity far
// off the screen, which the resolve reads as "no history here".
layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec2 vUV;
layout(location = 2) in float vAlpha;
layout(location = 3) in vec3 vCurrent;
layout(location = 4) in vec3 vPrevious;
layout(location = 5) flat in float vFresh;
layout(location = 0) out vec4 FragNormalVelocity;

vec2 octahedral(vec3 n) {
    n /= abs(n.x) + abs(n.y) + abs(n.z);
    const vec2 fold = (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    return n.z >= 0.0 ? n.xy : fold;
}

vec2 velocity() {
    if (vFresh > 0.5 || vPrevious.z <= 1e-5 || vCurrent.z <= 1e-5) {
        return vec2(8.0);
    }
    // Clip space's +y is up and the picture's rows run down.
    return (vCurrent.xy / vCurrent.z - vPrevious.xy / vPrevious.z) * vec2(0.5, -0.5);
}

void main() {
    FragNormalVelocity = vec4(octahedral(normalize(vNormal) * (gl_FrontFacing ? 1.0 : -1.0)), velocity());
}
