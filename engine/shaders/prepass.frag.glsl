#version 450 core
// kin's 3D ambient occlusion pre-pass: the world normal of what is nearest,
// on the side the viewer sees, octahedral in two floats.
layout(location = 0) in vec3 vNormal;
layout(location = 0) out vec2 FragNormal;

vec2 octahedral(vec3 n) {
    n /= abs(n.x) + abs(n.y) + abs(n.z);
    const vec2 fold = (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    return n.z >= 0.0 ? n.xy : fold;
}

void main() {
    FragNormal = octahedral(normalize(vNormal) * (gl_FrontFacing ? 1.0 : -1.0));
}
