// The module `kin` as Python sees it (kin/scripting/python.hpp): the Entity
// type, @component, the native half of @system and @on, and world, input,
// time. Tasks and what they wait on are Python (python_kin.py).

#include "python_internal.hpp"

#include <kin/anim/value.hpp>
#include <kin/ecs/component_schema.hpp>
#include <kin/ecs/saving.hpp>
#include <kin/ecs/tween.hpp>
#include <kin/platform/input.hpp>

#include <algorithm>
#include <array>

namespace kin::python_detail {
namespace {

bool host_now(Host*& host) {
    host = current();
    return host != nullptr || no_host();
}

// An Entity method's entity, alive: false (raised) when not.
bool self_entity(Host*& host, py_Ref self, flecs::entity_t& entity) {
    if (!host_now(host)) {
        return false;
    }
    entity = live_entity_arg(*host, self);
    return entity != 0;
}

EcsEntity kin_entity(Host& host, flecs::entity_t entity) {
    return EcsEntity{flecs::entity(host.world.raw().c_ptr(), entity)};
}

// "player", or "the entity #12" without a name.
std::string entity_label(Host& host, flecs::entity_t entity) {
    const char* name = ecs_get_name(host.world.raw().c_ptr(), entity);
    return name && *name ? std::string(name) : "the entity #" + std::to_string(entity);
}

// --- kin.Entity ----------------------------------------------------------

bool entity_id(int argc, py_StackRef argv) {
    PY_CHECK_ARGC(1);
    Host* host = nullptr;
    if (!host_now(host)) {
        return false;
    }
    const flecs::entity_t entity = entity_arg(*host, py_arg(0));
    if (entity == 0) {
        return false;
    }
    py_newint(py_retval(), static_cast<py_i64>(entity));
    return true;
}

bool entity_alive(int argc, py_StackRef argv) {
    PY_CHECK_ARGC(1);
    Host* host = nullptr;
    if (!host_now(host)) {
        return false;
    }
    const flecs::entity_t entity = entity_arg(*host, py_arg(0));
    if (entity == 0) {
        return false;
    }
    py_newbool(py_retval(), ecs_is_alive(host->world.raw().c_ptr(), entity));
    return true;
}

bool entity_name(int argc, py_StackRef argv) {
    PY_CHECK_ARGC(1);
    Host* host = nullptr;
    flecs::entity_t entity = 0;
    if (!self_entity(host, py_arg(0), entity)) {
        return false;
    }
    const char* name = ecs_get_name(host->world.raw().c_ptr(), entity);
    py_newstr(py_retval(), name ? name : "");
    return true;
}

bool entity_parent(int argc, py_StackRef argv) {
    PY_CHECK_ARGC(1);
    Host* host = nullptr;
    flecs::entity_t entity = 0;
    if (!self_entity(host, py_arg(0), entity)) {
        return false;
    }
    const flecs::entity parent = flecs::entity(host->world.raw().c_ptr(), entity).parent();
    if (parent) {
        host->entity_object(py_retval(), parent.id());
    } else {
        py_newnone(py_retval());
    }
    return true;
}

bool entity_children(int argc, py_StackRef argv) {
    PY_CHECK_ARGC(1);
    Host* host = nullptr;
    flecs::entity_t entity = 0;
    if (!self_entity(host, py_arg(0), entity)) {
        return false;
    }
    std::vector<flecs::entity_t> children;
    flecs::entity(host->world.raw().c_ptr(), entity).children([&](flecs::entity child) { children.push_back(child.id()); });
    py_newlistn(py_retval(), static_cast<int>(children.size()));
    const py_StackRef list = py_pushtmp();
    py_assign(list, py_retval());
    for (std::size_t i = 0; i < children.size(); ++i) {
        host->entity_object(py_list_getitem(list, static_cast<int>(i)), children[i]);
    }
    py_assign(py_retval(), list);
    py_pop();
    return true;
}

bool entity_child(int argc, py_StackRef argv) {
    PY_CHECK_ARGC(2);
    Host* host = nullptr;
    flecs::entity_t entity = 0;
    if (!self_entity(host, py_arg(0), entity) || !py_checkstr(py_arg(1))) {
        return false;
    }
    const flecs::entity child = flecs::entity(host->world.raw().c_ptr(), entity).lookup(py_tostr(py_arg(1)));
    if (child) {
        host->entity_object(py_retval(), child.id());
    } else {
        py_newnone(py_retval());
    }
    return true;
}

bool entity_destroy(int argc, py_StackRef argv) {
    PY_CHECK_ARGC(1);
    Host* host = nullptr;
    flecs::entity_t entity = 0;
    if (!self_entity(host, py_arg(0), entity)) {
        return false;
    }
    const EcsEntity target = kin_entity(*host, entity);
    if (host->world.entities().registered(target)) {
        host->world.entities().destroy(target); // its authored id (and its children's) let go
    } else {
        target.raw().destruct();
    }
    py_newnone(py_retval());
    return true;
}

bool entity_has(int argc, py_StackRef argv) {
    PY_CHECK_ARGC(2);
    Host* host = nullptr;
    flecs::entity_t entity = 0;
    if (!self_entity(host, py_arg(0), entity)) {
        return false;
    }
    const ComponentInfo* info = component_arg(*host, py_arg(1));
    if (!info) {
        return false;
    }
    py_newbool(py_retval(), ecs_has_id(host->world.raw().c_ptr(), entity, info->id)); // a data component's tag too
    return true;
}

// The fields given as name=value, checked and converted.
bool fields_of(ComponentInfo& info, py_Ref fields, std::vector<std::pair<std::size_t, ComponentFieldValue>>& out) {
    if (py_isnone(fields)) {
        return true;
    }
    struct Context {
        ComponentInfo* info;
        std::vector<std::pair<std::size_t, ComponentFieldValue>>* out;
    } context{&info, &out};
    return py_dict_apply(
        fields,
        [](py_Ref key, py_Ref value, void* raw) -> bool {
            Context& c = *static_cast<Context*>(raw);
            if (!py_isstr(key)) {
                return TypeError("a field is named by text");
            }
            const py_Name name = py_namev(py_tosv(key));
            const auto found = std::ranges::find(c.info->names, name);
            if (found == c.info->names.end()) {
                return py_exception(tp_AttributeError, "%s has no field '%n'", c.info->name.c_str(), name);
            }
            const auto index = static_cast<std::size_t>(found - c.info->names.begin());
            std::optional<ComponentFieldValue> converted = from_python(c.info->kinds[index], value);
            if (!converted) {
                const std::string kind = component_field_kind_name(c.info->kinds[index]);
                return py_exception(tp_TypeError, "%s.%n takes a %s, not %t", c.info->name.c_str(), name, kind.c_str(),
                                    py_typeof(value));
            }
            c.out->emplace_back(index, std::move(*converted));
            return true;
        },
        &context);
}

bool apply_fields(Host& host, flecs::entity_t entity, ComponentInfo& info,
                  const std::vector<std::pair<std::size_t, ComponentFieldValue>>& fields) {
    if (fields.empty()) {
        return true;
    }
    EcsComponentRegistry& registry = host.world.components();
    const EcsEntity target = kin_entity(host, entity);
    flecs::world_t* w = host.world.raw().c_ptr();
    if (info.data) {
        std::vector<ComponentFieldValue>* values = registry.data_values(target, info.id);
        if (!values) {
            return py_exception(tp_RuntimeError, "%s's fields are not there", info.name.c_str());
        }
        for (const auto& [index, value] : fields) {
            (*values)[index] = value;
        }
        return true;
    }
    void* data = ecs_get_mut_id(w, entity, info.id);
    for (const auto& [index, value] : fields) {
        const bool written = data && info.access[index] ? info.access[index]->write(data, value)
                                                        : registry.patch_field(target, info.id, text_of_name(info.names[index]), value);
        if (!written) {
            return py_exception(tp_ValueError, "%s.%n could not be written", info.name.c_str(), info.names[index]);
        }
    }
    if (data) {
        ecs_modified_id(w, entity, info.id);
    }
    return true;
}

// add(component, **fields): the view, the component added (with those fields) when missing, else those fields set.
bool entity_add(int argc, py_StackRef argv) {
    PY_CHECK_ARGC(3);
    Host* host = nullptr;
    flecs::entity_t entity = 0;
    if (!self_entity(host, py_arg(0), entity)) {
        return false;
    }
    ComponentInfo* info = component_arg(*host, py_arg(1));
    if (!info) {
        return false;
    }
    std::vector<std::pair<std::size_t, ComponentFieldValue>> fields;
    if (!fields_of(*info, py_arg(2), fields)) {
        return false;
    }
    EcsComponentRegistry& registry = host->world.components();
    const EcsEntity target = kin_entity(*host, entity);
    if (!registry.has(target, info->id) && !registry.add(target, info->id)) {
        return py_exception(tp_ValueError, "%s could not be added: %s", info->name.c_str(), registry.last_error().c_str());
    }
    if (!apply_fields(*host, entity, *info, fields)) {
        return false;
    }
    return host->view_object(py_retval(), entity, *info);
}

bool entity_remove(int argc, py_StackRef argv) {
    PY_CHECK_ARGC(2);
    Host* host = nullptr;
    flecs::entity_t entity = 0;
    if (!self_entity(host, py_arg(0), entity)) {
        return false;
    }
    const ComponentInfo* info = component_arg(*host, py_arg(1));
    if (!info) {
        return false;
    }
    host->world.components().remove(kin_entity(*host, entity), info->id);
    py_newnone(py_retval());
    return true;
}

// get(component): its view, or None without it. e[component]: its view, KeyError without it.
template <bool Missing> bool entity_get(int argc, py_StackRef argv) {
    PY_CHECK_ARGC(2);
    Host* host = nullptr;
    flecs::entity_t entity = 0;
    if (!self_entity(host, py_arg(0), entity)) {
        return false;
    }
    ComponentInfo* info = component_arg(*host, py_arg(1));
    if (!info) {
        return false;
    }
    if (!ecs_has_id(host->world.raw().c_ptr(), entity, info->id)) {
        if constexpr (Missing) {
            py_newnone(py_retval());
            return true;
        } else {
            return py_exception(tp_KeyError, "%s has no %s", entity_label(*host, entity).c_str(),
                                info->name.c_str());
        }
    }
    return host->view_object(py_retval(), entity, *info);
}

bool entity_emit(int argc, py_StackRef argv) {
    PY_CHECK_ARGC(3);
    Host* host = nullptr;
    flecs::entity_t entity = 0;
    if (!self_entity(host, py_arg(0), entity) || !py_checkstr(py_arg(1))) {
        return false;
    }
    host->queue(std::string(view_of(py_arg(1))), entity, py_arg(2));
    py_newnone(py_retval());
    return true;
}

bool spawn_task(Host& host, py_Ref generator, py_Ref owner) {
    const std::string module = module_running();
    py_TValue name{};
    py_newstrv(&name, view_of(module));
    py_TValue args[3] = {*generator, *owner, name};
    return py_call(&host.spawn, 3, args);
}

bool entity_run(int argc, py_StackRef argv) {
    PY_CHECK_ARGC(2);
    Host* host = nullptr;
    flecs::entity_t entity = 0;
    if (!self_entity(host, py_arg(0), entity)) {
        return false;
    }
    return spawn_task(*host, py_arg(1), py_arg(0));
}

// The value a tween goes to, of its field's kind.
std::optional<AnimValue> anim_value_of(ComponentFieldKind kind, py_Ref value) {
    const std::optional<ComponentFieldValue> converted = from_python(kind, value);
    if (!converted) {
        return std::nullopt;
    }
    return std::visit(
        [](const auto& v) -> std::optional<AnimValue> {
            using V = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<V, f32> || std::is_same_v<V, Vec2f> || std::is_same_v<V, Color> ||
                          std::is_same_v<V, i32> || std::is_same_v<V, bool> || std::is_same_v<V, std::string>) {
                return AnimValue{v};
            } else if constexpr (std::is_same_v<V, f64>) {
                return AnimValue{static_cast<f32>(v)};
            } else {
                return std::nullopt;
            }
        },
        *converted);
}

// tween(field, to, seconds, ease="ease_out", delay=0.0, repeats=0, yoyo=False, relative=False)
bool entity_tween(int argc, py_StackRef argv) {
    PY_CHECK_ARGC(9);
    Host* host = nullptr;
    flecs::entity_t entity = 0;
    if (!self_entity(host, py_arg(0), entity) || !py_checkstr(py_arg(1)) || !py_checkstr(py_arg(4))) {
        return false;
    }
    const std::string property(view_of(py_arg(1)));
    const std::size_t dot = property.find('.');
    const ComponentInfo* info = dot == std::string::npos ? nullptr : host->info_for(std::string_view(property).substr(0, dot));
    if (!info) {
        return py_exception(tp_AttributeError, "tween takes \"Component.field\", not '%s'", property.c_str());
    }
    const py_Name field = py_namev(view_of(std::string_view(property).substr(dot + 1)));
    const auto found = std::ranges::find(info->names, field);
    if (found == info->names.end()) {
        return py_exception(tp_AttributeError, "%s has no field '%n'", info->name.c_str(), field);
    }
    const ComponentFieldKind kind = info->kinds[static_cast<std::size_t>(found - info->names.begin())];
    TweenDef def;
    def.property = property;
    const std::optional<AnimValue> to = anim_value_of(kind, py_arg(2));
    if (!to) {
        const std::string name = component_field_kind_name(kind);
        return py_exception(tp_TypeError, "%s takes a %s, not %t", property.c_str(), name.c_str(), py_typeof(py_arg(2)));
    }
    def.to = *to;
    const std::optional<f64> seconds = number_of(py_arg(3));
    const std::optional<f64> delay = number_of(py_arg(5));
    if (!seconds || !delay || !py_isint(py_arg(6))) {
        return TypeError("tween takes seconds and delay as numbers, repeats as an int");
    }
    def.seconds = static_cast<f32>(*seconds);
    def.delay = static_cast<f32>(*delay);
    def.repeats = static_cast<i32>(py_toint(py_arg(6)));
    // kin's names (easeout) or Python's spelling of them (ease_out).
    std::string easing(view_of(py_arg(4)));
    std::erase(easing, '_');
    if (!parse_easing(easing, def.easing)) {
        return py_exception(tp_ValueError, "no easing is named '%v'", py_tosv(py_arg(4)));
    }
    def.yoyo = py_tobool(py_arg(7));
    def.relative = py_tobool(py_arg(8));
    std::string error;
    const u32 id = tween(host->world, kin_entity(*host, entity), std::move(def), &error);
    if (id == 0) {
        return py_exception(tp_ValueError, "tween: %s", error.c_str());
    }
    py_TValue args[2];
    py_assign(&args[0], py_arg(0));
    py_newint(&args[1], id);
    return py_call(&host->tween, 2, args);
}

bool entity_eq(int argc, py_StackRef argv) {
    PY_CHECK_ARGC(2);
    Host* host = nullptr;
    if (!host_now(host)) {
        return false;
    }
    const bool same = py_istype(py_arg(1), host->entity_type) &&
                      static_cast<const EntityData*>(py_touserdata(py_arg(0)))->entity ==
                          static_cast<const EntityData*>(py_touserdata(py_arg(1)))->entity;
    py_newbool(py_retval(), same);
    return true;
}

bool entity_ne(int argc, py_StackRef argv) {
    if (!entity_eq(argc, argv)) {
        return false;
    }
    py_newbool(py_retval(), !py_tobool(py_retval()));
    return true;
}

bool entity_hash(int argc, py_StackRef argv) {
    PY_CHECK_ARGC(1);
    py_newint(py_retval(), static_cast<py_i64>(static_cast<const EntityData*>(py_touserdata(py_arg(0)))->entity >> 1));
    return true;
}

bool entity_repr(int argc, py_StackRef argv) {
    PY_CHECK_ARGC(1);
    Host* host = nullptr;
    if (!host_now(host)) {
        return false;
    }
    const flecs::entity_t entity = static_cast<const EntityData*>(py_touserdata(py_arg(0)))->entity;
    const flecs::entity found(host->world.raw().c_ptr(), entity);
    const std::string name = found.is_alive() ? std::string(found.name().c_str()) : std::string("gone");
    py_newfstr(py_retval(), "<Entity %s #%i>", name.c_str(), static_cast<py_i64>(entity));
    return true;
}

bool entity_new(int argc, py_StackRef argv) {
    (void)argc;
    (void)argv;
    return TypeError("entities are made by world.create() or world.spawn()");
}

// --- The decorators' native half --------------------------------------------

std::vector<ComponentInfo*> components_of(Host& host, py_Ref sequence, bool& ok) {
    std::vector<ComponentInfo*> found;
    ok = true;
    const int count = py_istuple(sequence) ? py_tuple_len(sequence) : -1;
    if (count < 0) {
        ok = TypeError("expected components");
        return found;
    }
    for (int i = 0; i < count; ++i) {
        ComponentInfo* info = component_arg(host, py_tuple_getitem(sequence, i));
        if (!info) {
            ok = false;
            return found;
        }
        found.push_back(info);
    }
    return found;
}

std::string function_name(py_Ref fn) {
    if (py_getattr(fn, py_name("__name__")) && py_isstr(py_retval())) {
        return std::string(view_of(py_retval()));
    }
    py_clearexc(nullptr);
    return "?";
}

u64 generation_of(Host& host, const std::string& module) {
    const auto found = host.modules.find(module);
    return found == host.modules.end() ? 0 : found->second.generation;
}

// _add_system(fn, components, without)
bool kin_add_system(int argc, py_StackRef argv) {
    PY_CHECK_ARGC(3);
    Host* host = nullptr;
    if (!host_now(host)) {
        return false;
    }
    if (!py_istype(py_arg(0), tp_function)) {
        return TypeError("@system(...) goes on a function");
    }
    bool ok = true;
    const std::vector<ComponentInfo*> with = components_of(*host, py_arg(1), ok);
    if (!ok) {
        return false;
    }
    const std::vector<ComponentInfo*> without = components_of(*host, py_arg(2), ok);
    if (!ok) {
        return false;
    }
    System made;
    made.module = module_of_function(py_arg(0));
    made.generation = generation_of(*host, made.module);
    made.name = function_name(py_arg(0));
    py_assign(&made.fn, py_arg(0));
    made.with = with;
    auto builder = host->world.raw().query_builder<>();
    for (const ComponentInfo* info : with) {
        builder.with(static_cast<flecs::id_t>(info->id));
    }
    for (const ComponentInfo* info : without) {
        made.without.push_back(info->id);
        builder.without(static_cast<flecs::id_t>(info->id));
    }
    made.query = builder.build();
    host->systems.push_back(std::move(made));
    py_newnone(py_retval());
    return true;
}

// _add_handler(fn, name, components)
bool kin_add_handler(int argc, py_StackRef argv) {
    PY_CHECK_ARGC(3);
    Host* host = nullptr;
    if (!host_now(host)) {
        return false;
    }
    if (!py_istype(py_arg(0), tp_function)) {
        return TypeError("@on(...) goes on a function");
    }
    if (!py_checkstr(py_arg(1))) {
        return false;
    }
    bool ok = true;
    const std::vector<ComponentInfo*> with = components_of(*host, py_arg(2), ok);
    if (!ok) {
        return false;
    }
    const std::string event(view_of(py_arg(1)));
    if (const std::string_view named = component_of_event(event); !named.empty()) {
        ComponentInfo* info = host->info_for(named);
        if (!info) {
            return py_exception(tp_TypeError, "no component is named '%s' (%s)", std::string(named).c_str(), event.c_str());
        }
        host->watch_event(*info, event);
    } else if (event == "destroyed") {
        if (with.empty()) {
            return TypeError("@on(\"destroyed\") takes the component of the entities it is for: @on(\"destroyed\", Health)");
        }
        host->watch_event(*with[0], event);
    }
    Handler made;
    made.module = module_of_function(py_arg(0));
    made.generation = generation_of(*host, made.module);
    made.event = std::string(view_of(py_arg(1)));
    made.name = function_name(py_arg(0));
    py_assign(&made.fn, py_arg(0));
    for (const ComponentInfo* info : with) {
        made.with.push_back(info->id);
    }
    host->handlers.push_back(std::move(made));
    py_newnone(py_retval());
    return true;
}

bool kin_is_component(int argc, py_StackRef argv) {
    PY_CHECK_ARGC(1);
    Host* host = nullptr;
    if (!host_now(host)) {
        return false;
    }
    bool is = false;
    if (py_istype(py_arg(0), tp_type)) {
        is = host->by_type.contains(py_totype(py_arg(0)));
    } else if (py_isstr(py_arg(0))) {
        is = host->world.components().find(view_of(py_arg(0))) != nullptr;
    }
    py_newbool(py_retval(), is);
    return true;
}

bool kin_component(int argc, py_StackRef argv) {
    PY_CHECK_ARGC(1);
    Host* host = nullptr;
    if (!host_now(host)) {
        return false;
    }
    return define_component(*host, py_arg(0));
}

bool kin_emit(int argc, py_StackRef argv) {
    PY_CHECK_ARGC(2);
    Host* host = nullptr;
    if (!host_now(host) || !py_checkstr(py_arg(0))) {
        return false;
    }
    host->queue(std::string(view_of(py_arg(0))), 0, py_arg(1));
    py_newnone(py_retval());
    return true;
}

bool kin_run(int argc, py_StackRef argv) {
    PY_CHECK_ARGC(1);
    Host* host = nullptr;
    if (!host_now(host)) {
        return false;
    }
    return spawn_task(*host, py_arg(0), py_None());
}

bool kin_tween_running(int argc, py_StackRef argv) {
    PY_CHECK_ARGC(2);
    Host* host = nullptr;
    if (!host_now(host) || !py_checkint(py_arg(1))) {
        return false;
    }
    const flecs::entity_t entity = entity_arg(*host, py_arg(0));
    bool running = false;
    if (entity != 0 && ecs_is_alive(host->world.raw().c_ptr(), entity)) {
        if (const Tweens* tweens = flecs::entity(host->world.raw().c_ptr(), entity).get<Tweens>()) {
            const auto id = static_cast<u32>(py_toint(py_arg(1)));
            running = std::ranges::any_of(tweens->running, [&](const Tweens::Running& run) { return run.id == id; });
        }
    }
    py_newbool(py_retval(), running);
    return true;
}

bool kin_tween_stop(int argc, py_StackRef argv) {
    PY_CHECK_ARGC(2);
    Host* host = nullptr;
    if (!host_now(host) || !py_checkint(py_arg(1))) {
        return false;
    }
    const flecs::entity_t entity = entity_arg(*host, py_arg(0));
    if (entity != 0 && ecs_is_alive(host->world.raw().c_ptr(), entity)) {
        stop_tween(kin_entity(*host, entity), static_cast<u32>(py_toint(py_arg(1))));
    }
    py_newnone(py_retval());
    return true;
}

// --- query, world ----------------------------------------------------------

// query(*components, without=None): each entity with them, as its views
// (one view when one component is asked for, else a tuple of them).
bool kin_query(int argc, py_StackRef argv) {
    PY_CHECK_ARGC(2);
    Host* host = nullptr;
    if (!host_now(host)) {
        return false;
    }
    bool ok = true;
    const std::vector<ComponentInfo*> with = components_of(*host, py_arg(0), ok);
    if (!ok) {
        return false;
    }
    if (with.empty()) {
        return TypeError("query() takes the components to look for");
    }
    std::vector<ComponentInfo*> without;
    if (!py_isnone(py_arg(1))) {
        if (py_istuple(py_arg(1))) {
            without = components_of(*host, py_arg(1), ok);
        } else if (ComponentInfo* info = component_arg(*host, py_arg(1))) {
            without.push_back(info);
        } else {
            ok = false;
        }
        if (!ok) {
            return false;
        }
    }
    auto builder = host->world.raw().query_builder<>();
    for (const ComponentInfo* info : with) {
        builder.with(static_cast<flecs::id_t>(info->id));
    }
    for (const ComponentInfo* info : without) {
        builder.without(static_cast<flecs::id_t>(info->id));
    }
    std::vector<flecs::entity_t> found;
    builder.build().each([&](flecs::entity entity) { found.push_back(entity.id()); });
    const py_StackRef list = py_pushtmp();
    py_newlistn(list, static_cast<int>(found.size()));
    for (std::size_t i = 0; i < found.size(); ++i) {
        const py_ItemRef item = py_list_getitem(list, static_cast<int>(i));
        if (with.size() == 1) {
            host->view_object(item, found[i], *with[0]);
            continue;
        }
        py_newtuple(item, static_cast<int>(with.size()));
        for (std::size_t c = 0; c < with.size(); ++c) {
            host->view_object(py_tuple_getitem(item, static_cast<int>(c)), found[i], *with[c]);
        }
    }
    py_assign(py_retval(), list);
    py_pop();
    return true;
}

// single(component): the view of the one entity with it; None when none has it.
bool world_single(int argc, py_StackRef argv) {
    PY_CHECK_ARGC(1);
    Host* host = nullptr;
    if (!host_now(host)) {
        return false;
    }
    ComponentInfo* info = component_arg(*host, py_arg(0));
    if (!info) {
        return false;
    }
    std::vector<flecs::entity_t> found;
    host->world.raw().query_builder<>().with(static_cast<flecs::id_t>(info->id)).build().each([&](flecs::entity entity) {
        found.push_back(entity.id());
    });
    if (found.size() > 1) {
        return py_exception(tp_RuntimeError, "%i entities have %s, not one", static_cast<py_i64>(found.size()),
                            info->name.c_str());
    }
    if (found.empty()) {
        py_newnone(py_retval());
        return true;
    }
    return host->view_object(py_retval(), found[0], *info);
}

bool world_find(int argc, py_StackRef argv) {
    PY_CHECK_ARGC(1);
    Host* host = nullptr;
    if (!host_now(host) || !py_checkstr(py_arg(0))) {
        return false;
    }
    const EcsEntity found = host->world.entities().find_by_name(view_of(py_arg(0)));
    if (found.valid()) {
        host->entity_object(py_retval(), found.raw().id());
    } else {
        py_newnone(py_retval());
    }
    return true;
}

// create(name=""): a new entity with no components.
bool world_create(int argc, py_StackRef argv) {
    PY_CHECK_ARGC(1);
    Host* host = nullptr;
    if (!host_now(host) || !py_checkstr(py_arg(0))) {
        return false;
    }
    const EcsEntity made = host->world.entities().create({.name = std::string(view_of(py_arg(0)))});
    host->entity_object(py_retval(), made.raw().id());
    return true;
}

std::shared_ptr<const PrefabAsset> prefab_of(Host& host, const std::string& path) {
    if (const auto found = host.prefabs.find(path); found != host.prefabs.end()) {
        return found->second;
    }
    const PrefabLoadResult loaded = load_prefab_asset(host.config.content / path, host.world.components());
    std::shared_ptr<const PrefabAsset> asset = loaded.prefab ? std::make_shared<const PrefabAsset>(*loaded.prefab) : nullptr;
    host.prefabs.emplace(path, asset);
    return asset;
}

// spawn(prefab, at=None): its root.
bool world_spawn(int argc, py_StackRef argv) {
    PY_CHECK_ARGC(2);
    Host* host = nullptr;
    if (!host_now(host) || !py_checkstr(py_arg(0))) {
        return false;
    }
    Vec2f at{};
    if (!py_isnone(py_arg(1))) {
        const std::optional<Vec2f> pair = pair_of(py_arg(1));
        if (!pair) {
            return TypeError("spawn's at is a vec2 or (x, y)");
        }
        at = *pair;
    }
    const std::string path(view_of(py_arg(0)));
    const std::shared_ptr<const PrefabAsset> asset = prefab_of(*host, path);
    if (!asset) {
        return py_exception(tp_ValueError, "the prefab '%s' could not be read", path.c_str());
    }
    const PrefabInstance made = instantiate_prefab(host->world, *asset, PrefabSpawn{.pos = at}, host->world.components());
    if (!made.root.valid()) {
        return py_exception(tp_ValueError, "the prefab '%s' made nothing", path.c_str());
    }
    made.root.raw().set<SpawnedPrefab>({.prefab = path}); // a save makes it again from it
    host->entity_object(py_retval(), made.root.raw().id());
    return true;
}

// --- input -----------------------------------------------------------------

template <int Which> bool input_action(int argc, py_StackRef argv) {
    PY_CHECK_ARGC(1);
    if (!py_checkstr(py_arg(0))) {
        return false;
    }
    const Host* host = current();
    const Input* input = host ? host->input : nullptr;
    const std::string_view action = view_of(py_arg(0));
    if constexpr (Which == 3) {
        py_newfloat(py_retval(), input ? input->axis(action) : 0.0);
    } else {
        bool result = false;
        if (input) {
            result = Which == 0 ? input->held(action) : Which == 1 ? input->pressed(action) : input->released(action);
        }
        py_newbool(py_retval(), result);
    }
    return true;
}

bool input_mouse(int argc, py_StackRef argv) {
    PY_CHECK_ARGC(0);
    const Host* host = current();
    new_vec2(py_retval(), host && host->input ? host->input->mouse_pos() : Vec2f{});
    return true;
}

// --- @component --------------------------------------------------------------

// A field's kind from its annotation (a name: PocketPy keeps annotations as text).
std::optional<ComponentFieldKind> kind_of_annotation(std::string_view text) {
    static constexpr std::array<std::pair<std::string_view, ComponentFieldKind>, 13> kinds{{
        {"bool", ComponentFieldKind::Bool},
        {"int", ComponentFieldKind::I32},
        {"i64", ComponentFieldKind::I64},
        {"float", ComponentFieldKind::F32},
        {"f64", ComponentFieldKind::F64},
        {"str", ComponentFieldKind::String},
        {"vec2", ComponentFieldKind::Vec2f},
        {"vec2i", ComponentFieldKind::Vec2i},
        {"color", ComponentFieldKind::Color},
        {"color32", ComponentFieldKind::Color},
        {"Entity", ComponentFieldKind::EntityRef},
        {"asset", ComponentFieldKind::AssetRef},
        {"list[vec2]", ComponentFieldKind::Points},
    }};
    for (const auto& [name, kind] : kinds) {
        if (text == name) {
            return kind;
        }
    }
    return std::nullopt;
}

ComponentFieldValue default_of(ComponentFieldKind kind) {
    switch (kind) {
    case ComponentFieldKind::Bool: return false;
    case ComponentFieldKind::I32: return i32{0};
    case ComponentFieldKind::I64: return i64{0};
    case ComponentFieldKind::F32: return 0.0f;
    case ComponentFieldKind::F64: return 0.0;
    case ComponentFieldKind::String: return std::string();
    case ComponentFieldKind::Vec2f: return Vec2f{};
    case ComponentFieldKind::Vec2i: return Vec2i{};
    case ComponentFieldKind::Color: return Color{255, 255, 255, 255};
    case ComponentFieldKind::EntityRef: return ComponentEntityRef{};
    case ComponentFieldKind::AssetRef: return ComponentAssetRef{};
    case ComponentFieldKind::Points: return std::vector<Vec2f>{};
    default: return false;
    }
}

// An attribute of a `field(...)`: false when it has none (raised).
bool field_option(py_Ref field, const char* name) {
    return py_getattr(field, py_name(name));
}

bool text_option(py_Ref field, const char* name, std::string& out) {
    if (!field_option(field, name)) {
        return false;
    }
    if (py_isstr(py_retval())) {
        out = std::string(view_of(py_retval()));
    }
    return true;
}

bool read_field_options(py_Ref field, ComponentSchemaField& out) {
    if (!text_option(field, "label", out.label) || !text_option(field, "description", out.description) ||
        !text_option(field, "units", out.units) || !text_option(field, "asset_type", out.asset_type)) {
        return false;
    }
    if (!field_option(field, "hidden")) {
        return false;
    }
    out.hidden = py_tobool(py_retval());
    if (!field_option(field, "readonly")) {
        return false;
    }
    out.readonly = py_tobool(py_retval());
    if (!field_option(field, "min")) {
        return false;
    }
    if (const std::optional<f64> min = number_of(py_retval())) {
        out.min_value = *min;
    }
    if (!field_option(field, "max")) {
        return false;
    }
    if (const std::optional<f64> max = number_of(py_retval())) {
        out.max_value = *max;
    }
    if (!field_option(field, "choices")) {
        return false;
    }
    if (py_islist(py_retval()) || py_istuple(py_retval())) {
        const py_StackRef choices = py_pushtmp();
        py_assign(choices, py_retval());
        const int count = py_islist(choices) ? py_list_len(choices) : py_tuple_len(choices);
        for (int i = 0; i < count; ++i) {
            const py_Ref choice = py_islist(choices) ? py_list_getitem(choices, i) : py_tuple_getitem(choices, i);
            if (py_isstr(choice)) {
                out.choices.emplace_back(view_of(choice));
            }
        }
        py_pop();
    }
    return true;
}

struct Annotated {
    std::string name;
    std::string annotation;
};

} // namespace

bool define_component(Host& host, py_Ref cls) {
    if (!py_istype(cls, tp_type)) {
        return TypeError("@component goes on a class");
    }
    const py_Type user_type = py_totype(cls);
    const std::string name = py_tpname(user_type);
    const std::string module = module_running();
    EcsComponentRegistry& registry = host.world.components();
    if (const ComponentDescriptor* existing = registry.find(name)) {
        ComponentInfo* info = host.info_for(existing->id);
        if (existing->kind != ComponentKind::Data) {
            return py_exception(tp_TypeError, "kin has a component named %s already", name.c_str());
        }
        if (info && info->scripted && info->module != module) {
            return py_exception(tp_TypeError, "the module %s defines %s already", info->module.c_str(), name.c_str());
        }
    }

    // Its fields: the annotations, in order, with their defaults.
    std::vector<Annotated> annotated;
    // A property of type (PocketPy makes the dict when asked).
    if (!py_getattr(cls, py_name("__annotations__"))) {
        return false;
    }
    const py_StackRef annotations = py_pushtmp();
    py_assign(annotations, py_retval());
    struct PopAnnotations {
        ~PopAnnotations() { py_pop(); }
    } const pop_annotations;
    if (py_istype(annotations, tp_dict)) {
        const bool listed = py_dict_apply(
            annotations,
            [](py_Ref key, py_Ref value, void* raw) -> bool {
                auto& out = *static_cast<std::vector<Annotated>*>(raw);
                if (!py_isstr(key)) {
                    return TypeError("a field is named by text");
                }
                std::string annotation;
                if (py_isstr(value)) {
                    annotation = std::string(view_of(value));
                } else if (py_istype(value, tp_type)) {
                    annotation = py_tpname(py_totype(value));
                }
                out.push_back({std::string(view_of(key)), std::move(annotation)});
                return true;
            },
            &annotated);
        if (!listed) {
            return false;
        }
    }
    const py_ItemRef field_class = py_getdict(host.kin, py_name("field"));
    ComponentSchema schema{.name = name};
    for (const Annotated& field : annotated) {
        if (field.name == "entity") {
            return py_exception(tp_TypeError, "%s.entity: a view's entity is named so; call the field otherwise", name.c_str());
        }
        const std::optional<ComponentFieldKind> kind = kind_of_annotation(field.annotation);
        if (!kind) {
            return py_exception(tp_TypeError,
                                "%s.%s: kin has no field of type '%s' (bool, int, float, str, vec2, vec2i, color, "
                                "Entity, asset, list[vec2], i64, f64)",
                                name.c_str(), field.name.c_str(), field.annotation.c_str());
        }
        ComponentSchemaField made{.name = field.name, .kind = *kind, .default_value = default_of(*kind)};
        if (const py_ItemRef given = py_tpfindname(user_type, py_namev(view_of(field.name)))) {
            py_Ref value = given;
            const py_StackRef kept = py_pushtmp();
            py_assign(kept, given);
            if (field_class && py_istype(field_class, tp_type) && py_isinstance(kept, py_totype(field_class))) {
                if (!read_field_options(kept, made) || !field_option(kept, "default")) {
                    py_pop();
                    return false;
                }
                py_assign(kept, py_retval());
            }
            value = kept;
            if (!py_isnone(value) || *kind == ComponentFieldKind::EntityRef) {
                std::optional<ComponentFieldValue> converted = from_python(*kind, value);
                if (!converted) {
                    const std::string kind_name = component_field_kind_name(*kind);
                    py_pop();
                    return py_exception(tp_TypeError, "%s.%s's default is not a %s", name.c_str(), field.name.c_str(),
                                        kind_name.c_str());
                }
                made.default_value = std::move(*converted);
            }
            py_pop();
        }
        schema.fields.push_back(std::move(made));
    }
    ComponentSchemaDocument document;
    document.components.push_back(std::move(schema));
    std::vector<std::string> diagnostics = validate_component_schema_asset(document);
    if (diagnostics.empty()) {
        apply_component_schema_asset(registry, document, diagnostics);
    }
    if (!diagnostics.empty()) {
        std::string why;
        for (const std::string& line : diagnostics) {
            why += (why.empty() ? "" : "; ") + line;
        }
        return py_exception(tp_TypeError, "%s: %s", name.c_str(), why.c_str());
    }

    ComponentInfo* info = host.info_for(name);
    if (!info) {
        return py_exception(tp_RuntimeError, "%s was not made", name.c_str());
    }
    host.refresh_fields(*info);
    info->scripted = true;
    info->module = module;
    info->generation = generation_of(host, module);

    // Its type gets the class's methods and constants (those of a reload replace the last).
    const py_GlobalRef type_object = py_tpobject(info->type);
    for (const py_Name copied : info->copied) {
        py_deldict(type_object, copied);
    }
    info->copied.clear();
    struct Copy {
        ComponentInfo* info;
        py_GlobalRef target;
    } copy{info, type_object};
    const bool copied = py_applydict(
        cls,
        [](py_Name key, py_Ref value, void* raw) -> bool {
            auto& c = *static_cast<Copy*>(raw);
            const std::string text = text_of_name(key);
            if (text.starts_with("__") || std::ranges::find(c.info->names, key) != c.info->names.end()) {
                return true;
            }
            py_setdict(c.target, key, value);
            c.info->copied.push_back(key);
            return true;
        },
        &copy);
    if (!copied) {
        return false;
    }

    // Its start and stop, and update as a system of its module.
    py_newnil(&info->start);
    py_newnil(&info->stop);
    if (const py_ItemRef start = py_tpfindname(user_type, py_name("start")); start && py_istype(start, tp_function)) {
        py_assign(&info->start, start);
    }
    if (const py_ItemRef stop = py_tpfindname(user_type, py_name("stop")); stop && py_istype(stop, tp_function)) {
        py_assign(&info->stop, stop);
    }
    host.watch_start_stop(*info);
    if (const py_ItemRef update = py_tpfindname(user_type, py_name("update")); update && py_istype(update, tp_function)) {
        System made;
        made.module = module;
        made.generation = generation_of(host, module);
        made.name = name + ".update";
        py_assign(&made.fn, update);
        made.with = {info};
        made.method = true;
        made.query = host.world.raw().query_builder<>().with(static_cast<flecs::id_t>(info->id)).build();
        host.systems.push_back(std::move(made));
    }
    py_assign(py_retval(), type_object);
    return true;
}

void set_up_entity_type(Host& host) {
    const py_Type entity = py_newtype("Entity", tp_object, host.kin, nullptr);
    py_tpsetfinal(entity);
    host.entity_type = entity;
    py_setdict(host.kin, py_name("Entity"), py_tpobject(entity));
    py_bindmagic(entity, py_name("__new__"), entity_new);
    py_bindproperty(entity, "id", guarded<entity_id>, nullptr);
    py_bindproperty(entity, "alive", guarded<entity_alive>, nullptr);
    py_bindproperty(entity, "name", guarded<entity_name>, nullptr);
    py_bindproperty(entity, "parent", guarded<entity_parent>, nullptr);
    py_bindproperty(entity, "children", guarded<entity_children>, nullptr);
    py_bindmethod(entity, "child", guarded<entity_child>);
    py_bindmethod(entity, "destroy", guarded<entity_destroy>);
    py_bindmethod(entity, "has", guarded<entity_has>);
    py_bind(py_tpobject(entity), "add(self, component, **fields)", guarded<entity_add>);
    py_bindmethod(entity, "remove", guarded<entity_remove>);
    py_bindmethod(entity, "get", guarded<entity_get<true>>);
    py_bindmagic(entity, py_name("__getitem__"), guarded<entity_get<false>>);
    py_bind(py_tpobject(entity), "emit(self, name, data=None)", guarded<entity_emit>);
    py_bindmethod(entity, "run", guarded<entity_run>);
    py_bind(py_tpobject(entity),
            "tween(self, field, to, seconds, ease='ease_out', delay=0.0, repeats=0, yoyo=False, relative=False)",
            guarded<entity_tween>);
    py_bindmagic(entity, py_name("__eq__"), guarded<entity_eq>);
    py_bindmagic(entity, py_name("__ne__"), guarded<entity_ne>);
    py_bindmagic(entity, py_name("__hash__"), guarded<entity_hash>);
    py_bindmagic(entity, py_name("__repr__"), guarded<entity_repr>);
}

void bind_kin(Host& host) {
    set_up_entity_type(host);
    const py_GlobalRef kin = host.kin;
    py_bindfunc(kin, "component", guarded<kin_component>);
    py_bindfunc(kin, "_add_system", guarded<kin_add_system>);
    py_bindfunc(kin, "_add_handler", guarded<kin_add_handler>);
    py_bindfunc(kin, "_is_component", guarded<kin_is_component>);
    py_bind(kin, "emit(name, data=None)", guarded<kin_emit>);
    py_bindfunc(kin, "run", guarded<kin_run>);
    py_bind(kin, "query(*components, without=None)", guarded<kin_query>);
    py_bindfunc(kin, "_tween_running", guarded<kin_tween_running>);
    py_bindfunc(kin, "_tween_stop", guarded<kin_tween_stop>);
    py_setdict(kin, py_name("vec2"), py_tpobject(tp_vec2));
    py_setdict(kin, py_name("vec2i"), py_tpobject(tp_vec2i));
    py_setdict(kin, py_name("color"), py_tpobject(tp_color32));

    py_setdict(kin, py_name("world"), host.world_module);
    py_bindfunc(host.world_module, "find", guarded<world_find>);
    py_bind(host.world_module, "create(name='')", guarded<world_create>);
    py_bind(host.world_module, "spawn(prefab, at=None)", guarded<world_spawn>);
    py_bindfunc(host.world_module, "single", guarded<world_single>);
    py_bind(host.world_module, "query(*components, without=None)", guarded<kin_query>);

    py_setdict(kin, py_name("input"), host.input_module);
    py_bindfunc(host.input_module, "held", guarded<input_action<0>>);
    py_bindfunc(host.input_module, "pressed", guarded<input_action<1>>);
    py_bindfunc(host.input_module, "released", guarded<input_action<2>>);
    py_bindfunc(host.input_module, "axis", guarded<input_action<3>>);
    py_bindfunc(host.input_module, "mouse", guarded<input_mouse>);

    py_setdict(kin, py_name("time"), host.time);
    py_newfloat(py_emplacedict(host.time, py_name("dt")), 0.0);
    py_newfloat(py_emplacedict(host.time, py_name("now")), 0.0);
    py_newint(py_emplacedict(host.time, py_name("frame")), 0);
}

} // namespace kin::python_detail
