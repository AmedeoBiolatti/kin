#version 300 es
precision highp float;
precision highp int;
layout(location=0) in vec3 aPosition;
layout(location=1) in vec3 aNormal;
layout(location=2) in vec2 aUV;
layout(location=3) in vec4 aColor;
layout(location=4) in mat4 aModel;
layout(location=8) in mat3 aNormalMatrix;
layout(location=11) in vec4 aTint;
layout(location=12) in uvec4 aJoints;
layout(location=13) in vec4 aWeights;
layout(location=14) in uint aJointOffset;
uniform bool uSkinned;
uniform highp sampler2D uJoints;
uniform mat4 uViewProjection;
out vec3 vWorld;
out vec3 vNormal;
out vec2 vUV;
out vec4 vColor;
vec3 decode(vec3 c) {
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c));
}
mat4 joint(uint id) {
    int row = int(aJointOffset + id);
    return mat4(texelFetch(uJoints,ivec2(0,row),0),texelFetch(uJoints,ivec2(1,row),0),
                texelFetch(uJoints,ivec2(2,row),0),texelFetch(uJoints,ivec2(3,row),0));
}
void main() {
    vec4 position = vec4(aPosition,1.0);
    vec3 normal = aNormal;
    if (uSkinned) {
        mat4 skin = mat4(0.0);
        for (int k=0; k<4; ++k) if (aWeights[k] > 0.0) skin += joint(aJoints[k])*aWeights[k];
        position = skin*position;
        // Same normal approximation as native: rigid/uniformly scaled joints.
        normal = mat3(skin)*normal;
    }
    vec4 world = aModel * position;
    gl_Position = uViewProjection * world;
    // Kin's reverse-Z projection maps depth to 0..1. WebGL clips -w..w.
    gl_Position.z = 2.0 * gl_Position.z - gl_Position.w;
    vWorld = world.xyz;
    vNormal = aNormalMatrix * normal;
    vUV = aUV;
    vColor = vec4(decode(aColor.rgb) * decode(aTint.rgb), aColor.a * aTint.a);
}
