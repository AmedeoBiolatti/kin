#pragma once
#include <kin/core/types.hpp>
namespace kin {
// Version 2 appends independent RGB emission at byte 48 (64-byte material block).
// The first 48 bytes retain version 1 layout. Recompile custom shaders to use
// the new kin_emissive helper and emission fields.
inline constexpr u32 material_interface_version3d = 2;
} // namespace kin
