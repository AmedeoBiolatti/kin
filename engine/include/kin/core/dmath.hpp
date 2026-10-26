#pragma once

// Deterministic math: the same bits from the same input with every compiler, C library and CPU kin builds for. A
// simulation that peers must agree on (docs/networking.md) uses these instead of std::sin and friends, whose
// implementations differ between C libraries (glibc, the MSVC runtime, Apple's) in the last bit, and a last bit is
// enough for two simulations to drift apart.
//
// They are Cephes' single-precision polynomials, made only of +, -, *, / (and exact steps: conversion to integer,
// std::fmod, std::sqrt), compiled without fused multiply-adds (-ffp-contract=off) in kin's own build, so no compiler
// flag of the game's changes them. Accuracy is about 1 to 2 ulp: sin, cos and tan within 3e-7 of the true value for
// |x| up to 8192 (and still deterministic past it), the inverse functions within 4e-7 everywhere.
//
// std::sqrt, std::fabs, std::fmod, std::floor, +, -, * and / need no replacement: IEEE 754 defines their results
// exactly. Results for -0, infinities and NaN follow the C functions' signs; every NaN result is the same quiet NaN.

#include <kin/core/types.hpp>

namespace kin::dmath {

f32 sin(f32 x);
f32 cos(f32 x);
void sincos(f32 x, f32& sin_out, f32& cos_out);
f32 tan(f32 x);
f32 asin(f32 x);
f32 acos(f32 x);
f32 atan(f32 x);
f32 atan2(f32 y, f32 x);

} // namespace kin::dmath
