// What the pre-pass fragment shaders share (prepass*.frag).

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
