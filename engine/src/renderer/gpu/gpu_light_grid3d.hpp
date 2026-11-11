#pragma once

#include <array>
#include <bit>
#include <cassert>
#include <kin/core/types.hpp>
#include <span>
#include <vector>

namespace kin::gpu {

// A light of the light grid (as gi_light.comp's): place and reach, colour and the square of the size, a spot's way and
// the cosine of its outer angle (a point light: -2), 1 / (cos inner - cos outer) and its shadow's layer (a point
// light's cube's first; -1: none).
struct GridLight {
    f32 at[4], colour[4], axis[4], cone[4];
};
static_assert(sizeof(GridLight) == 64);

struct LightGridBox {
    u32 x0 = 1, x1 = 0, y0 = 1, y1 = 0, z0 = 1, z1 = 0; // inclusive; x0 > x1 means empty
};

// Exact CPU construction of the shader's cell headers and ordered light IDs.
// Inputs are projected boxes inside the supplied nonzero grid dimensions, in
// point-then-spot order. One box per light; at most 256 of either kind.
struct LightGridLists {
    static constexpr u32 max_lights = 512, max_count = 0xfff, max_indices = 1u << 20;
    static_assert(max_lights <= max_count);
    std::vector<u32> data;
    std::vector<u64> axes; // X, Y, Z slabs, each with ceil(light count / 64) words

    // False only on index overflow. Empty input produces the existing one-word
    // dummy binding; nonempty input with no covered cells keeps zero headers.
    bool build(std::span<const LightGridBox> boxes, u32 across, u32 down, u32 slices) {
        assert(across && down && slices && boxes.size() <= max_lights);
        u64 total = 0;
        for (const auto& b : boxes)
            if (b.x0 <= b.x1) {
                assert(b.x1 < across && b.y0 <= b.y1 && b.y1 < down && b.z0 <= b.z1 && b.z1 < slices);
                total += u64(b.x1 - b.x0 + 1) * (b.y1 - b.y0 + 1) * (b.z1 - b.z0 + 1);
            }
        if (boxes.empty() || total >= max_indices) {
            data.assign(1, 0);
            return total < max_indices;
        }
        const u32 words = static_cast<u32>((boxes.size() + 63) / 64);
        axes.assign(std::size_t(across + down + slices) * words, 0);
        u64* xs = axes.data();
        u64* ys = xs + std::size_t(across) * words;
        u64* zs = ys + std::size_t(down) * words;
        for (u32 i = 0; i < boxes.size(); ++i) {
            const auto& b = boxes[i];
            if (b.x0 > b.x1)
                continue;
            const u32 word = i / 64;
            const u64 bit = u64{1} << (i % 64);
            for (u32 x = b.x0; x <= b.x1; ++x)
                xs[std::size_t(x) * words + word] |= bit;
            for (u32 y = b.y0; y <= b.y1; ++y)
                ys[std::size_t(y) * words + word] |= bit;
            for (u32 z = b.z0; z <= b.z1; ++z)
                zs[std::size_t(z) * words + word] |= bit;
        }
        const u32 cells = across * down * slices;
        // Resize without first shrinking to the headers: retained index words
        // are overwritten once, rather than zeroed before being filled.
        data.resize(cells + static_cast<std::size_t>(total));
        const auto emit = [&]<bool single_word>() {
            const u32 n = single_word ? 1u : words;
            u32 cell = 0;
            u32* const indices = data.data() + cells;
            u32* out = indices;
            std::array<u64, max_lights / 64> yz;
            for (u32 z = 0; z < slices; ++z)
                for (u32 y = 0; y < down; ++y) {
                    for (u32 w = 0; w < n; ++w)
                        yz[w] = ys[std::size_t(y) * words + w] & zs[std::size_t(z) * words + w];
                    for (u32 x = 0; x < across; ++x) {
                        const u32 first = static_cast<u32>(out - indices);
                        for (u32 w = 0; w < n; ++w) {
                            u64 bits = xs[std::size_t(x) * words + w] & yz[w];
                            while (bits) {
                                *out++ = 64 * w + std::countr_zero(bits);
                                bits &= bits - 1;
                            }
                        }
                        data[cell++] = (first << 12) | (static_cast<u32>(out - indices) - first);
                    }
                }
            assert(static_cast<u64>(out - indices) == total);
        };
        if (words == 1)
            emit.template operator()<true>();
        else
            emit.template operator()<false>();
        return true;
    }
};

} // namespace kin::gpu
