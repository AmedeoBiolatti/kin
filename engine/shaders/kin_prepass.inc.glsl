// What the pre-pass fragment shaders share (prepass*.frag).

// The world normal on the side the viewer sees, from the vertex normal, or (a flat-shaded mesh with clusters has
// none: zero) from the derivatives of the world position, which every pre-pass fragment shader declares as vWorld.
vec3 kin_prepass_normal() {
    const vec3 dp1 = dFdx(vWorld), dp2 = dFdy(vWorld);
    if (dot(vNormal, vNormal) < 1e-20) {
        const vec3 n = cross(dp1, dp2);
        const float l = length(n);
        return l > 0.0 ? n * (-1.0 / l) : vec3(0.0, 1.0, 0.0);
    }
    return normalize(vNormal) * (gl_FrontFacing ? 1.0 : -1.0);
}

// A unit normal in two floats (octahedral); gtao.comp and contact.comp decode it.
vec2 octahedral(vec3 n) {
    n /= abs(n.x) + abs(n.y) + abs(n.z);
    const vec2 fold = (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    return n.z >= 0.0 ? n.xy : fold;
}

// How far a point moved on the screen since the last frame, from its clip
// x, y, w now and then; `fresh` is 1 for a copy with no past. Far off the
// screen for a point with no history.
vec2 kin_velocity(vec3 current, vec3 previous, float fresh) {
    if (fresh > 0.5 || previous.z <= 1e-5 || current.z <= 1e-5) {
        return vec2(8.0);
    }
    // Clip space's +y is up and the picture's rows run down.
    return (current.xy / current.z - previous.xy / previous.z) * vec2(0.5, -0.5);
}
