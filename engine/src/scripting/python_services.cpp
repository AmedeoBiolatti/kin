// What the game gives its scripts (PythonScripts::set_physics, set_flow,
// set_saves), as the modules kin.physics, kin.scene and kin.save. Each raises
// when the game gave none. The classes they give (Hit, Slot, SaveError) are
// Python's (python_kin.py).

#include "python_internal.hpp"

#include <kin/core/json_value.hpp>
#include <kin/ecs/saving.hpp>
#include <kin/ecs/scene_flow.hpp>
#include <kin/save/save_store.hpp>
#include <kin/scene/transitions.hpp>

#if KIN_ENABLE_PHYSICS
#include <kin/ecs/physics.hpp>
#include <kin/physics/physics.hpp>
#endif

#include <algorithm>
#include <cmath>

namespace kin::python_detail {
namespace {

// Calls kin's Python class `name` with the `count` values on top of the
// stack (popped), its object in py_retval().
bool make(Host& host, const char* name, int count) {
    py_Ref type = py_getdict(host.kin, py_name(name));
    if (!type) {
        py_shrink(count);
        return py_exception(tp_RuntimeError, "kin.%s is missing", name);
    }
    py_TValue callable = *type;
    const bool ok = py_call(&callable, count, py_peek(-count));
    py_shrink(count);
    return ok;
}

bool save_error(Host& host, const std::string& message) {
    py_Ref type = py_getdict(host.kin, py_name("SaveError"));
    return py_exception(type ? py_totype(type) : tp_RuntimeError, "%s", message.c_str());
}

// --- kin.physics -------------------------------------------------------------

#if KIN_ENABLE_PHYSICS

bool physics_of(Host*& host) {
    host = current();
    if (!host) {
        return no_host();
    }
    if (!host->physics) {
        return py_exception(tp_RuntimeError, "the game gives its scripts no physics (PythonScripts::set_physics)");
    }
    return true;
}

// The entity a body is: its body's, or its tilemap's solid cells'.
flecs::entity_t entity_of(Host& host, PhysicsBody body) {
    if (!body) {
        return 0;
    }
    const u64 id = host.physics->user_id(body);
    if (id == 0 || !host.world.raw().is_alive(id)) {
        return 0;
    }
    const flecs::entity found = host.world.raw().entity(id);
    if (const PhysicsBodyComponent* owned = found.get<PhysicsBodyComponent>(); owned && owned->body == body) {
        return id;
    }
    if (const TilemapCollider* cells = found.get<TilemapCollider>(); cells && cells->body == body) {
        return id;
    }
    return 0;
}

// The query's mask, sensors and ignore (arguments `first` on): what to find.
bool filter_of(Host& host, py_StackRef argv, int first, PhysicsFilter& filter) {
    filter = PhysicsFilter{.include_sensors = false};
    if (!py_checkint(py_arg(first))) {
        return false;
    }
    const py_i64 mask = py_toint(py_arg(first));
    if (mask < 0 || mask > 0xFFFF) {
        return py_exception(tp_ValueError, "mask takes bits, 0 to 65535");
    }
    filter.mask_bits = static_cast<u16>(mask);
    filter.include_sensors = py_tobool(py_arg(first + 1));
    py_Ref ignore = py_arg(first + 2);
    if (!py_isnone(ignore)) {
        const flecs::entity_t entity = entity_arg(host, ignore);
        if (entity == 0) {
            return false;
        }
        if (ecs_is_alive(host.world.raw().c_ptr(), entity)) {
            if (const PhysicsBodyComponent* body = host.world.raw().entity(entity).get<PhysicsBodyComponent>()) {
                filter.ignore = body->body;
            }
        }
    }
    return true;
}

bool point_arg(py_Ref value, const char* what, Vec2f& out) {
    const std::optional<Vec2f> pair = pair_of(value);
    if (!pair) {
        return TypeError("%s is a vec2 or (x, y)", what);
    }
    out = *pair;
    return true;
}

// A shape from radius and size: a circle, a box (size: its width and height),
// or none (a point).
bool shape_of(py_Ref radius, py_Ref size, std::optional<PhysicsShape>& shape) {
    shape.reset();
    if (!py_isnone(size)) {
        Vec2f box{};
        if (!point_arg(size, "size", box)) {
            return false;
        }
        if (box.x <= 0.0f || box.y <= 0.0f) {
            return py_exception(tp_ValueError, "a size is more than 0 across and down");
        }
        shape = PhysicsShape::box({0.5f * box.x, 0.5f * box.y});
        return true;
    }
    const std::optional<f64> r = number_of(radius);
    if (!r) {
        return TypeError("radius is a number");
    }
    if (*r < 0.0) {
        return py_exception(tp_ValueError, "a radius is 0 or more");
    }
    if (*r > 0.0) {
        shape = PhysicsShape::circle(static_cast<f32>(*r));
    }
    return true;
}

bool hit_object(Host& host, PhysicsBody body, Vec2f point, Vec2f normal, f32 fraction) {
    const flecs::entity_t entity = entity_of(host, body);
    if (entity) {
        host.entity_object(py_pushtmp(), entity);
    } else {
        py_pushnone();
    }
    new_vec2(py_pushtmp(), point);
    new_vec2(py_pushtmp(), normal);
    py_newfloat(py_pushtmp(), fraction);
    return make(host, "Hit", 4);
}

// raycast(start, end, mask, sensors, ignore): the nearest Hit, or None.
bool physics_raycast(int argc, py_StackRef argv) {
    PY_CHECK_ARGC(5);
    Host* host = nullptr;
    Vec2f from{}, to{};
    PhysicsFilter filter;
    if (!physics_of(host) || !point_arg(py_arg(0), "start", from) || !point_arg(py_arg(1), "end", to) ||
        !filter_of(*host, argv, 2, filter)) {
        return false;
    }
    const std::vector<PhysicsRaycastHit> hits = host->physics->raycast(from, to, filter);
    if (hits.empty()) {
        py_newnone(py_retval());
        return true;
    }
    const PhysicsRaycastHit& hit = hits.front();
    return hit_object(*host, hit.body, hit.point, hit.normal, hit.fraction);
}

// cast(start, end, radius, size, mask, sensors, ignore): the first Hit of a
// circle or a box moved from start to end, or None.
bool physics_cast(int argc, py_StackRef argv) {
    PY_CHECK_ARGC(7);
    Host* host = nullptr;
    Vec2f from{}, to{};
    std::optional<PhysicsShape> shape;
    PhysicsFilter filter;
    if (!physics_of(host) || !point_arg(py_arg(0), "start", from) || !point_arg(py_arg(1), "end", to) ||
        !shape_of(py_arg(2), py_arg(3), shape) || !filter_of(*host, argv, 4, filter)) {
        return false;
    }
    if (!shape) {
        return py_exception(tp_ValueError, "cast() moves a circle (radius=) or a box (size=); a point is raycast()");
    }
    const std::optional<PhysicsShapeHit> hit = host->physics->cast_shape(*shape, from, to, filter);
    if (!hit) {
        py_newnone(py_retval());
        return true;
    }
    return hit_object(*host, hit->body, hit->point, hit->normal, hit->fraction);
}

// overlap(at, radius, size, mask, sensors, ignore): the entities at a point,
// or in a circle or a box centred there, each once.
bool physics_overlap(int argc, py_StackRef argv) {
    PY_CHECK_ARGC(6);
    Host* host = nullptr;
    Vec2f at{};
    std::optional<PhysicsShape> shape;
    PhysicsFilter filter;
    if (!physics_of(host) || !point_arg(py_arg(0), "at", at) || !shape_of(py_arg(1), py_arg(2), shape) ||
        !filter_of(*host, argv, 3, filter)) {
        return false;
    }
    const std::vector<PhysicsFixture> fixtures =
        shape ? host->physics->query_shape(*shape, at, filter) : host->physics->query_point(at, filter);
    std::vector<flecs::entity_t> found;
    for (const PhysicsFixture fixture : fixtures) {
        const flecs::entity_t entity = entity_of(*host, host->physics->body_of(fixture));
        if (entity && std::ranges::find(found, entity) == found.end()) {
            found.push_back(entity);
        }
    }
    const py_StackRef list = py_pushtmp();
    py_newlistn(list, static_cast<int>(found.size()));
    for (std::size_t i = 0; i < found.size(); ++i) {
        host->entity_object(py_list_getitem(list, static_cast<int>(i)), found[i]);
    }
    py_assign(py_retval(), list);
    py_pop();
    return true;
}

#else

bool no_physics(int, py_StackRef) {
    return py_exception(tp_RuntimeError, "kin was built without physics (KIN_ENABLE_PHYSICS)");
}

#endif

// --- kin.scene ---------------------------------------------------------------

bool flow_of(Host*& host) {
    host = current();
    if (!host) {
        return no_host();
    }
    if (!host->flow) {
        return py_exception(tp_RuntimeError, "the game gives its scripts no scene flow (PythonScripts::set_flow)");
    }
    return true;
}

// go(path, arrive, transition, seconds): asks the game to go there.
bool scene_go(int argc, py_StackRef argv) {
    PY_CHECK_ARGC(4);
    Host* host = nullptr;
    if (!flow_of(host) || !py_checkstr(py_arg(0)) || !py_checkstr(py_arg(1))) {
        return false;
    }
    SceneTravel travel{.scene = std::string(view_of(py_arg(0))), .arrive = std::string(view_of(py_arg(1)))};
    py_Ref transition = py_arg(2);
    if (py_isnone(transition)) {
        travel.transition.reset();
    } else {
        if (!py_checkstr(transition)) {
            return false;
        }
        const std::string_view name = view_of(transition);
        if (name == "none") {
            travel.transition.reset();
        } else if (const std::optional<TransitionKind> kind = transition_from_name(name)) {
            travel.transition = kind;
        } else {
            return py_exception(tp_ValueError, "no transition is called '%s'", std::string(name).c_str());
        }
    }
    const std::optional<f64> seconds = number_of(py_arg(3));
    if (!seconds) {
        return TypeError("seconds is a number");
    }
    travel.duration = static_cast<f32>(std::max(*seconds, 0.0));
    host->flow->go(std::move(travel));
    py_newnone(py_retval());
    return true;
}

bool scene_going(int argc, py_StackRef argv) {
    PY_CHECK_ARGC(0);
    Host* host = nullptr;
    if (!flow_of(host)) {
        return false;
    }
    py_newbool(py_retval(), host->flow->pending());
    return true;
}

// --- kin.save ----------------------------------------------------------------

bool saves_of(Host*& host) {
    host = current();
    if (!host) {
        return no_host();
    }
    if (!host->saves) {
        return py_exception(tp_RuntimeError, "the game gives its scripts no saves (PythonScripts::set_saves)");
    }
    return true;
}

bool slot_arg(Host& host, py_Ref value, std::string& slot) {
    if (!py_checkstr(value)) {
        return false;
    }
    slot = std::string(view_of(value));
    if (!valid_save_id(slot)) {
        return save_error(host, "'" + slot + "' is not a slot's name (letters, digits, - and _)");
    }
    return true;
}

// A value of the game's as JSON: None, True/False, numbers, text, and lists
// and dicts (text keys) of them.
bool json_of(py_Ref value, JsonValue& out, int depth) {
    if (depth > 64) {
        return py_exception(tp_ValueError, "a saved value nests too deep");
    }
    if (py_isnone(value)) {
        out = JsonValue();
    } else if (py_isbool(value)) {
        out = JsonValue(py_tobool(value));
    } else if (py_isint(value)) {
        out = JsonValue(static_cast<f64>(py_toint(value)));
    } else if (py_isfloat(value)) {
        out = JsonValue(py_tofloat(value));
    } else if (py_isstr(value)) {
        out = JsonValue(std::string(view_of(value)));
    } else if (py_islist(value) || py_istuple(value)) {
        const bool list = py_islist(value);
        const int count = list ? py_list_len(value) : py_tuple_len(value);
        JsonValue::Array items(static_cast<std::size_t>(count));
        for (int i = 0; i < count; ++i) {
            if (!json_of(list ? py_list_getitem(value, i) : py_tuple_getitem(value, i),
                         items[static_cast<std::size_t>(i)], depth + 1)) {
                return false;
            }
        }
        out = JsonValue(std::move(items));
    } else if (py_isdict(value)) {
        struct Context {
            JsonValue::Object members;
            int depth = 0;
        } context{.depth = depth};
        const bool ok = py_dict_apply(
            value,
            [](py_Ref key, py_Ref item, void* data) {
                auto& context = *static_cast<Context*>(data);
                if (!py_isstr(key)) {
                    return TypeError("a saved dict's keys are text");
                }
                return json_of(item, context.members[std::string(view_of(key))], context.depth + 1);
            },
            &context);
        if (!ok) {
            return false;
        }
        out = JsonValue(std::move(context.members));
    } else {
        return TypeError("a saved value is None, True/False, a number, text, or a list or dict of them; not %t",
                         py_typeof(value));
    }
    return true;
}

// And back: a whole number comes back an int.
bool python_of(const JsonValue& value, py_OutRef out) {
    switch (value.type()) {
    case JsonValue::Type::Null:
        py_newnone(out);
        return true;
    case JsonValue::Type::Bool:
        py_newbool(out, value.as_bool());
        return true;
    case JsonValue::Type::Number: {
        const f64 number = value.as_number();
        if (std::trunc(number) == number && std::fabs(number) < 9.0e15) {
            py_newint(out, static_cast<py_i64>(number));
        } else {
            py_newfloat(out, number);
        }
        return true;
    }
    case JsonValue::Type::String:
        py_newstrv(out, view_of(value.as_string()));
        return true;
    case JsonValue::Type::Array: {
        py_newlist(out);
        for (const JsonValue& item : value.items()) {
            const py_StackRef made = py_pushtmp();
            if (!python_of(item, made)) {
                py_pop();
                return false;
            }
            py_list_append(out, made);
            py_pop();
        }
        return true;
    }
    case JsonValue::Type::Object: {
        py_newdict(out);
        for (const auto& [name, item] : value.members()) {
            const py_StackRef key = py_pushtmp();
            py_newstrv(key, view_of(name));
            const py_StackRef made = py_pushtmp();
            if (!python_of(item, made) || !py_dict_setitem(out, key, made)) {
                py_shrink(2);
                return false;
            }
            py_shrink(2);
        }
        return true;
    }
    }
    py_newnone(out);
    return true;
}

// write(slot, title, summary): the game written to the slot; SaveError when it can't be.
bool save_write(int argc, py_StackRef argv) {
    PY_CHECK_ARGC(3);
    Host* host = nullptr;
    std::string slot;
    if (!saves_of(host) || !slot_arg(*host, py_arg(0), slot) || !py_checkstr(py_arg(1)) || !py_checkstr(py_arg(2))) {
        return false;
    }
    std::string title(view_of(py_arg(1)));
    if (title.empty()) {
        title = slot;
    }
    const SaveResult written = host->saves->write(host->world, slot, std::move(title), std::string(view_of(py_arg(2))));
    if (!written.ok) {
        return save_error(*host, written.message);
    }
    py_newnone(py_retval());
    return true;
}

// read(slot): asks the game to read it, after this update; SaveError when there is none.
bool save_read(int argc, py_StackRef argv) {
    PY_CHECK_ARGC(1);
    Host* host = nullptr;
    std::string slot;
    if (!saves_of(host) || !slot_arg(*host, py_arg(0), slot)) {
        return false;
    }
    if (!host->saves->store().slot_exists(slot)) {
        return save_error(*host, "no save is called '" + slot + "'");
    }
    host->saves->ask_read(slot);
    py_newnone(py_retval());
    return true;
}

bool save_exists(int argc, py_StackRef argv) {
    PY_CHECK_ARGC(1);
    Host* host = nullptr;
    if (!saves_of(host) || !py_checkstr(py_arg(0))) {
        return false;
    }
    const std::string_view slot = view_of(py_arg(0));
    py_newbool(py_retval(), valid_save_id(slot) && host->saves->store().slot_exists(slot));
    return true;
}

bool save_delete(int argc, py_StackRef argv) {
    PY_CHECK_ARGC(1);
    Host* host = nullptr;
    std::string slot;
    if (!saves_of(host) || !slot_arg(*host, py_arg(0), slot)) {
        return false;
    }
    if (host->saves->store().slot_exists(slot)) {
        const SaveResult deleted = host->saves->store().delete_slot(slot);
        if (!deleted.ok) {
            return save_error(*host, deleted.message);
        }
    }
    py_newnone(py_retval());
    return true;
}

// slots(): a Slot for each, the newest first.
bool save_slots(int argc, py_StackRef argv) {
    PY_CHECK_ARGC(0);
    Host* host = nullptr;
    if (!saves_of(host)) {
        return false;
    }
    std::vector<SaveSlotInfo> slots = host->saves->store().list_slots();
    std::ranges::stable_sort(
        slots, [](const SaveSlotInfo& a, const SaveSlotInfo& b) { return a.updated_at_unix > b.updated_at_unix; });
    const py_StackRef list = py_pushtmp();
    py_newlist(list);
    for (const SaveSlotInfo& slot : slots) {
        py_newstrv(py_pushtmp(), view_of(slot.slot_id));
        py_newstrv(py_pushtmp(), view_of(slot.title));
        py_newstrv(py_pushtmp(), view_of(slot.summary));
        py_newint(py_pushtmp(), slot.updated_at_unix);
        py_newint(py_pushtmp(), slot.play_time_seconds);
        if (!make(*host, "Slot", 5)) {
            py_pop();
            return false;
        }
        const py_StackRef made = py_pushtmp();
        py_assign(made, py_retval());
        py_list_append(list, made);
        py_pop();
    }
    py_assign(py_retval(), list);
    py_pop();
    return true;
}

// get(name, default): the game's value, or default.
bool save_get(int argc, py_StackRef argv) {
    PY_CHECK_ARGC(2);
    Host* host = nullptr;
    if (!saves_of(host) || !py_checkstr(py_arg(0))) {
        return false;
    }
    const JsonValue* value = host->saves->value(view_of(py_arg(0)));
    if (!value) {
        py_assign(py_retval(), py_arg(1));
        return true;
    }
    const py_StackRef made = py_pushtmp();
    if (!python_of(*value, made)) {
        py_pop();
        return false;
    }
    py_assign(py_retval(), made);
    py_pop();
    return true;
}

// set(name, value): kept with the saves and from scene to scene; None takes it away.
bool save_set(int argc, py_StackRef argv) {
    PY_CHECK_ARGC(2);
    Host* host = nullptr;
    if (!saves_of(host) || !py_checkstr(py_arg(0))) {
        return false;
    }
    JsonValue value;
    if (!json_of(py_arg(1), value, 0)) {
        return false;
    }
    host->saves->set_value(std::string(view_of(py_arg(0))), std::move(value));
    py_newnone(py_retval());
    return true;
}

} // namespace

void bind_services(Host& host) {
    py_setdict(host.kin, py_name("physics"), host.physics_module);
#if KIN_ENABLE_PHYSICS
    py_bind(host.physics_module, "raycast(start, end, mask=65535, sensors=False, ignore=None)",
            guarded<physics_raycast>);
    py_bind(host.physics_module, "cast(start, end, radius=0.0, size=None, mask=65535, sensors=False, ignore=None)",
            guarded<physics_cast>);
    py_bind(host.physics_module, "overlap(at, radius=0.0, size=None, mask=65535, sensors=False, ignore=None)",
            guarded<physics_overlap>);
#else
    py_bind(host.physics_module, "raycast(*args, **kwargs)", no_physics);
    py_bind(host.physics_module, "cast(*args, **kwargs)", no_physics);
    py_bind(host.physics_module, "overlap(*args, **kwargs)", no_physics);
#endif

    py_setdict(host.kin, py_name("scene"), host.scene_module);
    py_bind(host.scene_module, "go(path='', arrive='', transition='fade', seconds=0.5)", guarded<scene_go>);
    py_bindfunc(host.scene_module, "going", guarded<scene_going>);

    py_setdict(host.kin, py_name("save"), host.save_module);
    py_bind(host.save_module, "write(slot, title='', summary='')", guarded<save_write>);
    py_bindfunc(host.save_module, "read", guarded<save_read>);
    py_bindfunc(host.save_module, "exists", guarded<save_exists>);
    py_bindfunc(host.save_module, "delete", guarded<save_delete>);
    py_bindfunc(host.save_module, "slots", guarded<save_slots>);
    py_bind(host.save_module, "get(name, default=None)", guarded<save_get>);
    py_bindfunc(host.save_module, "set", guarded<save_set>);
}

} // namespace kin::python_detail
