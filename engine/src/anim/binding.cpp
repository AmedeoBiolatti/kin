#include <kin/anim/binding.hpp>

#include <kin/ecs/render.hpp>

#include <utility>

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

    registry.add<SpriteRenderer>(world, "SpriteRenderer.offset", &SpriteRenderer::offset);
    registry.add<SpriteRenderer>(world, "SpriteRenderer.size", &SpriteRenderer::size);
    registry.add<SpriteRenderer>(world, "SpriteRenderer.pivot", &SpriteRenderer::pivot);
    registry.add<SpriteRenderer>(world, "SpriteRenderer.tint", &SpriteRenderer::tint);
    registry.add<SpriteRenderer>(world, "SpriteRenderer.rotation", &SpriteRenderer::rotation);

    registry.add<TextureRenderer>(world, "TextureRenderer.tint", &TextureRenderer::tint);
    registry.add<TextureRenderer>(world, "TextureRenderer.offset", &TextureRenderer::offset);
    registry.add<TextureRenderer>(world, "TextureRenderer.size", &TextureRenderer::size);

    registry.add<RectRenderer>(world, "RectRenderer.color", &RectRenderer::color);
    registry.add<RectRenderer>(world, "RectRenderer.size", &RectRenderer::size);
    registry.add<RectRenderer>(world, "RectRenderer.offset", &RectRenderer::offset);

    registry.add<LineRenderer>(world, "LineRenderer.color", &LineRenderer::color);
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
