#pragma once

// What a compiled fragment shader declares, read from its SPIR-V: the slots
// kin binds (sampled textures and storage buffers in set 2, the uniform block
// in set 3) and the uniform block's members by name, so a ShaderDesc needs no
// hand-kept counts and ShaderParams can be set by name.

#include <kin/core/types.hpp>
#include <kin/renderer/shader.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace kin {

// A member of the shader's uniform block: where it sits, in bytes.
struct ShaderParamInfo {
    std::string name;
    u32 offset = 0;
    u32 size = 0; // a whole array's, for an array
};

struct ShaderLayout {
    u32 samplers = 0;         // sampled textures, set 2 bindings 0..
    u32 storage_textures = 0; // set 2, after the samplers
    u32 storage_buffers = 0;  // set 2, after those
    u32 uniform_buffers = 0;  // set 3
    u32 uniform_bytes = 0;    // the uniform block's size
    std::vector<ShaderParamInfo> params;

    const ShaderParamInfo* find(std::string_view name) const;
};

// Null when `spirv` is not a SPIR-V module kin can read; `error` says why.
std::optional<ShaderLayout> reflect_spirv(ShaderBlob spirv, std::string* error = nullptr);

} // namespace kin
