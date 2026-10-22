#pragma once
#include <kin/core/types.hpp>

namespace kin::detail {
struct SceneIdentity3D {
    u64 session = 0, generation = 0;
};
struct SceneLease3D {
    u64 instance = 0;
    bool active = false;
};
} // namespace kin::detail
