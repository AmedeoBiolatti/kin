#version 450 core
layout(location = 4) flat in uint vId;
layout(location = 0) out vec4 FragColor;
layout(std140, set = 3, binding = 1) uniform Visibility { uvec4 ids; } query;
void main() {
    if (query.ids.z != 0u && vId == query.ids.y + 1u) discard;
    const float visible = vId == query.ids.x + 1u ? 1.0 : 0.0;
    FragColor = vec4(vec3(visible), 1.0);
}
