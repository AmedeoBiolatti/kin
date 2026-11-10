// Scales: where values sit along an axis, and the round values to mark on it.

#include <kin/ui2/scale.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <span>

namespace kin::ui2 {
namespace {

// The least of 1, 2 and 5 times a power of ten that is at least `raw`.
f64 nice_step(f64 raw) {
    if (!(raw > 0.0) || !std::isfinite(raw)) {
        return 1.0;
    }
    const f64 power = std::pow(10.0, std::floor(std::log10(raw)));
    for (const f64 multiple : {1.0, 2.0, 5.0}) {
        if (multiple * power >= raw * (1.0 - 1e-9)) {
            return multiple * power;
        }
    }
    return 10.0 * power;
}

// SI's units that take k, M and G (a number alone takes them too).
bool takes_prefix(std::string_view unit) {
    static constexpr std::array<std::string_view, 12> units{"Hz", "B", "b", "bit", "s", "m", "g", "W", "V", "A", "J", "N"};
    return unit.empty() || std::ranges::find(units, unit) != units.end();
}

f64 log_of(f64 value) {
    return std::log10(std::max(value, std::numeric_limits<f64>::min()));
}

void linear_ticks(std::vector<Tick>& out, f64 min, f64 max, i32 count, std::string_view unit) {
    const f64 step = nice_step((max - min) / static_cast<f64>(std::max(1, count - 1)));
    const f64 first = std::ceil(min / step - 1e-9);
    for (i32 i = 0; i < 1000; ++i) {
        f64 value = (first + static_cast<f64>(i)) * step;
        if (value > max + step * 1e-9) {
            break;
        }
        if (std::abs(value) < step * 1e-9) {
            value = 0.0;
        }
        out.push_back({.value = value, .label = tick_label(value, step, unit)});
    }
}

} // namespace

f64 Scale::fraction(f64 v) const {
    if (!valid()) {
        return 0.0;
    }
    if (kind == ScaleKind::Log) {
        const f64 lo = log_of(min);
        return (log_of(v) - lo) / (log_of(max) - lo);
    }
    return (v - min) / (max - min);
}

f64 Scale::value(f64 fraction) const {
    if (kind == ScaleKind::Log && valid()) {
        const f64 lo = log_of(min);
        return std::pow(10.0, lo + fraction * (log_of(max) - lo));
    }
    return min + fraction * (max - min);
}

bool Scale::valid() const {
    return std::isfinite(min) && std::isfinite(max) && max > min && (kind != ScaleKind::Log || min > 0.0);
}

std::vector<Tick> ticks(const Scale& scale, i32 count, std::string_view unit) {
    std::vector<Tick> out;
    if (!scale.valid() || count < 1) {
        return out;
    }
    count = std::max(count, 2);
    if (scale.kind == ScaleKind::Linear) {
        linear_ticks(out, scale.min, scale.max, count, unit);
        return out;
    }
    // The powers of ten within it, with 2 and 5 times them while they fit;
    // every second (third...) power when even those are too many.
    const i32 low = static_cast<i32>(std::floor(log_of(scale.min) + 1e-9));
    const i32 high = static_cast<i32>(std::ceil(log_of(scale.max) - 1e-9));
    const auto within = [&](f64 v) { return v >= scale.min * (1.0 - 1e-9) && v <= scale.max * (1.0 + 1e-9); };
    const auto gather = [&](std::span<const f64> multiples, i32 every) {
        std::vector<f64> values;
        for (i32 e = low; e <= high; ++e) {
            if ((e - low) % every != 0 && multiples.size() == 1) {
                continue;
            }
            for (const f64 m : multiples) {
                const f64 v = m * std::pow(10.0, e);
                if (within(v)) {
                    values.push_back(v);
                }
            }
        }
        return values;
    };
    static constexpr std::array<f64, 3> one_two_five{1.0, 2.0, 5.0};
    static constexpr std::array<f64, 1> ones{1.0};
    std::vector<f64> values = gather(one_two_five, 1);
    if (static_cast<i32>(values.size()) > count) {
        values = gather(ones, 1);
        for (i32 every = 2; static_cast<i32>(values.size()) > count; ++every) {
            values = gather(ones, every);
        }
    }
    if (values.size() < 2) {
        linear_ticks(out, scale.min, scale.max, count, unit); // less than a tenfold step: evenly
        return out;
    }
    for (const f64 v : values) {
        out.push_back({.value = v, .label = tick_label(v, v, unit)});
    }
    return out;
}

std::string tick_label(f64 value, f64 step, std::string_view unit, bool short_thousands) {
    static constexpr std::array<std::pair<f64, const char*>, 3> prefixes{{{1e9, "G"}, {1e6, "M"}, {1e3, "k"}}};
    step = std::abs(step);
    f64 divisor = 1.0;
    const char* prefix = "";
    for (const auto& [size, name] : prefixes) {
        if (short_thousands && takes_prefix(unit) && std::abs(value) >= size) {
            divisor = size;
            prefix = name;
            break;
        }
    }
    const f64 shown = value / divisor;
    const f64 shown_step = step > 0.0 ? step / divisor : 1.0;
    const i32 decimals = std::clamp(static_cast<i32>(std::ceil(-std::log10(shown_step) - 1e-9)), 0, 9);
    char text[64];
    std::snprintf(text, sizeof text, "%.*f", decimals, std::abs(shown) < 0.5 * std::pow(10.0, -decimals) ? 0.0 : shown);
    std::string label = text;
    if (*prefix != '\0' && label.find('.') != std::string::npos) {
        // 1.0k is 1k (beside 1.5k).
        while (label.back() == '0') {
            label.pop_back();
        }
        if (label.back() == '.') {
            label.pop_back();
        }
    }
    if (!unit.empty()) {
        label += ' ';
        label += prefix;
        label += unit;
    } else {
        label += prefix;
    }
    return label;
}

Scale nice_scale(f64 min, f64 max, ScaleKind kind, i32 count) {
    if (!std::isfinite(min) || !std::isfinite(max)) {
        return kind == ScaleKind::Log ? Scale{.min = 1.0, .max = 10.0, .kind = kind} : Scale{};
    }
    if (min > max) {
        std::swap(min, max);
    }
    if (kind == ScaleKind::Log) {
        if (max <= 0.0) {
            return {.min = 1.0, .max = 10.0, .kind = kind};
        }
        min = min > 0.0 ? min : max / 1000.0;
        const f64 lo = std::pow(10.0, std::floor(log_of(min) + 1e-9));
        f64 hi = std::pow(10.0, std::ceil(log_of(max) - 1e-9));
        if (hi <= lo) {
            hi = lo * 10.0;
        }
        return {.min = lo, .max = hi, .kind = kind};
    }
    if (max - min <= std::max(std::abs(max), std::abs(min)) * 1e-12) {
        const f64 pad = min == 0.0 ? 1.0 : std::abs(min) * 0.1;
        min -= pad;
        max += pad;
    }
    const f64 step = nice_step((max - min) / static_cast<f64>(std::max(1, count - 1)));
    return {.min = std::floor(min / step + 1e-9) * step, .max = std::ceil(max / step - 1e-9) * step, .kind = kind};
}

} // namespace kin::ui2
