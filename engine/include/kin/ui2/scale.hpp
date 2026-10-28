#pragma once

// Where values sit along a chart's axis or a control's travel: evenly
// (linear), or with each tenfold step as long (log: frequencies, levels), and
// the round values worth marking on it.

#include <kin/core/types.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace kin::ui2 {

enum class ScaleKind : u8 {
    Linear,
    Log, // each tenfold step as long; min and max above 0
};

struct Scale {
    f64 min = 0.0;
    f64 max = 1.0;
    ScaleKind kind = ScaleKind::Linear;

    // How far along `value` is: 0 at min, 1 at max (below 0 and above 1 outside them).
    f64 fraction(f64 value) const;
    // The value `fraction` along.
    f64 value(f64 fraction) const;
    // max above min (and min above 0, for Log).
    bool valid() const;

    friend bool operator==(const Scale&, const Scale&) = default;
};

struct Tick {
    f64 value = 0.0;
    std::string label;
};

// Round values to mark on `scale`, in order, at most about `count` of them:
// 1, 2 or 5 times a power of ten apart (linear), or the powers of ten, with 2
// and 5 times them while there is room (log). Each is labelled as
// tick_label() does, with `unit` after it.
std::vector<Tick> ticks(const Scale& scale, i32 count = 6, std::string_view unit = {});
// A value as an axis shows it: with the decimals marks `step` apart need (a
// log scale's: the value itself) and the unit after it ("40 ms"); thousands
// and up as k, M and G ("12k", "2 kHz", "1.5 MB") when `short_thousands`, the
// number alone or in a unit that takes them (Hz, B, b, bit, s, m, g, W, V, A,
// J, N: not "1500 ms"). Never "-0".
std::string tick_label(f64 value, f64 step, std::string_view unit = {}, bool short_thousands = true);
// [min, max] rounded out to round values: an axis fitted to data (0.13 to
// 0.88 is 0 to 1). An empty range is widened around its value.
Scale nice_scale(f64 min, f64 max, ScaleKind kind = ScaleKind::Linear, i32 count = 6);

} // namespace kin::ui2
