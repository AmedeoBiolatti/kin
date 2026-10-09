// How far a point moved on the screen since the last frame: shared by the velocity pre-pass (prepass_velocity*.frag)
// and the main pass that writes the velocity itself (mesh_velocity.frag), so that both give the same bits.
// x, y, w now and then; `fresh` is 1 for a copy with no past. Far off the
// screen for a point with no history.
vec2 kin_velocity(vec3 current, vec3 previous, float fresh) {
    if (fresh > 0.5 || previous.z <= 1e-5 || current.z <= 1e-5) {
        return vec2(8.0);
    }
    // Clip space's +y is up and the picture's rows run down.
    return (current.xy / current.z - previous.xy / previous.z) * vec2(0.5, -0.5);
}
