#version 450 core
// A conservative history-rejection footprint for blended/custom meshes. Only R
// is written after AO/contact shading has finished; BA retain opaque motion.
layout(location = 1) in vec4 vColor;
layout(location = 2) in vec2 vUV;
layout(set = 2, binding = 0) uniform sampler2D uBase;
layout(set = 3, binding = 0) uniform Reactive {
    vec4 flags; // x: custom shader (its procedural coverage is unknown)
} reactive;
layout(location = 0) out vec4 FragReactive;
void main() {
    if (reactive.flags.x < 0.5 && vColor.a * texture(uBase, vUV).a <= 0.0)
        discard;
    // Octahedral normals occupy [-1,1]; 2 marks a surface with no single motion.
    FragReactive = vec4(2.0, 0.0, 0.0, 0.0);
}
