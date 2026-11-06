#include <kin/ecs/tween.hpp>

#include <kin/ecs/component.hpp>

#include <algorithm>
#include <cmath>
#include <utility>

namespace kin {
namespace {

std::optional<AnimValue> anim_value_of(const ComponentFieldValue& value) {
    return std::visit(
        [](const auto& typed) -> std::optional<AnimValue> {
            using V = std::decay_t<decltype(typed)>;
            if constexpr (std::is_same_v<V, f32> || std::is_same_v<V, Vec2f> || std::is_same_v<V, Color> || std::is_same_v<V, i32> ||
                          std::is_same_v<V, bool> || std::is_same_v<V, std::string>) {
                return AnimValue{typed};
            } else {
                return std::nullopt;
            }
        },
        value);
}

ComponentFieldValue field_value_of(const AnimValue& value) {
    return std::visit([](const auto& typed) { return ComponentFieldValue{typed}; }, value);
}

std::string_view kind_name(const AnimValue& value) {
    return anim_value_kind_name(value_kind(value));
}

// `to` as the field's kind (a whole number for a whole-number field, and back); none when it can't be.
std::optional<AnimValue> as_kind_of(const AnimValue& field, const AnimValue& to) {
    if (field.index() == to.index()) {
        return to;
    }
    if (std::holds_alternative<i32>(field) && std::holds_alternative<f32>(to)) {
        return AnimValue{static_cast<i32>(std::lround(std::get<f32>(to)))};
    }
    if (std::holds_alternative<f32>(field) && std::holds_alternative<i32>(to)) {
        return AnimValue{static_cast<f32>(std::get<i32>(to))};
    }
    return std::nullopt;
}

AnimValue plus(const AnimValue& a, const AnimValue& b) {
    if (const auto* x = std::get_if<f32>(&a); x && std::holds_alternative<f32>(b)) {
        return *x + std::get<f32>(b);
    }
    if (const auto* x = std::get_if<i32>(&a); x && std::holds_alternative<i32>(b)) {
        return *x + std::get<i32>(b);
    }
    if (const auto* x = std::get_if<Vec2f>(&a); x && std::holds_alternative<Vec2f>(b)) {
        const Vec2f y = std::get<Vec2f>(b);
        return Vec2f{x->x + y.x, x->y + y.y};
    }
    return b;
}

// A component inherited from a prefab, made the entity's own before it is
// written (else the prefab's, and every instance's, would change).
void own(flecs::entity entity, flecs::id_t component) {
    if (!entity.has(component) || entity.owns(component)) {
        return;
    }
    const ecs_type_info_t* info = ecs_get_type_info(entity.world(), component);
    const void* inherited = entity.get(component);
    if (info && inherited) {
        entity.set_ptr(component, static_cast<std::size_t>(info->size), inherited);
    }
}

// The reader and writer of `property` ("Component.field"); none, with why.
std::optional<ComponentFieldAccess> reach(const EcsComponentRegistry& registry, std::string_view property, std::string& why) {
    const std::size_t dot = property.find('.');
    if (dot == std::string_view::npos || dot == 0 || dot + 1 == property.size()) {
        why = "'" + std::string(property) + "' is not Component.field";
        return std::nullopt;
    }
    const std::string_view component = property.substr(0, dot);
    const std::string_view field = property.substr(dot + 1);
    const ComponentDescriptor* descriptor = registry.find(component);
    if (!descriptor) {
        why = "no component '" + std::string(component) + "' is described";
        return std::nullopt;
    }
    const auto described = std::ranges::find(descriptor->fields, field, &ComponentFieldDescriptor::name);
    if (described == descriptor->fields.end()) {
        why = std::string(component) + " has no field '" + std::string(field) + "'";
        return std::nullopt;
    }
    if (described->readonly) {
        why = std::string(property) + " is read only";
        return std::nullopt;
    }
    std::optional<ComponentFieldAccess> access = registry.field_access(component, field);
    if (!access) {
        why = std::string(property) + " can't be written from code";
    }
    return access;
}

} // namespace

u32 tween(EcsWorld& world, EcsEntity entity, TweenDef def, std::string* error) {
    std::string why;
    const auto fail = [&](std::string message) {
        if (error) {
            *error = std::move(message);
        }
        return 0u;
    };
    if (!entity.valid() || !entity.raw().is_alive()) {
        return fail("the entity is gone");
    }
    const std::optional<ComponentFieldAccess> access = reach(world.components(), def.property, why);
    if (!access) {
        return fail(std::move(why));
    }
    flecs::entity raw = entity.raw();
    const void* data = raw.get(access->component);
    if (!data) {
        return fail("the entity has no " + def.property.substr(0, def.property.find('.')));
    }
    const std::optional<AnimValue> now = anim_value_of(access->read(data));
    if (!now) {
        return fail(def.property + " is not a number, a pair, a colour, true/false or text");
    }
    const std::optional<AnimValue> to = as_kind_of(*now, def.to);
    if (!to) {
        return fail(def.property + " takes a " + std::string(kind_name(*now)) + ", not a " + std::string(kind_name(def.to)));
    }
    def.to = *to;

    world.raw().component<Tweens>("Tweens");
    Tweens& tweens = raw.ensure<Tweens>();
    std::erase_if(tweens.running, [&](const Tweens::Running& running) { return running.def.property == def.property; });
    const u32 id = tweens.next_id++;
    const f32 delay = std::max(def.delay, 0.0f);
    tweens.running.push_back({.id = id, .def = std::move(def), .time = -delay});
    return id;
}

bool stop_tween(EcsEntity entity, u32 id) {
    Tweens* tweens = entity.valid() ? entity.raw().get_mut<Tweens>() : nullptr;
    return tweens && std::erase_if(tweens->running, [&](const Tweens::Running& running) { return running.id == id; }) > 0;
}

u32 stop_tweens(EcsEntity entity, std::string_view property) {
    Tweens* tweens = entity.valid() ? entity.raw().get_mut<Tweens>() : nullptr;
    if (!tweens) {
        return 0;
    }
    return static_cast<u32>(std::erase_if(tweens->running, [&](const Tweens::Running& running) {
        return property.empty() || running.def.property == property;
    }));
}

bool tweening(EcsEntity entity, std::string_view property) {
    const Tweens* tweens = entity.valid() ? entity.raw().get<Tweens>() : nullptr;
    return tweens && std::ranges::any_of(tweens->running, [&](const Tweens::Running& running) {
        return property.empty() || running.def.property == property;
    });
}

void update_tweens(EcsWorld& world, f32 dt) {
    flecs::world& raw = world.raw();
    raw.component<Tweens>("Tweens");
    std::vector<flecs::entity> moving;
    world.each([&](flecs::entity entity, Tweens& tweens) {
        tweens.ended.clear();
        if (!tweens.running.empty()) {
            moving.push_back(entity);
        }
    });
    const EcsComponentRegistry& registry = world.components();
    std::string why;
    for (const flecs::entity entity : moving) {
        if (!entity.is_alive()) {
            continue;
        }
        // What its tweens write, made its own first: that may move it, and its Tweens with it.
        std::vector<std::pair<std::string, ComponentFieldAccess>> reached;
        for (const Tweens::Running& running : entity.get<Tweens>()->running) {
            if (std::ranges::find(reached, running.def.property, &std::pair<std::string, ComponentFieldAccess>::first) == reached.end()) {
                if (std::optional<ComponentFieldAccess> access = reach(registry, running.def.property, why)) {
                    own(entity, access->component);
                    reached.emplace_back(running.def.property, std::move(*access));
                }
            }
        }
        Tweens* tweens = entity.get_mut<Tweens>();
        std::vector<flecs::id_t> written;
        for (std::size_t i = 0; i < tweens->running.size();) {
            Tweens::Running& run = tweens->running[i];
            const auto found = std::ranges::find(reached, run.def.property, &std::pair<std::string, ComponentFieldAccess>::first);
            void* data = found != reached.end() ? entity.get_mut(found->second.component) : nullptr;
            if (!data) {
                tweens->running.erase(tweens->running.begin() + static_cast<std::ptrdiff_t>(i)); // its component is gone
                continue;
            }
            const ComponentFieldAccess& access = found->second;
            run.time += dt;
            if (run.time < 0.0f) {
                ++i; // waiting
                continue;
            }
            if (!run.started) {
                run.started = true;
                run.from = anim_value_of(access.read(data)).value_or(run.def.to);
                run.goal = run.def.relative ? plus(run.from, run.def.to) : run.def.to;
            }
            const f32 seconds = std::max(run.def.seconds, 0.0f);
            bool ended = false;
            for (i32 laps = 0; laps < 1000 && (seconds <= 0.0f || run.time >= seconds); ++laps) {
                access.write(data, field_value_of(run.goal));
                if (run.def.repeats >= 0 && run.rounds >= run.def.repeats) {
                    ended = true;
                    break;
                }
                ++run.rounds;
                run.time = seconds > 0.0f ? run.time - seconds : 0.0f;
                if (run.def.yoyo) {
                    std::swap(run.from, run.goal);
                }
                if (seconds <= 0.0f) {
                    break; // a tween of no time repeats once a step
                }
            }
            if (!ended && seconds > 0.0f) {
                access.write(data, field_value_of(interpolate(run.from, run.goal, run.time / seconds, run.def.easing)));
            }
            if (std::ranges::find(written, access.component) == written.end()) {
                written.push_back(access.component);
            }
            if (ended) {
                tweens->ended.push_back(run.id);
                tweens->running.erase(tweens->running.begin() + static_cast<std::ptrdiff_t>(i));
            } else {
                ++i;
            }
        }
        for (const flecs::id_t component : written) {
            entity.modified(component);
        }
    }
}

} // namespace kin
