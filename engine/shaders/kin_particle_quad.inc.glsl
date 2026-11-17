// Shared by the current and previous-view GPU particle quads.
vec3 kin_particle_corner(Particle p, vec2 c, vec4 camera_right, vec4 camera_up,
                         vec4 eye, vec4 forward, vec4 axis, vec4 stretch) {
    const vec3 centre = p.position_size.xyz;
    const float half_size = p.position_size.w * 0.5;
    const bool ortho = eye.w > 0.5;
    const vec3 to_eye = ortho ? -forward.xyz : normalize(eye.xyz - centre);
    const int facing = int(axis.w + 0.5);
    vec3 world;
    if (facing == 1) {
        // Along the velocity: the head at the particle, the tail a streak behind; broad toward the viewer.
        vec3 streak = p.axis_rotation.xyz * stretch.x;
        const float l = length(streak);
        if (l < stretch.y) {
            streak = l > 1e-6 ? streak * (stretch.y / l) : vec3(0.0, stretch.y, 0.0);
        }
        const float len = length(streak);
        const vec3 dir = len > 1e-6 ? streak / len : vec3(0.0, 1.0, 0.0);
        vec3 side = cross(dir, to_eye);
        side = dot(side, side) > 1e-12 ? normalize(side) : camera_right.xyz;
        const float along = c.y * 0.5 + 0.5;
        world = centre - streak * (1.0 - along) + side * (c.x * half_size);
    } else if (facing == 2) {
        const vec3 up = axis.xyz;
        vec3 right = cross(up, to_eye);
        right = dot(right, right) > 1e-12 ? normalize(right) : camera_right.xyz;
        world = centre + right * (c.x * half_size) + up * (c.y * half_size);
    } else if (facing == 3) {
        const vec3 n = axis.xyz;
        const vec3 helper = abs(n.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
        const vec3 t = normalize(cross(n, helper));
        const vec3 b = cross(n, t);
        const float cs = cos(p.axis_rotation.w), sn = sin(p.axis_rotation.w);
        world = centre + (t * cs + b * sn) * (c.x * half_size) + (t * -sn + b * cs) * (c.y * half_size);
    } else {
        const float cs = cos(p.axis_rotation.w), sn = sin(p.axis_rotation.w);
        world = centre + (camera_right.xyz * cs + camera_up.xyz * sn) * (c.x * half_size) +
                (camera_right.xyz * -sn + camera_up.xyz * cs) * (c.y * half_size);
    }
    return world;
}
