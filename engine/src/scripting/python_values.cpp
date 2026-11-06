// Python's values of kin (kin/scripting/python.hpp): entities, the views of
// their components, and fields' values as Python holds them.

#include "python_internal.hpp"

#if KIN_ENABLE_PHYSICS
#include <kin/ecs/physics.hpp>
#endif

#include <algorithm>
#include <new>
#include <type_traits>

namespace kin::python_detail {

c11_sv view_of(std::string_view text) {
    return c11_sv{text.data(), static_cast<int>(text.size())};
}

std::string_view view_of(py_Ref value) {
    const c11_sv text = py_tosv(value);
    return {text.data, static_cast<std::size_t>(text.size)};
}

std::string text_of_name(py_Name name) {
    const c11_sv text = py_name2sv(name);
    return {text.data, static_cast<std::size_t>(text.size)};
}

void new_vec2(py_OutRef out, Vec2f value) {
    c11_vec2 made{};
    made.x = value.x;
    made.y = value.y;
    py_newvec2(out, made);
}

std::optional<f64> number_of(py_Ref value) {
    if (py_isint(value)) {
        return static_cast<f64>(py_toint(value));
    }
    if (py_isfloat(value)) {
        return py_tofloat(value);
    }
    return std::nullopt;
}

namespace {

// The items of a tuple or list.
std::optional<std::pair<py_Ref, int>> items_of(py_Ref value) {
    if (py_istuple(value)) {
        return std::pair{py_tuple_data(value), py_tuple_len(value)};
    }
    if (py_islist(value)) {
        return std::pair{py_list_data(value), py_list_len(value)};
    }
    return std::nullopt;
}

} // namespace

std::optional<Vec2f> pair_of(py_Ref value) {
    if (py_istype(value, tp_vec2)) {
        const c11_vec2 v = py_tovec2(value);
        return Vec2f{v.x, v.y};
    }
    if (py_istype(value, tp_vec2i)) {
        const c11_vec2i v = py_tovec2i(value);
        return Vec2f{static_cast<f32>(v.x), static_cast<f32>(v.y)};
    }
    const auto items = items_of(value);
    if (!items || items->second != 2) {
        return std::nullopt;
    }
    const std::optional<f64> x = number_of(&items->first[0]);
    const std::optional<f64> y = number_of(&items->first[1]);
    if (!x || !y) {
        return std::nullopt;
    }
    return Vec2f{static_cast<f32>(*x), static_cast<f32>(*y)};
}

void to_python(Host& host, py_OutRef out, const ComponentFieldValue& value) {
    std::visit(
        [&](const auto& v) {
            using V = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<V, bool>) {
                py_newbool(out, v);
            } else if constexpr (std::is_same_v<V, std::string>) {
                py_newstrv(out, view_of(v));
            } else if constexpr (std::is_integral_v<V>) {
                py_newint(out, static_cast<py_i64>(v));
            } else if constexpr (std::is_floating_point_v<V>) {
                py_newfloat(out, static_cast<py_f64>(v));
            } else if constexpr (std::is_same_v<V, Vec2f>) {
                new_vec2(out, v);
            } else if constexpr (std::is_same_v<V, Vec2i>) {
                c11_vec2i made{};
                made.x = v.x;
                made.y = v.y;
                py_newvec2i(out, made);
            } else if constexpr (std::is_same_v<V, Color>) {
                c11_color32 made{};
                made.r = v.r;
                made.g = v.g;
                made.b = v.b;
                made.a = v.a;
                py_newcolor32(out, made);
            } else if constexpr (std::is_same_v<V, ComponentAssetRef>) {
                py_newstrv(out, view_of(v.id));
            } else if constexpr (std::is_same_v<V, ComponentEntityRef>) {
                if (v.id != 0) {
                    host.entity_object(out, v.id);
                } else {
                    py_newnone(out);
                }
            } else if constexpr (std::is_same_v<V, std::vector<Vec2f>>) {
                py_newlistn(out, static_cast<int>(v.size()));
                for (std::size_t i = 0; i < v.size(); ++i) {
                    new_vec2(py_list_getitem(out, static_cast<int>(i)), v[i]);
                }
            } else {
                py_newnone(out); // 3D values: not for 2D scripts yet
            }
        },
        value);
}

std::optional<ComponentFieldValue> from_python(ComponentFieldKind kind, py_Ref value) {
    switch (kind) {
    case ComponentFieldKind::Bool:
        if (py_isbool(value)) {
            return py_tobool(value);
        }
        break;
    case ComponentFieldKind::I32:
        if (py_isint(value)) {
            return static_cast<i32>(py_toint(value));
        }
        break;
    case ComponentFieldKind::I64:
        if (py_isint(value)) {
            return static_cast<i64>(py_toint(value));
        }
        break;
    case ComponentFieldKind::U32:
        if (py_isint(value) && py_toint(value) >= 0) {
            return static_cast<u32>(py_toint(value));
        }
        break;
    case ComponentFieldKind::U64:
        if (py_isint(value) && py_toint(value) >= 0) {
            return static_cast<u64>(py_toint(value));
        }
        break;
    case ComponentFieldKind::F32:
        if (const std::optional<f64> number = number_of(value)) {
            return static_cast<f32>(*number);
        }
        break;
    case ComponentFieldKind::F64:
        if (const std::optional<f64> number = number_of(value)) {
            return *number;
        }
        break;
    case ComponentFieldKind::String:
        if (py_isstr(value)) {
            return std::string(view_of(value));
        }
        break;
    case ComponentFieldKind::AssetRef:
        if (py_isstr(value)) {
            return ComponentAssetRef{std::string(view_of(value))};
        }
        break;
    case ComponentFieldKind::EntityRef:
        if (py_isnone(value)) {
            return ComponentEntityRef{};
        }
        if (const Host* host = current(); host && py_istype(value, host->entity_type)) {
            return ComponentEntityRef{.id = static_cast<const EntityData*>(py_touserdata(value))->entity};
        }
        break;
    case ComponentFieldKind::Vec2f:
        if (const std::optional<Vec2f> pair = pair_of(value)) {
            return *pair;
        }
        break;
    case ComponentFieldKind::Vec2i:
        if (const std::optional<Vec2f> pair = pair_of(value)) {
            return Vec2i{static_cast<i32>(pair->x), static_cast<i32>(pair->y)};
        }
        break;
    case ComponentFieldKind::Color:
        if (py_istype(value, tp_color32)) {
            const c11_color32 c = py_tocolor32(value);
            return Color{c.r, c.g, c.b, c.a};
        }
        if (const auto items = items_of(value); items && (items->second == 3 || items->second == 4)) {
            u8 channels[4] = {255, 255, 255, 255};
            for (int i = 0; i < items->second; ++i) {
                const py_Ref channel = &items->first[i];
                if (!py_isint(channel)) {
                    return std::nullopt;
                }
                channels[i] = static_cast<u8>(std::clamp<py_i64>(py_toint(channel), 0, 255));
            }
            return Color{channels[0], channels[1], channels[2], channels[3]};
        }
        break;
    case ComponentFieldKind::Points:
        if (const auto items = items_of(value)) {
            std::vector<Vec2f> points;
            points.reserve(static_cast<std::size_t>(items->second));
            for (int i = 0; i < items->second; ++i) {
                const std::optional<Vec2f> point = pair_of(&items->first[i]);
                if (!point) {
                    return std::nullopt;
                }
                points.push_back(*point);
            }
            return points;
        }
        break;
    default:
        break;
    }
    return std::nullopt;
}

// --- Components --------------------------------------------------------------

void Host::refresh_fields(ComponentInfo& info) {
    const ComponentDescriptor* descriptor = world.components().find(info.id);
    info.names.clear();
    info.kinds.clear();
    info.readonly.clear();
    info.access.clear();
    if (!descriptor) {
        return;
    }
    info.name = descriptor->name;
    info.data = descriptor->kind == ComponentKind::Data;
    for (const ComponentFieldDescriptor& field : descriptor->fields) {
        info.names.push_back(py_namev(view_of(field.name)));
        info.kinds.push_back(field.kind);
        info.readonly.push_back(field.readonly);
        info.access.push_back(info.data ? std::nullopt : world.components().field_access(info.name, field.name));
    }
}

ComponentInfo* Host::info_for(ComponentId id) {
    if (const auto found = components.find(id); found != components.end()) {
        return found->second.get();
    }
    if (!world.components().find(id)) {
        return nullptr;
    }
    auto made = std::make_unique<ComponentInfo>();
    ComponentInfo& info = *made;
    info.id = id;
    refresh_fields(info);
    components.emplace(id, std::move(made));
    set_up_view_type(*this, info);
    return &info;
}

ComponentInfo* Host::info_for(std::string_view name) {
    const ComponentDescriptor* descriptor = world.components().find(name);
    return descriptor ? info_for(descriptor->id) : nullptr;
}

// Each component described, as kin.<its name> (unless kin has that name already).
void Host::expose_components() {
    const std::vector<ComponentDescriptor> described = world.components().descriptors();
    if (described.size() == descriptors_seen) {
        return;
    }
    descriptors_seen = described.size();
    for (const ComponentDescriptor& descriptor : described) {
        ComponentInfo* info = info_for(descriptor.id);
        if (!info || info->scripted) {
            continue;
        }
        const py_Name name = py_namev(view_of(descriptor.name));
        if (!py_getdict(kin, name)) {
            py_setdict(kin, name, py_tpobject(info->type));
        }
    }
}

ComponentInfo* component_arg(Host& host, py_Ref value) {
    if (py_istype(value, tp_type)) {
        if (const auto found = host.by_type.find(py_totype(value)); found != host.by_type.end()) {
            return found->second;
        }
        return py_exception(tp_TypeError, "%s is not a component", py_tpname(py_totype(value))), nullptr;
    }
    if (py_isstr(value)) {
        if (ComponentInfo* info = host.info_for(view_of(value))) {
            return info;
        }
        return py_exception(tp_TypeError, "no component is named '%v'", py_tosv(value)), nullptr;
    }
    return py_exception(tp_TypeError, "expected a component (Transform2D, or its name), not %t",
                        py_typeof(value)), nullptr;
}

// --- Views -------------------------------------------------------------------

namespace {

flecs::world_t* raw_world(Host& host) {
    return host.world.raw().c_ptr();
}

std::string entity_text(Host& host, flecs::entity_t entity) {
    const flecs::entity found(raw_world(host), entity);
    if (!found.is_alive()) {
        return "<Entity gone #" + std::to_string(entity) + ">";
    }
    const char* name = found.name().c_str();
    return std::string("<Entity ") + (name && *name ? name : "") + " #" + std::to_string(entity) + ">";
}

bool gone(const View& view) {
    return py_exception(tp_RuntimeError, "the entity of this %s is gone", view.info->name.c_str());
}

bool missing(Host& host, const View& view) {
    const std::string text = entity_text(host, view.entity);
    return py_exception(tp_RuntimeError, "%s has no %s now", text.c_str(), view.info->name.c_str());
}

bool read_field(Host& host, const View& view, std::size_t index) {
    flecs::world_t* w = raw_world(host);
    if (!ecs_is_alive(w, view.entity)) {
        return gone(view);
    }
    const ComponentInfo& info = *view.info;
    if (info.data) {
        const std::vector<ComponentFieldValue>* values = host.data_values(view.entity, info.id);
        if (!values || index >= values->size()) {
            return missing(host, view);
        }
        to_python(host, py_retval(), (*values)[index]);
        return true;
    }
    const void* data = ecs_get_id(w, view.entity, info.id);
    if (!data) {
        return missing(host, view);
    }
    if (info.access[index]) {
        to_python(host, py_retval(), info.access[index]->read(data));
        return true;
    }
    // A field only the registry reads (no direct access).
    const std::string field = text_of_name(info.names[index]);
    bool found = false;
    host.world.components().visit_fields(EcsEntity{flecs::entity(w, view.entity)}, info.id,
                                         [&](std::string_view name, ComponentFieldKind, const ComponentFieldValue& value) {
                                             if (!found && name == field) {
                                                 found = true;
                                                 to_python(host, py_retval(), value);
                                             }
                                         });
    return found || py_exception(tp_RuntimeError, "%s.%s could not be read", info.name.c_str(), field.c_str());
}

// An entity moved by a script: its body goes there at the next step.
void moved(Host& host, flecs::entity_t entity, const ComponentInfo& info) {
#if KIN_ENABLE_PHYSICS
    if (info.name == "Transform2D") {
        flecs::entity found(raw_world(host), entity);
        if (found.has<PhysicsBodyComponent>()) {
            found.add<PhysicsTeleport>();
        }
    }
#else
    (void)host;
    (void)entity;
    (void)info;
#endif
}

bool write_field(Host& host, const View& view, std::size_t index, py_Ref value) {
    const ComponentInfo& info = *view.info;
    const std::string field = text_of_name(info.names[index]);
    if (info.readonly[index]) {
        return py_exception(tp_AttributeError, "%s.%s is read only", info.name.c_str(), field.c_str());
    }
    const std::optional<ComponentFieldValue> converted = from_python(info.kinds[index], value);
    if (!converted) {
        const std::string kind = component_field_kind_name(info.kinds[index]);
        return py_exception(tp_TypeError, "%s.%s takes a %s, not %t", info.name.c_str(), field.c_str(), kind.c_str(),
                            py_typeof(value));
    }
    flecs::world_t* w = raw_world(host);
    if (!ecs_is_alive(w, view.entity)) {
        return gone(view);
    }
    if (info.data) {
        std::vector<ComponentFieldValue>* values = host.writable_data_values(view.entity, info.id);
        if (!values || index >= values->size()) {
            return missing(host, view);
        }
        (*values)[index] = *converted;
        return true;
    }
    if (info.access[index]) {
        // In place when it owns it, so what the script reads next is what it
        // wrote; its own copy when it has it from a prefab (IsA).
        void* data = ecs_get_mut_id(w, view.entity, info.id);
        if (!data && ecs_has_id(w, view.entity, info.id)) {
            data = ecs_ensure_id(w, view.entity, info.id);
        }
        if (!data) {
            return missing(host, view);
        }
        if (!info.access[index]->write(data, *converted)) {
            return py_exception(tp_ValueError, "%s.%s could not be written", info.name.c_str(), field.c_str());
        }
        ecs_modified_id(w, view.entity, info.id);
    } else if (!host.world.components().patch_field(EcsEntity{flecs::entity(w, view.entity)}, info.id, field, *converted)) {
        return py_exception(tp_ValueError, "%s.%s: %s", info.name.c_str(), field.c_str(),
                            host.world.components().last_error().c_str());
    }
    moved(host, view.entity, info);
    return true;
}

// What the view's type has (a method, a property, a constant), as Python finds it.
bool class_attribute(py_Ref self, py_Name name, const ComponentInfo& info) {
    const py_Type type = py_typeof(self);
    const py_ItemRef found = py_tpfindname(type, name);
    if (!found) {
        return py_exception(tp_AttributeError, "%s has no field or method '%n'", info.name.c_str(), name);
    }
    if (py_istype(found, tp_property)) {
        return py_call(py_getslot(found, 0), 1, self);
    }
    if (py_istype(found, tp_function) || py_istype(found, tp_nativefunc)) {
        py_newboundmethod(py_retval(), self, found);
        return true;
    }
    if (py_istype(found, tp_staticmethod)) {
        py_assign(py_retval(), py_getslot(found, 0));
        return true;
    }
    if (py_istype(found, tp_classmethod)) {
        py_newboundmethod(py_retval(), py_tpobject(type), py_getslot(found, 0));
        return true;
    }
    py_assign(py_retval(), found);
    return true;
}

std::optional<std::size_t> field_index(const ComponentInfo& info, py_Name name) {
    for (std::size_t i = 0; i < info.names.size(); ++i) {
        if (info.names[i] == name) {
            return i;
        }
    }
    return std::nullopt;
}

bool view_getattribute(py_Ref self, py_Name name) {
    Host* host = current();
    if (!host) {
        return no_host();
    }
    const View& view = *static_cast<const View*>(py_touserdata(self));
    if (const std::optional<std::size_t> index = field_index(*view.info, name)) {
        return read_field(*host, view, *index);
    }
    if (name == py_name("entity")) {
        host->entity_object(py_retval(), view.entity);
        return true;
    }
    return class_attribute(self, name, *view.info);
}

bool view_setattribute(py_Ref self, py_Name name, py_Ref value) {
    Host* host = current();
    if (!host) {
        return no_host();
    }
    const View& view = *static_cast<const View*>(py_touserdata(self));
    if (const std::optional<std::size_t> index = field_index(*view.info, name)) {
        return write_field(*host, view, *index, value);
    }
    if (const py_ItemRef found = py_tpfindname(py_typeof(self), name); found && py_istype(found, tp_property)) {
        const py_Ref setter = py_getslot(found, 1);
        if (!py_isnone(setter)) {
            py_push(setter);
            py_push(self);
            py_push(value);
            return py_vectorcall(1, 0);
        }
    }
    return py_exception(tp_AttributeError, "%s has no field '%n' (a component keeps what it holds in its fields)",
                        view.info->name.c_str(), name);
}

bool view_delattribute(py_Ref self, py_Name name) {
    const View& view = *static_cast<const View*>(py_touserdata(self));
    return py_exception(tp_AttributeError, "%s's fields cannot be deleted ('%n')", view.info->name.c_str(), name);
}

bool view_repr(int argc, py_StackRef argv) {
    PY_CHECK_ARGC(1);
    Host* host = current();
    if (!host) {
        return no_host();
    }
    const View& view = *static_cast<const View*>(py_touserdata(py_arg(0)));
    const std::string text = "<" + view.info->name + " of " + entity_text(*host, view.entity) + ">";
    py_newstrv(py_retval(), view_of(text));
    return true;
}

bool view_eq(int argc, py_StackRef argv) {
    PY_CHECK_ARGC(2);
    const View& view = *static_cast<const View*>(py_touserdata(py_arg(0)));
    if (py_typeof(py_arg(1)) != py_typeof(py_arg(0))) {
        py_newbool(py_retval(), false);
        return true;
    }
    const View& other = *static_cast<const View*>(py_touserdata(py_arg(1)));
    py_newbool(py_retval(), view.entity == other.entity && view.info == other.info);
    return true;
}

bool view_ne(int argc, py_StackRef argv) {
    if (!view_eq(argc, argv)) {
        return false;
    }
    py_newbool(py_retval(), !py_tobool(py_retval()));
    return true;
}

bool view_hash(int argc, py_StackRef argv) {
    PY_CHECK_ARGC(1);
    const View& view = *static_cast<const View*>(py_touserdata(py_arg(0)));
    py_newint(py_retval(), static_cast<py_i64>(ViewKeyHash{}(ViewKey{view.entity, view.info->id}) >> 1));
    return true;
}

bool view_new(int argc, py_StackRef argv) {
    const py_Type type = py_istype(py_arg(0), tp_type) ? py_totype(py_arg(0)) : py_typeof(py_arg(0));
    (void)argc;
    return py_exception(tp_TypeError, "%s is a component: add it to an entity (e.add(%s, ...))", py_tpname(type),
                        py_tpname(type));
}

} // namespace

void set_up_view_type(Host& host, ComponentInfo& info) {
    info.type = py_newtype(info.name.c_str(), tp_object, host.kin, nullptr);
    py_tphookattributes(info.type, view_getattribute, view_setattribute, view_delattribute, nullptr);
    py_tpsetfinal(info.type);
    py_bindmagic(info.type, py_name("__new__"), view_new);
    py_bindmagic(info.type, py_name("__repr__"), guarded<view_repr>);
    py_bindmagic(info.type, py_name("__eq__"), view_eq);
    py_bindmagic(info.type, py_name("__ne__"), view_ne);
    py_bindmagic(info.type, py_name("__hash__"), view_hash);
    host.by_type[info.type] = &info;
}

namespace {

// The slot of `entity` in a cache by entity index: true when it holds its object already.
bool slot_of(std::vector<std::pair<flecs::entity_t, py_TValue>>& cache, flecs::entity_t entity,
             std::pair<flecs::entity_t, py_TValue>*& slot) {
    const auto index = static_cast<std::size_t>(static_cast<u32>(entity));
    if (index >= cache.size()) {
        cache.resize(std::max(index + 1, cache.size() * 2));
    }
    slot = &cache[index];
    if (slot->first == entity) {
        return true;
    }
    slot->first = entity;
    return false;
}

} // namespace

bool Host::view_object(py_OutRef out, flecs::entity_t entity, ComponentInfo& info) {
    std::pair<flecs::entity_t, py_TValue>* slot = nullptr;
    if (!slot_of(info.views, entity, slot)) {
        void* data = py_newobject(&slot->second, info.type, 0, sizeof(View));
        new (data) View{entity, &info};
    }
    py_assign(out, &slot->second);
    return true;
}

// --- Entities ----------------------------------------------------------------

void Host::entity_object(py_OutRef out, flecs::entity_t entity) {
    std::pair<flecs::entity_t, py_TValue>* slot = nullptr;
    if (!slot_of(entities, entity, slot)) {
        void* data = py_newobject(&slot->second, entity_type, 0, sizeof(EntityData));
        new (data) EntityData{entity};
    }
    py_assign(out, &slot->second);
}

const std::vector<ComponentFieldValue>* Host::data_values(flecs::entity_t entity, ComponentId id) const {
    // Its own, or (a prefab instance) its prefab's.
    const auto* payload =
        static_cast<const detail::DataComponentPayload*>(ecs_get_id(world.raw().c_ptr(), entity, data_payload));
    if (!payload) {
        return nullptr;
    }
    const auto found = payload->values.find(id);
    return found == payload->values.end() ? nullptr : &found->second;
}

std::vector<ComponentFieldValue>* Host::writable_data_values(flecs::entity_t entity, ComponentId id) {
    if (auto* payload =
            static_cast<detail::DataComponentPayload*>(ecs_get_mut_id(world.raw().c_ptr(), entity, data_payload))) {
        const auto found = payload->values.find(id);
        if (found != payload->values.end()) {
            return &found->second;
        }
    }
    // Not its own yet (a prefab instance's): the registry makes it its own.
    return world.components().data_values(EcsEntity{flecs::entity(world.raw().c_ptr(), entity)}, id);
}

flecs::entity_t entity_arg(Host& host, py_Ref value) {
    if (!py_istype(value, host.entity_type)) {
        return py_exception(tp_TypeError, "expected an Entity, not %t", py_typeof(value)), 0;
    }
    return static_cast<const EntityData*>(py_touserdata(value))->entity;
}

flecs::entity_t live_entity_arg(Host& host, py_Ref value) {
    const flecs::entity_t entity = entity_arg(host, value);
    if (entity == 0) {
        return 0;
    }
    if (!ecs_is_alive(host.world.raw().c_ptr(), entity)) {
        return py_exception(tp_RuntimeError, "the entity is gone (#%i)", static_cast<py_i64>(entity)), 0;
    }
    return entity;
}

} // namespace kin::python_detail
