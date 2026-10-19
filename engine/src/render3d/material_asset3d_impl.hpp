#pragma once

#include <kin/render3d/material_asset3d.hpp>

namespace kin {

// Internal builder: Assets3D::load_materials translates validation failures
// into its public diagnostics contract before publishing or caching a binding.
MaterialBinding3D build_materials3d_binding(const Materials3DAsset& asset, const std::shared_ptr<const Model>& model,
                                         std::string_view variant, Assets3D& assets);

} // namespace kin
