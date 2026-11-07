#include "entity_scripts_internal.hpp"


#include <kin/ecs/camera.hpp>
#include <kin/ecs/dialogue.hpp>
#include <kin/ecs/render.hpp>
#include <kin/ecs/saving.hpp>
#include <kin/ecs/scene_flow.hpp>
#include <kin/ecs/ui_screens.hpp>
#include <kin/l10n/localization.hpp>
#include <kin/platform/input.hpp>
#include <kin/prefab/prefab_asset.hpp>
#include <kin/scripting/lua_values.hpp>
#if KIN_ENABLE_PHYSICS
#include <kin/ecs/physics.hpp>
#endif

#include <algorithm>
#include <cstdio>
#include <functional>
#include <tuple>
#include <utility>

namespace kin {
namespace {

// Makes a script's `self`: a table of its own, with the entity's methods and
// the script's own: timers, and tweens with a done callback, run as the
// script's tasks (cancelled when it stops, dropped by a reload).
constexpr const char* self_prelude = R"lua(
__kin_self_methods = {}
local own = __kin_self_methods
function own.after(self, seconds, fn)
    assert(type(seconds) == "number" and seconds >= 0, "after takes seconds")
    assert(type(fn) == "function", "after takes a function")
    return task.run(function()
        task.wait(seconds)
        fn(self)
    end)
end
function own.every(self, seconds, fn)
    assert(type(seconds) == "number" and seconds > 0, "every takes a positive interval")
    assert(type(fn) == "function", "every takes a function")
    return task.run(function()
        while true do
            task.wait(seconds)
            fn(self)
        end
    end)
end
function own.cancel(self, timer)
    local state = timer:state()
    timer:cancel()
    return state == "pending" or state == "running" or state == "waiting"
end
function own.tween(self, property, to, seconds, easing, options)
    local id = self.entity:tween(property, to, seconds, easing, options)
    local given = type(easing) == "table" and easing or options
    local done = type(given) == "table" and given.done or nil
    if done ~= nil then
        assert(type(done) == "function", "tween: done takes a function")
        local tween = tweens.of(self.entity, id)
        task.run(function()
            task.until_(function() return tween:state() ~= "running" end)
            if tween:state() == "completed" then
                done(self)
            end
        end)
    end
    return id
end
function __kin_self(entity)
    return setmetatable({entity = entity}, {__index = function(_, key)
        local mine = own[key]
        if mine then
            return mine
        end
        local method = entity[key]
        if type(method) == "function" then
            return function(_, ...) return method(entity, ...) end
        end
        return nil
    end})
end
)lua";

// A number as text: 12, not 12.000000.
std::string format_number(double value) {
    char text[32];
    std::snprintf(text, sizeof(text), "%g", value);
    return text;
}

} // namespace

void EntityScripts::Impl::bind_services(sol::state& lua) {
    sol::table input_table = lua.create_named_table("input");
    // Each takes a player last (1 the first, 0 anyone), or reads the
    // player its entity's ScriptComponent names.
    input_table["held"] = std::function{[this](const std::string& action, sol::optional<i32> who) {
        return input && input->held(player_of(who), action);
    }};
    input_table["pressed"] = std::function{[this](const std::string& action, sol::optional<i32> who) {
        return input && input->pressed(player_of(who), action);
    }};
    input_table["released"] = std::function{[this](const std::string& action, sol::optional<i32> who) {
        return input && input->released(player_of(who), action);
    }};
    input_table["axis"] = std::function{[this](const std::string& name, sol::optional<i32> who) {
        return input ? input->axis(player_of(who), name) : 0.0f;
    }};
    // Whose input this script's entity reads (0: anyone).
    input_table["player"] = std::function{[this]() { return player + 1; }};
    // The fingers down, {id, x, y, start_x, start_y} each; two fingers'
    // pinch (1: none) and pan since the last step. Gestures are actions
    // an input file binds ("touch jump tap").
    input_table["touches"] = std::function{[this](sol::this_state state) {
        sol::state_view lua{state};
        sol::table list = lua.create_table();
        if (input) {
            i32 at = 1;
            for (const TouchPoint& t : input->touches()) {
                list[at++] = lua.create_table_with("id", static_cast<f64>(t.id), "x", t.pos.x, "y", t.pos.y, "start_x", t.start.x,
                                                   "start_y", t.start.y);
            }
        }
        return list;
    }};
    input_table["pinch"] = std::function{[this]() { return input ? input->touch_pinch() : 1.0f; }};
    input_table["pan"] = std::function{[this]() {
        const Vec2f moved = input ? input->touch_pan() : Vec2f{};
        return std::make_tuple(moved.x, moved.y);
    }};
    input_table["mouse"] = std::function{[this]() {
        const Vec2f at = input ? input->mouse_pos() : Vec2f{};
        return std::make_tuple(at.x, at.y);
    }};

    // The language, which game scripts may change (a language menu's).
    if (sol::optional<sol::table> l10n = lua["l10n"]) {
        (*l10n)["set_locale"] = +[](const std::string& tag) -> std::string {
            if (scripting_detail::reload_staging) {
                throw sol::error("live services are unavailable while preparing reload");
            }
            Localization* active = active_localization();
            return active ? active->set_locale(tag) : std::string{};
        };
    }

    sol::table ui_table = lua.create_named_table("ui");
    ui_table["clicked"] = std::function{[this](const std::string& id) { return screens && screens->clicked(id); }};
    ui_table["changed"] = std::function{[this](const std::string& id) { return screens && screens->changed(id); }};
    ui_table["value"] = std::function{[this](const std::string& id) { return screens ? screens->value(id) : 0.0f; }};
    ui_table["checked"] = std::function{[this](const std::string& id) { return screens && screens->checked(id); }};
    ui_table["text"] = std::function{[this](const std::string& id) { return screens ? screens->text(id) : std::string(); }};
    ui_table["set_text"] = std::function{[this](const std::string& id, const sol::object& text) {
        if (screens) {
            screens->set_text(id, text.is<std::string>() ? text.as<std::string>()
                                                        : text.is<double>() ? format_number(text.as<double>()) : std::string());
        }
    }};
    ui_table["set_value"] = std::function{[this](const std::string& id, f32 value) {
        if (screens) screens->set_value(id, value);
    }};
    ui_table["set_checked"] = std::function{[this](const std::string& id, bool checked) {
        if (screens) screens->set_checked(id, checked);
    }};
    ui_table["show"] = std::function{[this](const std::string& id, sol::optional<bool> shown) {
        if (screens) screens->set_visible(id, shown.value_or(true));
    }};
    ui_table["hide"] = std::function{[this](const std::string& id) {
        if (screens) screens->set_visible(id, false);
    }};
    ui_table["set_image"] = std::function{[this](const std::string& id, const std::string& path) {
        if (screens) screens->set_image(id, path);
    }};
    ui_table["set_enabled"] = std::function{[this](const std::string& id, bool enabled) {
        if (screens) screens->set_enabled(id, enabled);
    }};

    sol::table dialogue_table = lua.create_named_table("dialogue");
    dialogue_table["start"] = std::function{[this](const std::string& path, sol::optional<std::string> node) {
        return dialogue && dialogue->start(path, node.value_or(""));
    }};
    dialogue_table["stop"] = std::function{[this]() {
        if (dialogue) dialogue->stop();
    }};
    dialogue_table["active"] = std::function{[this]() { return dialogue && dialogue->active(); }};
    dialogue_table["advance"] = std::function{[this]() {
        if (dialogue) dialogue->advance();
    }};
    dialogue_table["choose"] = std::function{[this](const sol::object& which) {
        if (!dialogue) return false;
        if (which.is<std::string>()) return dialogue->choose(std::string_view(which.as<std::string>()));
        return which.is<double>() && dialogue->choose(static_cast<i32>(which.as<double>()) - 1); // Lua counts from 1
    }};
    dialogue_table["node"] = std::function{[this]() { return dialogue && dialogue->active() ? dialogue->view().node_id : std::string(); }};
    dialogue_table["get"] = std::function{[this](sol::this_state state, const std::string& name) -> sol::object {
        if (!dialogue) return sol::make_object(state, sol::lua_nil);
        const DialogueVariable value = dialogue->variable(name);
        if (const auto* b = std::get_if<bool>(&value.value)) return sol::make_object(state, *b);
        if (const auto* n = std::get_if<f64>(&value.value)) return sol::make_object(state, *n);
        return sol::make_object(state, std::get<std::string>(value.value));
    }};
    dialogue_table["set"] = std::function{[this](const std::string& name, const sol::object& value) {
        if (!dialogue) return;
        if (value.is<bool>()) dialogue->set_variable(name, {.value = value.as<bool>()});
        else if (value.is<double>()) dialogue->set_variable(name, {.value = value.as<double>()});
        else if (value.is<std::string>()) dialogue->set_variable(name, {.value = value.as<std::string>()});
    }};

    sol::table scene_table = lua.create_named_table("scene");
    scene_table["go"] = std::function{[this](const std::string& path, sol::optional<std::string> arrive, sol::optional<std::string> transition,
                               sol::optional<f32> duration) {
        if (!flow) {
            return false;
        }
        SceneTravel travel{.scene = path, .arrive = arrive.value_or("")};
        if (transition) {
            if (*transition == "none") {
                travel.transition.reset();
            } else if (const std::optional<TransitionKind> kind = transition_from_name(*transition)) {
                travel.transition = kind;
            } else {
                throw sol::error("no transition is called '" + *transition + "'");
            }
        }
        travel.duration = std::max(duration.value_or(travel.duration), 0.0f);
        flow->go(std::move(travel));
        return true;
    }};
    scene_table["going"] = std::function{[this]() { return flow && flow->pending(); }};

    sol::table world_table = lua.create_named_table("world");
    world_table["find"] = std::function{[this](const std::string& name) -> std::optional<ScriptEntity> {
        const EcsEntity found = world ? world->entities().find_by_name(name) : EcsEntity{};
        return found.valid() ? std::optional(ScriptEntity{world, found.raw()}) : std::nullopt;
    }};
    world_table["spawn"] = std::function{[this](const std::string& prefab, sol::optional<f32> x, sol::optional<f32> y) -> std::optional<ScriptEntity> {
        const std::shared_ptr<const PrefabAsset> asset = prefab_of(prefab);
        if (!asset || !world) {
            throw sol::error("the prefab '" + prefab + "' could not be read");
        }
        const PrefabInstance made = instantiate_prefab(*world, *asset, PrefabSpawn{.pos = {x.value_or(0.0f), y.value_or(0.0f)}},
                                                       world->components());
        if (!made.root.valid()) {
            return std::nullopt;
        }
        made.root.raw().set<SpawnedPrefab>({.prefab = prefab}); // a save makes it again from it
        return ScriptEntity{world, made.root.raw()};
    }};

    sol::table camera_table = lua.create_named_table("camera");
    camera_table["shake"] = std::function{[this](f32 trauma) {
        if (world) {
            shake_camera(*world, trauma);
        }
    }};
    camera_table["trauma"] = std::function{[this]() {
        f32 most = 0.0f;
        if (world) {
            world->each([&](const CameraComponent& camera) {
                if (camera.current) {
                    most = std::max(most, camera.trauma);
                }
            });
        }
        return most;
    }};

#if KIN_ENABLE_PHYSICS
    bind_physics(lua);
#endif

    sol::table save_table = lua.create_named_table("save");
    const auto saves_or_throw = [this]() -> SceneSaves& {
        if (!saves) {
            throw sol::error("the game keeps no saves");
        }
        return *saves;
    };
    save_table["write"] = std::function{[this, saves_or_throw](const std::string& slot, sol::optional<std::string> title,
                                                 sol::optional<std::string> summary, sol::this_state state) {
        SceneSaves& kept = saves_or_throw();
        if (!world) {
            throw sol::error("save.write: no world");
        }
        const SaveResult written = kept.write(*world, slot, title.value_or(slot), summary.value_or(""));
        sol::state_view lua(state);
        if (written.ok) {
            return std::make_tuple(sol::make_object(lua, true), sol::make_object(lua, sol::lua_nil));
        }
        return std::make_tuple(sol::make_object(lua, sol::lua_nil), sol::make_object(lua, written.message));
    }};
    save_table["read"] = std::function{[saves_or_throw](const std::string& slot) {
        SceneSaves& kept = saves_or_throw();
        if (!valid_save_id(slot)) {
            return std::make_tuple(false, std::string("'" + slot + "' is not a slot's name"));
        }
        if (!kept.store().slot_exists(slot)) {
            return std::make_tuple(false, std::string("no save is called '" + slot + "'"));
        }
        kept.ask_read(slot);
        return std::make_tuple(true, std::string());
    }};
    save_table["exists"] = std::function{[saves_or_throw](const std::string& slot) {
        return valid_save_id(slot) && saves_or_throw().store().slot_exists(slot);
    }};
    save_table["delete"] = std::function{[saves_or_throw](const std::string& slot) {
        return valid_save_id(slot) && saves_or_throw().store().delete_slot(slot).ok;
    }};
    save_table["slots"] = std::function{[saves_or_throw](sol::this_state state) {
        sol::state_view lua(state);
        std::vector<SaveSlotInfo> slots = saves_or_throw().store().list_slots();
        std::ranges::stable_sort(slots, [](const SaveSlotInfo& a, const SaveSlotInfo& b) { return a.updated_at_unix > b.updated_at_unix; });
        sol::table listed = lua.create_table(static_cast<int>(slots.size()), 0);
        for (std::size_t i = 0; i < slots.size(); ++i) {
            listed[i + 1] = lua.create_table_with("id", slots[i].slot_id, "title", slots[i].title, "summary", slots[i].summary,
                                                  "updated", static_cast<f64>(slots[i].updated_at_unix));
        }
        return listed;
    }};
    save_table["get"] = std::function{[saves_or_throw](const std::string& name, sol::object fallback, sol::this_state state) -> sol::object {
        const JsonValue* value = saves_or_throw().value(name);
        return value ? json_to_lua(state, *value) : fallback;
    }};
    save_table["set"] = std::function{[saves_or_throw](const std::string& name, sol::object value) {
        SceneSaves& kept = saves_or_throw();
        if (value.get_type() == sol::type::lua_nil || value.get_type() == sol::type::none) {
            kept.set_value(name, JsonValue{});
            return;
        }
        std::optional<JsonValue> json = lua_to_json(value);
        if (!json) {
            throw sol::error("save.set: '" + name + "' is not kept (true/false, numbers, text and tables of them are)");
        }
        kept.set_value(name, std::move(*json));
    }};
    lua.safe_script(self_prelude);
}

#if KIN_ENABLE_PHYSICS
flecs::entity EntityScripts::Impl::entity_of(PhysicsBody body) const {
    if (!physics || !world || !body) {
        return {};
    }
    const u64 id = physics->user_id(body);
    if (id == 0 || !world->raw().is_alive(id)) {
        return {};
    }
    const flecs::entity found = world->raw().entity(id);
    if (const PhysicsBodyComponent* owned = found.get<PhysicsBodyComponent>(); owned && owned->body == body) {
        return found;
    }
    if (const TilemapCollider* cells = found.get<TilemapCollider>(); cells && cells->body == body) {
        return found;
    }
    return {};
}

PhysicsFilter EntityScripts::Impl::filter_of(const sol::optional<sol::table>& options) {
    PhysicsFilter filter{.include_sensors = false};
    if (!options) {
        return filter;
    }
    if (const sol::optional<double> mask = (*options)["mask"]) {
        if (*mask < 0.0 || *mask > 65535.0) {
            throw sol::error("physics: mask takes bits, 0 to 65535");
        }
        filter.mask_bits = static_cast<u16>(*mask);
    }
    if (const sol::optional<bool> sensors = (*options)["sensors"]) {
        filter.include_sensors = *sensors;
    }
    sol::object ignore = (*options)["ignore"];
    if (ignore.is<sol::table>()) {
        ignore = ignore.as<sol::table>()["entity"]; // a script's self
    }
    if (ignore.is<ScriptEntity>()) {
        const ScriptEntity& other = ignore.as<const ScriptEntity&>();
        if (other.alive()) {
            if (const PhysicsBodyComponent* body = other.entity.get<PhysicsBodyComponent>()) {
                filter.ignore = body->body;
            }
        }
    }
    return filter;
}

PhysicsWorld& EntityScripts::Impl::physics_or_throw() const {
    if (!physics) {
        throw sol::error("the game gives its scripts no physics");
    }
    return *physics;
}

sol::table EntityScripts::Impl::hit_table(sol::state_view lua, PhysicsBody body, Vec2f point, Vec2f normal, f32 fraction) const {
    sol::table hit = lua.create_table();
    if (const flecs::entity entity = entity_of(body)) {
        hit["entity"] = ScriptEntity{world, entity};
    }
    hit["x"] = point.x;
    hit["y"] = point.y;
    hit["normal"] = lua.create_table_with("x", normal.x, "y", normal.y);
    hit["fraction"] = fraction;
    return hit;
}

sol::table EntityScripts::Impl::entities_table(sol::state_view lua, const std::vector<PhysicsFixture>& fixtures) const {
    sol::table list = lua.create_table();
    std::vector<u64> seen;
    int n = 0;
    for (const PhysicsFixture fixture : fixtures) {
        const flecs::entity entity = entity_of(physics->body_of(fixture));
        if (entity && std::ranges::find(seen, entity.id()) == seen.end()) {
            seen.push_back(entity.id());
            list[++n] = ScriptEntity{world, entity};
        }
    }
    return list;
}

void EntityScripts::Impl::bind_physics(sol::state& lua) {
    sol::table table = lua.create_named_table("physics");
    table["raycast"] = std::function{[this](f32 x1, f32 y1, f32 x2, f32 y2, sol::optional<sol::table> options, sol::this_state state) -> sol::object {
        const std::vector<PhysicsRaycastHit> hits = physics_or_throw().raycast({x1, y1}, {x2, y2}, filter_of(options));
        if (hits.empty()) {
            return sol::lua_nil;
        }
        const PhysicsRaycastHit& hit = hits.front();
        return hit_table(state, hit.body, hit.point, hit.normal, hit.fraction);
    }};
    const auto cast = [this](const PhysicsShape& shape, Vec2f from, Vec2f to, const sol::optional<sol::table>& options,
                             sol::this_state state) -> sol::object {
        const std::optional<PhysicsShapeHit> hit = physics_or_throw().cast_shape(shape, from, to, filter_of(options));
        if (!hit) {
            return sol::lua_nil;
        }
        return hit_table(state, hit->body, hit->point, hit->normal, hit->fraction);
    };
    table["cast_circle"] = std::function{[cast](f32 x1, f32 y1, f32 x2, f32 y2, f32 radius, sol::optional<sol::table> options, sol::this_state state) {
        return cast(PhysicsShape::circle(radius), {x1, y1}, {x2, y2}, options, state);
    }};
    table["cast_box"] = std::function{[cast](f32 x1, f32 y1, f32 x2, f32 y2, f32 width, f32 height, sol::optional<sol::table> options,
                               sol::this_state state) {
        return cast(PhysicsShape::box({0.5f * width, 0.5f * height}), {x1, y1}, {x2, y2}, options, state);
    }};
    table["overlap_point"] = std::function{[this](f32 x, f32 y, sol::optional<sol::table> options, sol::this_state state) {
        return entities_table(state, physics_or_throw().query_point({x, y}, filter_of(options)));
    }};
    table["overlap_circle"] = std::function{[this](f32 x, f32 y, f32 radius, sol::optional<sol::table> options, sol::this_state state) {
        return entities_table(state, physics_or_throw().query_shape(PhysicsShape::circle(radius), {x, y}, filter_of(options)));
    }};
    table["overlap_box"] = std::function{[this](f32 x, f32 y, f32 width, f32 height, sol::optional<sol::table> options, sol::this_state state) {
        return entities_table(state,
                              physics_or_throw().query_shape(PhysicsShape::box({0.5f * width, 0.5f * height}), {x, y}, filter_of(options)));
    }};
}

#endif

} // namespace kin
