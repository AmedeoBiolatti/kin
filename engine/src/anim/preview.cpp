#include <kin/anim/preview.hpp>

namespace kin {

AnimationPreview::AnimationPreview(AnimationPreviewConfig config)
    : _registry(config.registry) {
    _world.component<Transform2D>("Transform2D");
    _world.component<SpriteRenderer>("SpriteRenderer");
    _world.component<TextureRenderer>("TextureRenderer");
    _world.component<RectRenderer>("RectRenderer");
    _world.component<LineRenderer>("LineRenderer");
    _world.component<AnimationPlayer>("AnimationPlayer");
    register_builtin_properties(_properties, _world);

    AnimationPlayer player{
        .registry = _registry,
        .properties = &_properties,
        .sprite_catalog = config.sprite_catalog,
        .bindings = std::move(config.bindings),
    };
    _entity = _world.entity("preview")
                  .set(Transform2D{})
                  .set(SpriteRenderer{})
                  .set(std::move(player));
}

AnimationPlayer* AnimationPreview::player() {
    return _entity.get_mut<AnimationPlayer>();
}

const AnimationPlayer* AnimationPreview::player() const {
    return _entity.get<AnimationPlayer>();
}

std::shared_ptr<const Animation> AnimationPreview::resolve(std::string_view name) const {
    const AnimationPlayer* current = player();
    if (!_registry || !current) {
        return nullptr;
    }
    std::string resolved;
    std::string error;
    if (!substitute(name, current->bindings, resolved, error)) {
        return nullptr;
    }
    return _registry->resolve(resolved, current->bindings);
}

bool AnimationPreview::set_animation(std::string_view name) {
    AnimationPlayer* current = player();
    if (!current || !resolve(name)) {
        return false;
    }
    set_base(*current, name);
    return !current->layers.empty();
}

bool AnimationPreview::push(std::string_view name, PushOptions options) {
    AnimationPlayer* current = player();
    if (!current || !resolve(name)) {
        return false;
    }
    const std::size_t before = current->layers.size();
    kin::push(*current, name, std::move(options));
    return current->layers.size() > before;
}

void AnimationPreview::advance(f32 dt) {
    advance_animation_players(_world, dt);
}

void AnimationPreview::sample() {
    sample_animation_players(_world);
}

AnimationPlayerSnapshot AnimationPreview::snapshot() const {
    const AnimationPlayer* current = player();
    return current ? kin::snapshot(*current) : AnimationPlayerSnapshot{};
}

} // namespace kin
