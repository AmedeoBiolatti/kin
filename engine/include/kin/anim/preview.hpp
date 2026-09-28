#pragma once

#include <kin/anim/player.hpp>
#include <kin/ecs/render.hpp>

namespace kin {

struct AnimationPreviewConfig {
    AnimationRegistry* registry = nullptr;
    const SpriteCatalog* sprite_catalog = nullptr;
    Bindings bindings;
};

class AnimationPreview {
public:
    explicit AnimationPreview(AnimationPreviewConfig config = {});

    EcsWorld& world() { return _world; }
    EcsEntity entity() const { return _entity; }
    AnimationPlayer* player();
    const AnimationPlayer* player() const;
    PropertyRegistry& properties() { return _properties; }

    bool set_animation(std::string_view name);
    bool push(std::string_view name, PushOptions options = {});
    void advance(f32 dt);
    void sample();
    AnimationPlayerSnapshot snapshot() const;

private:
    std::shared_ptr<const Animation> resolve(std::string_view name) const;

    EcsWorld _world;
    PropertyRegistry _properties;
    AnimationRegistry* _registry = nullptr;
    EcsEntity _entity;
};

} // namespace kin
