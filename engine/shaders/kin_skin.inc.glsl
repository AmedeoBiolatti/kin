// Skinning for kin's 3D vertex shaders (docs/3d_plan.md, P1a), shared by mesh_skinned.vert, mesh_skinned_velocity.vert,
// shadow_skinned.vert, shadow_mask_skinned.vert, prepass_skinned.vert and prepass_velocity_skinned.vert. The skin stream is a second vertex buffer (location 4: four joint
// indices, a byte each; location 5: four weights, unorm bytes that sum to 1), the joints' matrices one storage buffer for
// the frame (world pose times inverse bind, mesh space to model space), and each copy's first matrix a u32 in a buffer
// of its own, indexed as Instances are (so GpuInstance3D stays 128 B). Define KIN_SKIN_BINDING (the first of the two
// storage buffers' bindings in set 0) before including it. With KIN_SKIN_PREVIOUS (the velocity variants) two more follow
// at KIN_SKIN_BINDING + 2 and + 3: the joint matrices of the frame before and, indexed as the copies are, where each
// copy's own start there (a copy with no past pose: all ones, and then skin_matrix_previous is the current matrix).
layout(location = 4) in uvec4 aJoints;
layout(location = 5) in vec4 aWeights;

layout(std430, set = 0, binding = KIN_SKIN_BINDING) readonly buffer Joints {
    mat4 joints[];
};
layout(std430, set = 0, binding = KIN_SKIN_BINDING + 1) readonly buffer JointOffsets {
    uint joint_offsets[];
};

// The weighted sum of the vertex's joint matrices: position = this * vec4(position, 1) is the sum of the weights times
// each joint's matrix times the position. Its linear part also turns normals: right for rigid and uniformly scaled
// joints; a non-uniform scale in a joint is only approximated (the proper matrix is the inverse transpose of the sum).
// `slot` is the copy's place in Instances.
mat4 skin_matrix(uint slot) {
    const uint first = joint_offsets[slot];
    return joints[first + aJoints.x] * aWeights.x + joints[first + aJoints.y] * aWeights.y +
           joints[first + aJoints.z] * aWeights.z + joints[first + aJoints.w] * aWeights.w;
}

#ifdef KIN_SKIN_PREVIOUS
layout(std430, set = 0, binding = KIN_SKIN_BINDING + 2) readonly buffer PreviousJoints {
    mat4 previous_joints[];
};
layout(std430, set = 0, binding = KIN_SKIN_BINDING + 3) readonly buffer PreviousJointOffsets {
    uint previous_joint_offsets[];
};

// The same sum over the copy's pose of the frame before (a copy with none: `current`, so only the camera and the copy's
// own placing move it).
mat4 skin_matrix_previous(uint slot, mat4 current) {
    const uint first = previous_joint_offsets[slot];
    if (first == 0xffffffffu) {
        return current;
    }
    return previous_joints[first + aJoints.x] * aWeights.x + previous_joints[first + aJoints.y] * aWeights.y +
           previous_joints[first + aJoints.z] * aWeights.z + previous_joints[first + aJoints.w] * aWeights.w;
}
#endif
