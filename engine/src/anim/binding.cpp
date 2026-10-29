#include <kin/anim/binding.hpp>

#include <kin/ecs/render.hpp>

#include <type_traits>
#include <utility>
#include <variant>

namespace kin {

void PropertyRegistry::add(std::string key, PropertyAccessor accessor) {
    _props[std::move(key)] = std::move(accessor);
}

const PropertyAccessor* PropertyRegistry::find(std::string_view key) const {
    const auto found = _props.find(std::string{key});
    return found == _props.end() ? nullptr : &found->second;
}

void register_builtin_properties(PropertyRegistry& registry, EcsWorld& world) {
    registry.add<Transform2D>(world, "Transform2D.pos", &Transform2D::pos);
    registry.add<Transform2D>(world, "Transform2D.rotation", &Transform2D::rotation);
    registry.add<Transform2D>(world, "Transform2D.scale", &Transform2D::scale);

    registry.add<SpriteRenderer>(world, "SpriteRenderer.offset", &SpriteRenderer::offset);
    registry.add<SpriteRenderer>(world, "SpriteRenderer.size", &SpriteRenderer::size);
    registry.add<SpriteRenderer>(world, "SpriteRenderer.pivot", &SpriteRenderer::pivot);
    registry.add<SpriteRenderer>(world, "SpriteRenderer.tint", &SpriteRenderer::tint);
    registry.add<SpriteRenderer>(world, "SpriteRenderer.rotation", &SpriteRenderer::rotation);
    registry.add<SpriteRenderer>(world, "SpriteRenderer.flip_x", &SpriteRenderer::flip_x);
    registry.add<SpriteRenderer>(world, "SpriteRenderer.flip_y", &SpriteRenderer::flip_y);

    registry.add<TextureRenderer>(world, "TextureRenderer.tint", &TextureRenderer::tint);
    registry.add<TextureRenderer>(world, "TextureRenderer.offset", &TextureRenderer::offset);
    registry.add<TextureRenderer>(world, "TextureRenderer.size", &TextureRenderer::size);
    registry.add<TextureRenderer>(world, "TextureRenderer.flip_x", &TextureRenderer::flip_x);
    registry.add<TextureRenderer>(world, "TextureRenderer.flip_y", &TextureRenderer::flip_y);

    registry.add<RectRenderer>(world, "RectRenderer.color", &RectRenderer::color);
    registry.add<RectRenderer>(world, "RectRenderer.size", &RectRenderer::size);
    registry.add<RectRenderer>(world, "RectRenderer.offset", &RectRenderer::offset);

    registry.add<LineRenderer>(world, "LineRenderer.color", &LineRenderer::color);
}

std::optional<AnimValueKind> anim_value_kind_of(ComponentFieldKind kind) {
    switch (kind) {
    case ComponentFieldKind::F32: return AnimValueKind::Float;
    case ComponentFieldKind::Vec2f: return AnimValueKind::Vec2;
    case ComponentFieldKind::Color: return AnimValueKind::Color;
    case ComponentFieldKind::I32: return AnimValueKind::Int;
    case ComponentFieldKind::Bool: return AnimValueKind::Bool;
    case ComponentFieldKind::String: return AnimValueKind::String;
    default: return std::nullopt;
    }
}

void register_described_properties(PropertyRegistry& registry, const EcsWorld& world) {
    const EcsComponentRegistry& components = world.components();
    for (const ComponentDescriptor& component : components.descriptors()) {
        if (component.kind != ComponentKind::Native) {
            continue;
        }
        for (const ComponentFieldDescriptor& field : component.fields) {
            const std::optional<AnimValueKind> kind = anim_value_kind_of(field.kind);
            std::optional<ComponentFieldAccess> access = components.field_access(component.name, field.name);
            if (!kind || field.readonly || field.hidden || !access) {
                continue;
            }
            registry.add(component.name + "." + field.name,
                         PropertyAccessor{
                             .component = access->component,
                             .kind = *kind,
                             .get = [read = access->read](const void* data) -> AnimValue {
                                 return std::visit(
                                     [](const auto& value) -> AnimValue {
                                         using V = std::decay_t<decltype(value)>;
                                         if constexpr (std::is_constructible_v<AnimValue, V> &&
                                                       (std::is_same_v<V, f32> || std::is_same_v<V, Vec2f> || std::is_same_v<V, Color> ||
                                                        std::is_same_v<V, i32> || std::is_same_v<V, bool> || std::is_same_v<V, std::string>)) {
                                             return value;
                                         } else {
                                             return 0.0f;
                                         }
                                     },
                                     read(data));
                             },
                             .set = [write = access->write](void* data, const AnimValue& value) {
                                 std::visit([&](const auto& typed) { write(data, ComponentFieldValue{typed}); }, value);
                             },
                         });
        }
    }
}

bool set_sprite_renderer_sprite(EcsEntity entity,
                                const SpriteCatalog* catalog,
                                std::string_view sprite_id,
                                bool has_pivot,
                                Vec2f pivot) {
    auto* renderer = entity.get_mut<SpriteRenderer>();
    if (!renderer) {
        return false;
    }
    const SpriteCatalog* resolved_catalog = catalog ? catalog : renderer->sprite.catalog;
    renderer->sprite = SpriteRef{.catalog = resolved_catalog, .id = std::string{sprite_id}};
    if (has_pivot) {
        renderer->pivot = pivot;
    }
    return true;
}

const SpriteCatalog* sprite_renderer_catalog(EcsEntity entity) {
    const auto* renderer = entity.get<SpriteRenderer>();
    return renderer ? renderer->sprite.catalog : nullptr;
}

} // namespace kin
