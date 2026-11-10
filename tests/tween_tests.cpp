// Easing curves, tweens of components' fields (kin/ecs/tween.hpp) and Timers
// (kin/core/timers.hpp).

#include <kin/anim/value.hpp>
#include <kin/core/timers.hpp>
#include <kin/ecs/render.hpp>
#include <kin/ecs/tween.hpp>
#include <kin/ecs/world.hpp>
#include <kin/scene/scene_asset.hpp>

#include <cassert>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

bool near(kin::f32 a, kin::f32 b, kin::f32 epsilon = 1e-3f) {
    return std::fabs(a - b) <= epsilon;
}

void test_easings() {
    assert(kin::easings().size() == 20);
    for (const kin::Easing easing : kin::easings()) {
        // From where it was to where it goes; named, and read back by name.
        assert(near(kin::ease(easing, 0.0f), 0.0f) && near(kin::ease(easing, 1.0f), 1.0f));
        kin::Easing read = kin::Easing::Step;
        assert(kin::parse_easing(kin::easing_name(easing), read) && read == easing);
    }
    kin::Easing unknown = kin::Easing::Linear;
    assert(!kin::parse_easing("wobbly", unknown) && unknown == kin::Easing::Linear);
    assert(kin::easing_name(kin::Easing::BounceOut) == "bounceout");
    // Back overshoots and comes back; Elastic springs past; Bounce stays within.
    assert(kin::ease(kin::Easing::BackOut, 0.6f) > 1.0f && kin::ease(kin::Easing::BackIn, 0.3f) < 0.0f);
    assert(kin::ease(kin::Easing::ElasticOut, 0.2f) > 1.0f);
    for (int i = 0; i <= 100; ++i) {
        const kin::f32 t = static_cast<kin::f32>(i) / 100.0f;
        assert(kin::ease(kin::Easing::BounceOut, t) >= -1e-4f && kin::ease(kin::Easing::BounceOut, t) <= 1.0f + 1e-4f);
    }
    assert(near(kin::ease(kin::Easing::SineInOut, 0.5f), 0.5f) && near(kin::ease(kin::Easing::CubicInOut, 0.5f), 0.5f));
    assert(near(kin::ease(kin::Easing::CubicIn, 0.5f), 0.125f) && near(kin::ease(kin::Easing::CubicOut, 0.5f), 0.875f));
    // A colour overshooting stays a colour.
    const kin::AnimValue past = kin::interpolate(kin::Color{0, 0, 0, 0}, kin::Color{250, 250, 250, 250}, 0.6f, kin::Easing::BackOut);
    assert(std::get<kin::Color>(past).r == 255);
}

void test_tweens() {
    kin::EcsWorld world;
    kin::register_builtin_editor_components(world);
    kin::EcsEntity card = world.entity("Card").set(kin::Transform2D{}).set(kin::RectRenderer{.size = {10.0f, 10.0f}});

    // A number, linearly: halfway at half time; at its end it is there, and has ended.
    const kin::u32 turn = kin::tween(world, card, {.property = "Transform2D.rotation", .to = 90.0f, .seconds = 1.0f, .easing = kin::Easing::Linear});
    assert(turn != 0 && kin::tweening(card) && kin::tweening(card, "Transform2D.rotation") && !kin::tweening(card, "Transform2D.pos"));
    kin::update_tweens(world, 0.5f);
    assert(near(card.get<kin::Transform2D>()->rotation, 45.0f));
    kin::update_tweens(world, 0.6f);
    assert(card.get<kin::Transform2D>()->rotation == 90.0f);
    assert(!kin::tweening(card) && card.get<kin::Tweens>()->ended == std::vector<kin::u32>{turn});
    kin::update_tweens(world, 0.1f);
    assert(card.get<kin::Tweens>()->ended.empty()); // told once

    // A pair, eased out: three quarters there at half time.
    kin::tween(world, card, {.property = "Transform2D.pos", .to = kin::Vec2f{100.0f, 50.0f}, .seconds = 2.0f});
    kin::update_tweens(world, 1.0f);
    assert(near(card.get<kin::Transform2D>()->pos.x, 75.0f) && near(card.get<kin::Transform2D>()->pos.y, 37.5f));
    // A new tween of the field replaces it, from where it is.
    const kin::u32 back = kin::tween(world, card, {.property = "Transform2D.pos", .to = kin::Vec2f{}, .seconds = 1.0f, .easing = kin::Easing::Linear});
    assert(card.get<kin::Tweens>()->running.size() == 1);
    kin::update_tweens(world, 0.5f);
    assert(near(card.get<kin::Transform2D>()->pos.x, 37.5f));
    assert(kin::stop_tween(card, back) && !kin::tweening(card));
    kin::update_tweens(world, 1.0f);
    assert(near(card.get<kin::Transform2D>()->pos.x, 37.5f) && card.get<kin::Tweens>()->ended.empty()); // stopped, not ended

    // A colour; a whole number (at its end); a number for a whole number.
    kin::tween(world, card, {.property = "RectRenderer.color", .to = kin::Color{0, 0, 0, 0}, .seconds = 1.0f, .easing = kin::Easing::Linear});
    kin::tween(world, card, {.property = "RectRenderer.order", .to = 7.0f, .seconds = 1.0f});
    kin::update_tweens(world, 0.5f);
    assert(card.get<kin::RectRenderer>()->color.a == 128 && card.get<kin::RectRenderer>()->order == 0);
    kin::update_tweens(world, 0.5f);
    assert(card.get<kin::RectRenderer>()->color == (kin::Color{0, 0, 0, 0}) && card.get<kin::RectRenderer>()->order == 7);

    // A delay: it waits, then starts from what the field is then; relative: added to it.
    card.set(kin::Transform2D{.pos = {0.0f, 0.0f}});
    kin::tween(world, card, {.property = "Transform2D.pos", .to = kin::Vec2f{10.0f, 0.0f}, .seconds = 1.0f, .easing = kin::Easing::Linear,
                             .delay = 0.5f, .relative = true});
    kin::update_tweens(world, 0.25f);
    card.set(kin::Transform2D{.pos = {100.0f, 0.0f}});
    kin::update_tweens(world, 0.25f);
    assert(card.get<kin::Transform2D>()->pos.x == 100.0f);
    kin::update_tweens(world, 0.5f);
    assert(near(card.get<kin::Transform2D>()->pos.x, 105.0f));
    kin::update_tweens(world, 0.5f);
    assert(card.get<kin::Transform2D>()->pos.x == 110.0f && !kin::tweening(card));

    // Again and back (yoyo), once more: there and back; on and on (-1).
    card.set(kin::RectRenderer{.size = {0.0f, 0.0f}});
    const kin::u32 pulse = kin::tween(world, card, {.property = "RectRenderer.size", .to = kin::Vec2f{10.0f, 10.0f}, .seconds = 1.0f,
                                                    .easing = kin::Easing::Linear, .repeats = 1, .yoyo = true});
    kin::update_tweens(world, 1.5f);
    assert(near(card.get<kin::RectRenderer>()->size.x, 5.0f) && kin::tweening(card));
    kin::update_tweens(world, 0.6f);
    assert(card.get<kin::RectRenderer>()->size.x == 0.0f && card.get<kin::Tweens>()->ended == std::vector<kin::u32>{pulse});
    kin::tween(world, card, {.property = "RectRenderer.size", .to = kin::Vec2f{10.0f, 10.0f}, .seconds = 0.5f, .repeats = -1});
    for (int i = 0; i < 100; ++i) {
        kin::update_tweens(world, 0.3f);
    }
    assert(kin::tweening(card, "RectRenderer.size"));
    assert(kin::stop_tweens(card) == 1 && kin::stop_tweens(card) == 0);

    // What can't be tweened says why.
    std::string why;
    assert(kin::tween(world, card, {.property = "Transform2D"}, &why) == 0 && why.find("Component.field") != std::string::npos);
    assert(kin::tween(world, card, {.property = "Nothing.at"}, &why) == 0 && why.find("Nothing") != std::string::npos);
    assert(kin::tween(world, card, {.property = "Transform2D.spin"}, &why) == 0 && why.find("spin") != std::string::npos);
    assert(kin::tween(world, card, {.property = "Transform2D.pos", .to = 3.0f}, &why) == 0 && why.find("vec2") != std::string::npos);
    assert(kin::tween(world, card, {.property = "SpriteRenderer.tint", .to = kin::Color{}}, &why) == 0 && why.find("no SpriteRenderer") != std::string::npos);
    assert(world.components().add(card, "ParticleSystemComponent"));
    assert(kin::tween(world, card, {.property = "ParticleSystemComponent.active", .to = 3.0f}, &why) == 0 && why.find("read only") != std::string::npos);

    // An instance of a prefab: its own copy changes, not the prefab's.
    flecs::entity prefab = world.raw().prefab("Base").set(kin::RectRenderer{.size = {4.0f, 4.0f}});
    flecs::entity made = world.raw().entity("Made").is_a(prefab);
    const kin::EcsEntity instance{made};
    assert(kin::tween(world, instance, {.property = "RectRenderer.size", .to = kin::Vec2f{8.0f, 8.0f}, .seconds = 1.0f}) != 0);
    kin::update_tweens(world, 1.0f);
    assert(made.get<kin::RectRenderer>()->size == (kin::Vec2f{8.0f, 8.0f}) && prefab.get<kin::RectRenderer>()->size == (kin::Vec2f{4.0f, 4.0f}));

    // Gone with its entity, or its component.
    kin::tween(world, card, {.property = "RectRenderer.size", .to = kin::Vec2f{1.0f, 1.0f}, .seconds = 1.0f});
    card.raw().remove<kin::RectRenderer>();
    kin::update_tweens(world, 0.1f);
    assert(!kin::tweening(card));
    kin::tween(world, card, {.property = "Transform2D.rotation", .to = 1.0f, .seconds = 1.0f});
    card.raw().destruct();
    kin::update_tweens(world, 0.1f);
}

void test_timers() {
    kin::Timers timers;
    std::vector<std::string> done;
    const kin::u32 once = timers.after(0.5f, [&] { done.push_back("once"); });
    const kin::u32 tick = timers.every(0.2f, [&] { done.push_back("tick"); });
    assert(timers.pending(once) && timers.pending(tick) && timers.size() == 2);
    timers.update(0.1f);
    assert(done.empty());
    timers.update(0.45f); // 0.55: ticks at 0.2 and 0.4, once at 0.5, in order
    assert((done == std::vector<std::string>{"tick", "tick", "once"}) && !timers.pending(once));
    done.clear();
    assert(timers.cancel(tick) && !timers.cancel(tick) && timers.size() == 0);
    timers.update(1.0f);
    assert(done.empty());
    // An action starts another and cancels itself.
    kin::u32 self = 0;
    self = timers.every(0.1f, [&] {
        done.push_back("self");
        timers.cancel(self);
        timers.after(0.0f, [&] { done.push_back("then"); });
    });
    timers.update(0.35f);
    assert((done == std::vector<std::string>{"self", "then"}) && timers.size() == 0);
    // Not moved by no time, or back in time.
    timers.after(0.1f, [&] { done.push_back("late"); });
    timers.update(0.0f);
    timers.update(-1.0f);
    assert(timers.size() == 1);
}

} // namespace

int main() {
    test_easings();
    test_tweens();
    test_timers();
    std::puts("tween tests passed");
    return 0;
}
