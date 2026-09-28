#include <kin/anim/animation.hpp>
#include <kin/anim/registry.hpp>
#include <kin/anim/track.hpp>
#include <kin/anim/value.hpp>

#include <cassert>
#include <cmath>
#include <string>
#include <unordered_set>
#include <utility>
#include <variant>

namespace {

bool near(kin::f32 a, kin::f32 b) {
    return std::fabs(a - b) < 0.0001f;
}

kin::Clip simple_clip() {
    kin::PropertyTrack track;
    track.property = "SpriteRenderer.tint";
    track.keys = {
        {.time = 0.0f, .value = kin::Color{0, 0, 0, 255}},
        {.time = 1.0f, .value = kin::Color{255, 128, 64, 255}},
    };

    kin::SpriteTrack sprites;
    sprites.keys = {
        {.time = 0.0f, .sprite_id = "{X}.idle.0"},
        {.time = 0.5f, .sprite_id = "{X}.idle.1"},
    };

    kin::EventTrack events;
    events.keys = {
        {.time = 0.25f, .event = {.channel = "sound", .name = "step", .value = "{X}.step"}},
    };

    return kin::Clip{
        .duration = 1.0f,
        .properties = {std::move(track)},
        .sprites = {std::move(sprites)},
        .events = {std::move(events)},
    };
}

void interpolation_tests() {
    assert(std::get<kin::f32>(kin::interpolate(0.0f, 10.0f, 0.5f, kin::Easing::Linear)) == 5.0f);

    const kin::Vec2f vec = std::get<kin::Vec2f>(
        kin::interpolate(kin::Vec2f{0.0f, 10.0f}, kin::Vec2f{10.0f, 20.0f}, 0.5f, kin::Easing::Linear));
    assert(near(vec.x, 5.0f));
    assert(near(vec.y, 15.0f));

    const kin::Color color = std::get<kin::Color>(
        kin::interpolate(kin::Color{0, 0, 0, 0}, kin::Color{100, 200, 255, 10}, 0.5f, kin::Easing::Linear));
    assert((color == kin::Color{50, 100, 128, 5}));

    assert(std::get<kin::i32>(kin::interpolate(1, 2, 0.99f, kin::Easing::Linear)) == 1);
    assert(std::get<kin::i32>(kin::interpolate(1, 2, 1.0f, kin::Easing::Linear)) == 2);
    assert(std::get<bool>(kin::interpolate(false, true, 0.5f, kin::Easing::Linear)) == false);
    assert(std::get<std::string>(kin::interpolate(std::string{"a"}, std::string{"b"}, 1.0f, kin::Easing::Linear)) == "b");

    assert(near(kin::ease(kin::Easing::EaseIn, 0.0f), 0.0f));
    assert(near(kin::ease(kin::Easing::EaseIn, 1.0f), 1.0f));
    assert(kin::ease(kin::Easing::EaseIn, 0.25f) <= kin::ease(kin::Easing::EaseIn, 0.5f));
}

void track_tests() {
    kin::PropertyTrack track;
    track.property = "Transform2D.pos";
    track.keys = {
        {.time = 0.25f, .value = kin::Vec2f{1.0f, 2.0f}},
        {.time = 0.75f, .value = kin::Vec2f{3.0f, 6.0f}},
    };

    assert((std::get<kin::Vec2f>(kin::sample(track, 0.0f)) == kin::Vec2f{1.0f, 2.0f}));
    assert((std::get<kin::Vec2f>(kin::sample(track, 1.0f)) == kin::Vec2f{3.0f, 6.0f}));

    const kin::Vec2f mid = std::get<kin::Vec2f>(kin::sample(track, 0.5f));
    assert(near(mid.x, 2.0f));
    assert(near(mid.y, 4.0f));

    kin::SpriteTrack sprites;
    sprites.keys = {
        {.time = 0.2f, .sprite_id = "a"},
        {.time = 0.4f, .sprite_id = "b"},
    };
    assert(kin::active_sprite_key(sprites, 0.1f) == -1);
    assert(kin::active_sprite_key(sprites, 0.2f) == 0);
    assert(kin::active_sprite_key(sprites, 0.5f) == 1);
}

void validation_tests() {
    kin::Animation valid{
        .name = "valid",
        .root = kin::clip_node(simple_clip()),
    };

    std::string error;
    assert(kin::validate(valid, error));

    kin::Clip invalid_clip = simple_clip();
    invalid_clip.properties[0].keys = {
        {.time = 1.0f, .value = 1.0f},
        {.time = 0.0f, .value = 0.0f},
    };
    kin::Animation invalid{
        .name = "invalid",
        .root = kin::clip_node(std::move(invalid_clip)),
    };
    assert(!kin::validate(invalid, error));

    kin::Animation empty_ref{
        .name = "empty_ref",
        .root = kin::ref_node(""),
    };
    assert(!kin::validate(empty_ref, error));

    // An empty Parallel with the default (All) end is legal: `primary` is ignored
    // unless end == Primary.
    kin::Animation empty_parallel{
        .name = "empty_parallel",
        .root = kin::parallel_node({}, kin::ParallelEnd::All),
    };
    assert(kin::validate(empty_parallel, error));

    // A non-Primary Parallel with an out-of-range primary still validates.
    kin::AnimationNode stale = kin::parallel_node({}, kin::ParallelEnd::Any);
    {
        auto& parallel = std::get<kin::Parallel>(stale.value);
        parallel.children.push_back(kin::clip_node(simple_clip()));
        parallel.primary = 7;
    }
    kin::Animation stale_primary{.name = "stale_primary", .root = std::move(stale)};
    assert(kin::validate(stale_primary, error));

    // But a Primary Parallel must have an in-range primary index.
    kin::AnimationNode bad = kin::parallel_node({}, kin::ParallelEnd::Primary);
    {
        auto& parallel = std::get<kin::Parallel>(bad.value);
        parallel.children.push_back(kin::clip_node(simple_clip()));
        parallel.primary = 7;
    }
    kin::Animation bad_primary{.name = "bad_primary", .root = std::move(bad)};
    assert(!kin::validate(bad_primary, error));
}

void clone_and_termination_tests() {
    kin::AnimationNode finite = kin::repeat_node(kin::clip_node(simple_clip()), 2);
    kin::AnimationNode infinite = kin::repeat_node(kin::clip_node(simple_clip()), 0);
    assert(kin::terminates(finite));
    assert(!kin::terminates(infinite));

    kin::AnimationNode any = kin::parallel_node({}, kin::ParallelEnd::Any);
    auto& parallel = std::get<kin::Parallel>(any.value);
    parallel.children.push_back(kin::repeat_node(kin::clip_node(simple_clip()), 0));
    parallel.children.push_back(kin::clip_node(simple_clip()));
    assert(kin::terminates(any));

    kin::Animation original{
        .name = "clone",
        .root = kin::repeat_node(kin::ref_node("child"), 3),
    };
    kin::Animation copied = kin::clone(original);
    assert(copied.name == original.name);
    assert(kin::terminates(copied.root));
}

void instantiate_tests() {
    kin::Animation tmpl{
        .name = "{X}.idle",
        .root = kin::clip_node(simple_clip()),
        .is_template = true,
    };

    kin::Animation out;
    std::string error;
    const kin::Bindings goblin_bindings{{"X", "goblin"}};
    assert(kin::instantiate(tmpl, goblin_bindings, out, error));
    assert(out.name == "goblin.idle");
    const auto& clip = std::get<kin::Clip>(out.root.value);
    assert(clip.sprites[0].keys[0].sprite_id == "goblin.idle.0");
    assert(clip.events[0].keys[0].event.value == "goblin.step");

    kin::Animation unresolved;
    const kin::Bindings empty_bindings;
    assert(!kin::instantiate(tmpl, empty_bindings, unresolved, error));

    std::string substituted;
    assert(kin::substitute("{X}.walk", goblin_bindings, substituted, error));
    assert(substituted == "goblin.walk");
    assert(!kin::substitute("{Y}.walk", goblin_bindings, substituted, error));
}

void registry_tests() {
    kin::AnimationRegistry registry;
    registry.add_template(kin::Animation{
        .name = "{X}.idle",
        .root = kin::clip_node(simple_clip()),
        .is_template = true,
    });

    const auto first = registry.resolve("goblin.idle", {{"X", "goblin"}});
    const auto second = registry.resolve("goblin.idle", {{"X", "goblin"}});
    assert(first);
    assert(first == second);
    assert(first->name == "goblin.idle");

    std::string error;
    const kin::Bindings orc_bindings{{"X", "orc"}};
    assert(registry.expand("{X}.idle", orc_bindings, error));
    const auto expanded = registry.resolve("orc.idle", orc_bindings);
    assert(expanded);
    assert(expanded->name == "orc.idle");

    const kin::Bindings troll_bindings{{"X", "troll"}};
    const std::unordered_set<std::string> valid_sprites{
        "troll.idle.0",
        "troll.idle.1",
    };
    assert(registry.expand("{X}.idle", troll_bindings, error, [&](std::string_view sprite_id) {
        return valid_sprites.contains(std::string{sprite_id});
    }));
    assert(registry.resolve("troll.idle"));

    const kin::Bindings missing_bindings{{"X", "missing"}};
    assert(!registry.expand("{X}.idle", missing_bindings, error, [&](std::string_view) {
        return false;
    }));
    assert(error.find("missing sprite") != std::string::npos);
    assert(!registry.resolve("missing.idle"));

    const kin::Bindings empty_bindings;
    assert(!registry.expand("{X}.idle", empty_bindings, error));
    assert(error.find("unresolved placeholder") != std::string::npos);
}

} // namespace

int main() {
    interpolation_tests();
    track_tests();
    validation_tests();
    clone_and_termination_tests();
    instantiate_tests();
    registry_tests();
    return 0;
}
