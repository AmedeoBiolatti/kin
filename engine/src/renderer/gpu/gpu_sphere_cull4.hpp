#pragma once
// Four independent spheres from the existing AoS bounds cache. No copy/cache allocation.
#include <kin/core/math3d.hpp>
#include <array>
#include <cassert>
#include <span>

// Keep scalar boundary arithmetic on targets where contraction/reassociation may
// differ. Other targets retain the original gather; the portable helper remains testable.
#if defined(__SSE2__) && defined(__SSE_MATH__) && !defined(__FMA__) && !defined(__FAST_MATH__) && !defined(_MSC_VER) && !defined(KIN_SCALAR_SPHERES4)
#define KIN_NATIVE_SPHERES4 1
#include <xmmintrin.h>
#else
#define KIN_NATIVE_SPHERES4 0
#endif

namespace kin::gpu::sphere_cull4 {
inline constexpr bool available = KIN_NATIVE_SPHERES4 != 0;

class Test {
public:
    Test(std::span<const Vec4f> planes, Vec3f centre, f32 radius, bool bounded)
#if KIN_NATIVE_SPHERES4
        : _count(static_cast<u32>(planes.size())), _bounded(bounded) {
        assert(planes.size() <= _planes.size());
        if (bounded) {
            _x = _mm_set1_ps(centre.x); _y = _mm_set1_ps(centre.y); _z = _mm_set1_ps(centre.z); _radius = _mm_set1_ps(radius);
        }
        for (u32 k = 0; k < _count; ++k) {
            const auto& p = planes[k];
            _planes[k] = {_mm_set1_ps(p.x), _mm_set1_ps(p.y), _mm_set1_ps(p.z), _mm_set1_ps(p.w)};
        }
    }
#else
        : _planes(planes), _centre(centre), _radius(radius), _bounded(bounded) {}
#endif

    // Exactly four valid records, lower four bits in source order. A rejection
    // comparison is ordered (< or >), so NaNs stay accepted as in the scalar path.
    u32 mask(const Vec4f* spheres) const {
#if KIN_NATIVE_SPHERES4
        static_assert(sizeof(Vec4f) == 4 * sizeof(f32));
        __m128 x = _mm_loadu_ps(&spheres[0].x), y = _mm_loadu_ps(&spheres[1].x);
        __m128 z = _mm_loadu_ps(&spheres[2].x), r = _mm_loadu_ps(&spheres[3].x);
        _MM_TRANSPOSE4_PS(x, y, z, r);
        const __m128 zero = _mm_setzero_ps();
        __m128 alive = _mm_cmpeq_ps(zero, zero);
        u32 bits = 15;
        if (_bounded) {
            const __m128 dx = _mm_sub_ps(x, _x), dy = _mm_sub_ps(y, _y), dz = _mm_sub_ps(z, _z);
            const __m128 distance = _mm_add_ps(_mm_add_ps(_mm_mul_ps(dx, dx), _mm_mul_ps(dy, dy)), _mm_mul_ps(dz, dz));
            const __m128 reach = _mm_add_ps(r, _radius);
            alive = _mm_andnot_ps(_mm_cmpgt_ps(distance, _mm_mul_ps(reach, reach)), alive);
            bits = static_cast<u32>(_mm_movemask_ps(alive));
            if (!bits) return 0;
        }
        const __m128 negative_r = _mm_xor_ps(r, _mm_set1_ps(-0.0f));
        for (u32 k = 0; k < _count; ++k) {
            const auto& p = _planes[k];
            const __m128 distance = _mm_add_ps(_mm_add_ps(_mm_add_ps(_mm_mul_ps(p.x, x), _mm_mul_ps(p.y, y)), _mm_mul_ps(p.z, z)), p.w);
            alive = _mm_andnot_ps(_mm_cmplt_ps(distance, negative_r), alive);
            bits = static_cast<u32>(_mm_movemask_ps(alive));
            if (!bits) return 0;
        }
        return bits;
#else
        u32 bits = 0;
        for (u32 i = 0; i < 4; ++i) {
            const Vec4f& sp = spheres[i]; const Vec3f c{sp.x, sp.y, sp.z};
            if (_bounded) {
                const Vec3f d = c - _centre; const f32 reach = sp.w + _radius;
                if (dot(d, d) > reach * reach) continue;
            }
            bool accepted = true;
            for (const auto& p : _planes)
                if (p.x * c.x + p.y * c.y + p.z * c.z + p.w < -sp.w) { accepted = false; break; }
            if (accepted) bits |= 1u << i;
        }
        return bits;
#endif
    }
private:
#if KIN_NATIVE_SPHERES4
    struct Plane { __m128 x, y, z, w; };
    std::array<Plane, 6> _planes;
    __m128 _x, _y, _z, _radius;
    u32 _count;
    bool _bounded;
#else
    std::span<const Vec4f> _planes;
    Vec3f _centre;
    f32 _radius;
    bool _bounded;
#endif
};
} // namespace kin::gpu::sphere_cull4
#undef KIN_NATIVE_SPHERES4
