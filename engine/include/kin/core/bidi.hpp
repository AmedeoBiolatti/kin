#pragma once

#include <kin/core/types.hpp>

#include <cstddef>
#include <optional>
#include <string_view>
#include <vector>

namespace kin {

// Which way a paragraph or a run of text goes.
enum class TextDirection : u8 { LeftToRight, RightToLeft };

// The Unicode bidirectional algorithm (UAX #9) for UI text: what part of a
// line runs which way, and the order its parts are shown in. It follows the
// standard's rules for strong, weak and neutral characters (W1-W7, N1-N2,
// I1-I2), resets trailing whitespace (L1) and reorders (L2). The explicit
// embeddings, overrides and isolates (U+202A-202E, U+2066-2069) are honoured
// as embeddings and overrides; isolates are treated as embeddings. Mirrored
// brackets are left to the shaper.

// A run of one embedding level, as byte offsets into the line.
struct BidiRun {
    std::size_t begin = 0;
    std::size_t end = 0;
    u8 level = 0; // odd: right to left

    bool right_to_left() const { return (level & 1) != 0; }
    friend constexpr bool operator==(const BidiRun&, const BidiRun&) = default;
};

// Whether `text` holds anything that can run right to left (Hebrew, Arabic and
// the other right-to-left scripts, or explicit direction marks). Text without
// is one left-to-right run, so callers can skip the algorithm.
bool has_right_to_left(std::string_view text);

// The direction of the paragraph's first strong character (P2-P3), or
// `fallback` if it has none.
TextDirection first_strong_direction(std::string_view text, TextDirection fallback = TextDirection::LeftToRight);

// The runs of one line (no paragraph separators inside), in the order they
// are shown, left to right. `base` is the paragraph's direction; unset, its
// first strong character decides. An empty line has no runs.
std::vector<BidiRun> bidi_runs(std::string_view line, std::optional<TextDirection> base = std::nullopt);

} // namespace kin
