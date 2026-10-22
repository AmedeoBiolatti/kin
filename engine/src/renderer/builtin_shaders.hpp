#pragma once

#include <kin/core/types.hpp>

#include <span>
#include <string_view>

namespace kin::render_detail {

// Build-generated SPIR-V with static lifetime. Empty when the shader compiler
// was unavailable; no runtime access to the source or build tree is needed.
std::span<const u8> builtin_shader_bytes(std::string_view name);

} // namespace kin::render_detail
