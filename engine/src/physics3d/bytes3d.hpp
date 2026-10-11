#pragma once

// Little-endian bytes in and out for the state of ragdolls and ropes (hash, save and restore). Internal.

#include <kin/core/math3d.hpp>
#include <kin/core/types.hpp>

#include <bit>
#include <vector>

namespace kin::physics3d_detail {

struct ByteOut {
    std::vector<u8>& out;
    void u8v(u8 v) { out.push_back(v); }
    void b(bool v) { out.push_back(v ? 1 : 0); }
    void u32v(u32 v) {
        for (int i = 0; i < 4; ++i) out.push_back(static_cast<u8>(v >> (8 * i)));
    }
    void u64v(u64 v) {
        u32v(static_cast<u32>(v));
        u32v(static_cast<u32>(v >> 32));
    }
    void f32v(f32 v) { u32v(std::bit_cast<u32>(v)); }
    void vec(Vec3f v) {
        f32v(v.x);
        f32v(v.y);
        f32v(v.z);
    }
    void quat(Quatf q) {
        f32v(q.x);
        f32v(q.y);
        f32v(q.z);
        f32v(q.w);
    }
};

struct ByteIn {
    const u8* p;
    const u8* end;
    bool failed = false;
    u8 u8v() {
        if (end - p < 1) {
            failed = true;
            return 0;
        }
        return *p++;
    }
    bool b() { return u8v() != 0; }
    u32 u32v() {
        if (end - p < 4) {
            failed = true;
            return 0;
        }
        u32 v = 0;
        for (int i = 0; i < 4; ++i) v |= static_cast<u32>(p[i]) << (8 * i);
        p += 4;
        return v;
    }
    u64 u64v() {
        const u64 lo = u32v();
        const u64 hi = u32v();
        return lo | hi << 32;
    }
    f32 f32v() { return std::bit_cast<f32>(u32v()); }
    Vec3f vec() {
        Vec3f v;
        v.x = f32v();
        v.y = f32v();
        v.z = f32v();
        return v;
    }
    Quatf quat() {
        Quatf q;
        q.x = f32v();
        q.y = f32v();
        q.z = f32v();
        q.w = f32v();
        return q;
    }
};

} // namespace kin::physics3d_detail
