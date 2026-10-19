#pragma once

#include <functional>
#include <kin/scene/scene_session3d.hpp>

namespace kin {
// Native game-defined IDs. Map these to prompts/input/actions in game code.
// Zero is reserved for an invalid offer; no strings or callbacks are retained.
using InteractionVerb3D = u32;
struct InteractionOffer3D {
    Vec3f point; // current world-space contact point, independent of model scale
    InteractionVerb3D verb = 0;
};
struct InteractionFocus3D {
    SceneEntity3D target;
    InteractionVerb3D verb = 0;
    Vec3f point;
    f32 distance_squared = 0;
    explicit operator bool() const { return bool(target) && verb != 0; }
};
struct InteractionObserver3D {
    SceneEntity3D actor; // required, checked; all its native bodies are ignored
    Vec3f origin;
    std::optional<Vec3f> view; // optional second line of sight, e.g. fixed-step camera
};
struct InteractionSettings3D {
    f32 reach = 2;
    Layer3DMask candidates = all_layers3d, sight = all_layers3d;
    bool candidate_sensors = false, sight_sensors = false;
    bool require_sight = true;
};
enum class InteractionStatus3D : u8 { Ready, NoTarget, Overflow, Unavailable, InvalidObserver };
struct InteractionResult3D {
    InteractionStatus3D status = InteractionStatus3D::NoTarget;
    InteractionFocus3D focus;
    explicit operator bool() const { return status == InteractionStatus3D::Ready; }
};
// Owner-thread utility over existing scene queries, with one retained sphere.
// No ECS components, world, scheduler, strings or effect queues are owned here.
class InteractionQuery3D {
public:
    explicit InteractionQuery3D(InteractionSettings3D settings = {});
    const InteractionSettings3D& settings() const { return _settings; }
    // describe(EcsEntity) -> optional<InteractionOffer3D>: read-only game policy
    // (ownership, enabled state, components, verb and current contact point).
    // Only bodies overlapping the reach sphere are candidates. The point must
    // also be in range. Scratch is caller-owned; saturation always fails closed.
    // Exact distance ties use the lower entity ID, independent of body order.
    template <class Describe>
    InteractionResult3D select(const SceneQueries3D& queries, const InteractionObserver3D& observer,
                               std::span<SceneOverlapHit3D> scratch, Describe&& describe) const {
        const auto status = observer_status(queries, observer);
        if (status != InteractionStatus3D::Ready)
            return {status};
        const auto found = queries.overlap(
            _reach, observer.origin, {}, scratch,
            {.mask = _settings.candidates, .ignore = observer.actor, .sensors = _settings.candidate_sensors});
        if (!found.valid)
            return {InteractionStatus3D::Unavailable};
        if (found.overflow)
            return {InteractionStatus3D::Overflow};
        InteractionResult3D result;
        for (size_t i = 0; i < found.count; ++i) {
            auto entity = queries.resolve(scratch[i]); // includes native-body identity
            if (!entity || scratch[i].entity == observer.actor)
                continue;
            const auto offer = std::invoke(describe, entity);
            if (!offer)
                continue;
            const auto focus = check(queries, observer, scratch[i].entity, *offer);
            if (focus && (!result || focus->distance_squared < result.focus.distance_squared ||
                          (focus->distance_squared == result.focus.distance_squared &&
                           focus->target.entity.id < result.focus.target.entity.id)))
                result = {InteractionStatus3D::Ready, *focus};
        }
        return result;
    }
    // Recompute policy, range and sight for the original checked target/verb.
    // Never switches to a new nearest target. Broadphase candidate masks are
    // selection-only; describe() must enforce any game eligibility at contact.
    // Keep an existing action token alongside this focus and check it before
    // applying effects; this utility does not own animation or commit effects.
    template <class Describe>
    InteractionResult3D revalidate(const SceneQueries3D& queries, const InteractionObserver3D& observer,
                                   const InteractionFocus3D& locked, Describe&& describe) const {
        const auto status = observer_status(queries, observer);
        if (status != InteractionStatus3D::Ready)
            return {status};
        auto entity = queries.resolve(locked.target);
        if (!entity || !locked || locked.target == observer.actor)
            return {};
        const auto offer = std::invoke(describe, entity);
        if (!offer || offer->verb != locked.verb)
            return {};
        const auto focus = check(queries, observer, locked.target, *offer);
        return focus ? InteractionResult3D{InteractionStatus3D::Ready, *focus} : InteractionResult3D{};
    }

private:
    InteractionStatus3D observer_status(const SceneQueries3D&, const InteractionObserver3D&) const;
    std::optional<InteractionFocus3D> check(const SceneQueries3D&, const InteractionObserver3D&, SceneEntity3D,
                                            const InteractionOffer3D&) const;
    InteractionSettings3D _settings;
    Shape3D _reach;
};
} // namespace kin
