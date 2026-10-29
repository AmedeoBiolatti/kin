#pragma once

#include <kin/anim/value.hpp>
#include <kin/ecs/component.hpp>
#include <kin/ecs/world.hpp>
#include <kin/renderer/sprite_catalog.hpp>

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace kin {

class PropertyRegistry;

struct PropertyAccessor {
    ComponentId component = 0;
    AnimValueKind kind = AnimValueKind::Float;
    std::function<AnimValue(const void*)> get;
    std::function<void(void*, const AnimValue&)> set;
};

template <typename T>
struct AnimValueKindOf;

template <>
struct AnimValueKindOf<f32> {
    static constexpr AnimValueKind value = AnimValueKind::Float;
};

template <>
struct AnimValueKindOf<Vec2f> {
    static constexpr AnimValueKind value = AnimValueKind::Vec2;
};

template <>
struct AnimValueKindOf<Color> {
    static constexpr AnimValueKind value = AnimValueKind::Color;
};

template <>
struct AnimValueKindOf<i32> {
    static constexpr AnimValueKind value = AnimValueKind::Int;
};

template <>
struct AnimValueKindOf<bool> {
    static constexpr AnimValueKind value = AnimValueKind::Bool;
};

template <>
struct AnimValueKindOf<std::string> {
    static constexpr AnimValueKind value = AnimValueKind::String;
};

class PropertyRegistry {
public:
    template <typename C, typename T>
    void add(EcsWorld& world, std::string key, T C::* member) {
        PropertyAccessor accessor;
        accessor.component = world.component<C>().id();
        accessor.kind = AnimValueKindOf<T>::value;
        accessor.get = [member](const void* ptr) -> AnimValue {
            return static_cast<const C*>(ptr)->*member;
        };
        accessor.set = [member](void* ptr, const AnimValue& value) {
            if (const auto* typed = std::get_if<T>(&value)) {
                static_cast<C*>(ptr)->*member = *typed;
            }
        };
        add(std::move(key), std::move(accessor));
    }

    void add(std::string key, PropertyAccessor accessor);
    const PropertyAccessor* find(std::string_view key) const;

private:
    std::unordered_map<std::string, PropertyAccessor> _props;
};

void register_builtin_properties(PropertyRegistry& registry, EcsWorld& world);
// Every field of the world's described native components that holds a number,
// a pair, a colour, a whole number, a check or text (and is not read-only), as
// "Component.field" ("Transform2D.pos", "TextRenderer.color"): what scene
// files and editors animate.
void register_described_properties(PropertyRegistry& registry, const EcsWorld& world);
// The kind of value a property of that kind animates; none for kinds that
// are not animated (an asset, an entity, a 64-bit number).
std::optional<AnimValueKind> anim_value_kind_of(ComponentFieldKind kind);
bool set_sprite_renderer_sprite(EcsEntity entity,
                                const SpriteCatalog* catalog,
                                std::string_view sprite_id,
                                bool has_pivot,
                                Vec2f pivot);
const SpriteCatalog* sprite_renderer_catalog(EcsEntity entity);

} // namespace kin
