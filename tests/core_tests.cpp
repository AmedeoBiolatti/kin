#include <kin/core/profile.hpp>
#include <kin/core/rng.hpp>
#include <kin/core/types.hpp>

#include <cassert>
#include <limits>
#include <source_location>
#include <sstream>
#include <string>

int main() {
    static_assert(sizeof(kin::i8) == 1);
    static_assert(sizeof(kin::i16) == 2);
    static_assert(sizeof(kin::i32) == 4);
    static_assert(sizeof(kin::i64) == 8);
    static_assert(sizeof(kin::u8) == 1);
    static_assert(sizeof(kin::u16) == 2);
    static_assert(sizeof(kin::u32) == 4);
    static_assert(sizeof(kin::u64) == 8);

    constexpr kin::Vec2i tile{2, 3};
    static_assert(tile == kin::Vec2i{2, 3});

    const kin::RngKey root = kin::make_key(1234);
    const auto [next_a, sub_a] = kin::split(root);
    const auto [next_b, sub_b] = kin::split(root);

    assert(next_a == next_b);
    assert(sub_a == sub_b);
    assert(next_a != sub_a);

    const kin::i32 roll = kin::rng_i32(sub_a, 3, 9);
    assert(roll >= 3);
    assert(roll <= 9);

    const kin::f32 value = kin::rng_f32(sub_a, -1.0f, 1.0f);
    assert(value >= -1.0f);
    assert(value <= 1.0f);

    const kin::i32 full_range = kin::rng_i32(sub_a,
                                              std::numeric_limits<kin::i32>::min(),
                                              std::numeric_limits<kin::i32>::max());
    assert(full_range >= std::numeric_limits<kin::i32>::min());
    assert(full_range <= std::numeric_limits<kin::i32>::max());

    kin::ProfileSession profile;
    profile.start("core-test");
    profile.set_frame(7);
    const auto shared_location = std::source_location::current();
    profile.record("escaped\"scope", "unit", 1'000'000, shared_location);
    profile.record("escaped\"scope", "unit", 3'000'000, shared_location);
    profile.record("other", "unit", 2'000'000);
    {
        kin::ScopedProfile scope{&profile, "raii", "unit"};
    }
    KIN_PROFILE_SCOPE("macro-scope-compiles");
    KIN_PROFILE_FUNCTION();
    KIN_PROFILE_LINE("macro-line-compiles");
    profile.stop();

    const std::vector<kin::ProfileSummary> summaries = profile.summarize();
    assert(!summaries.empty());
    bool found_escaped = false;
    for (const kin::ProfileSummary& summary : summaries) {
        if (summary.name == "escaped\"scope") {
            found_escaped = true;
            assert(summary.calls == 2);
            assert(summary.total_ns == 4'000'000);
            assert(summary.median_ms == 3.0);
            assert(summary.p99_ms == 3.0);
        }
    }
    assert(found_escaped);

    std::ostringstream profile_text;
    kin::write_profile_text(profile_text, profile, 8, "test profile");
    assert(profile_text.str().find("test profile core-test") != std::string::npos);
    assert(profile_text.str().find("escaped\"scope") != std::string::npos);

    std::ostringstream profile_json;
    kin::write_profile_json(profile_json, profile, "kin.profile/1", 8);
    const std::string json = profile_json.str();
    assert(json.find("\"schema\": \"kin.profile/1\"") != std::string::npos);
    assert(json.find("escaped\\\"scope") != std::string::npos);
    assert(json.find("\"frame\": 7") != std::string::npos);

    profile.clear();
    assert(profile.events().empty());
    assert(profile.frame() == 0);

    return 0;
}
