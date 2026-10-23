#pragma once
#include <kin/ecs/physics3d.hpp>

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#else
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
inline void legacy_interpolate(kin::WorldPhysicsState3D& bodies, kin::f32 alpha) {
    bodies.interpolate(alpha);
}
#if defined(_MSC_VER)
#pragma warning(pop)
#else
#pragma GCC diagnostic pop
#endif
