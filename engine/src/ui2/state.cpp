#include <kin/ui2/state.hpp>

namespace kin::ui2 {
namespace {

constexpr u64 fnv_offset = 1469598103934665603ull;
constexpr u64 fnv_prime = 1099511628211ull;

u64 fnv1a(std::string_view text, u64 seed) {
    u64 hash = seed;
    for (const char c : text) {
        hash ^= static_cast<u64>(static_cast<u8>(c));
        hash *= fnv_prime;
    }
    return hash;
}

u64 mix_u64(u64 seed, u64 value) {
    u64 hash = seed;
    for (i32 i = 0; i < 8; ++i) {
        hash ^= (value >> (i * 8)) & 0xffull;
        hash *= fnv_prime;
    }
    return hash;
}

Id non_zero(u64 hash) {
    return {hash == 0 ? 1ull : hash};
}

} // namespace

Id make_id(std::string_view label) {
    return non_zero(fnv1a(label, fnv_offset));
}

Id make_id(Id parent, std::string_view label) {
    return non_zero(fnv1a(label, parent.value != 0 ? parent.value : fnv_offset));
}

Id make_id(Id parent, u64 value) {
    return non_zero(mix_u64(parent.value != 0 ? parent.value : fnv_offset, value));
}

void State::begin_frame() {
    _hot = _has_hit ? _hit_id : Id{};
    _has_hit = false;
    _hit_id = {};
    _hit_z = 0;
    _focused_this_frame = false;
    ++_frame;
}

void State::clear() {
    _frame = 0;
    _hot = {};
    _active = {};
    _active_button = MouseButton::Left;
    _focused = {};
    _focused_this_frame = false;
    _hit_id = {};
    _hit_z = 0;
    _has_hit = false;
}

void State::register_hit(Id id, i32 z) {
    if (!id) {
        return;
    }
    // >= so a region drawn later at equal depth wins (last == topmost).
    if (!_has_hit || z >= _hit_z) {
        _hit_id = id;
        _hit_z = z;
        _has_hit = true;
    }
}

} // namespace kin::ui2
