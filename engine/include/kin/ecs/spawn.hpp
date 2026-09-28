#pragma once

#include <kin/ecs/world.hpp>

#include <functional>
#include <string_view>
#include <utility>

namespace kin {

class EntityBuilder {
public:
    explicit EntityBuilder(EcsEntity entity)
        : _entity(std::move(entity)) {
    }

    template <typename T>
    EntityBuilder& add() {
        _entity.add<T>();
        return *this;
    }

    template <typename T>
    EntityBuilder& set(const T& value) {
        _entity.set<T>(value);
        return *this;
    }

    template <typename T>
    EntityBuilder& set(T&& value) {
        _entity.set<T>(std::forward<T>(value));
        return *this;
    }

    template <typename T, typename... Args>
    EntityBuilder& emplace(Args&&... args) {
        _entity.emplace<T>(std::forward<Args>(args)...);
        return *this;
    }

    EcsEntity& entity() { return _entity; }
    const EcsEntity& entity() const { return _entity; }

private:
    EcsEntity _entity;
};

using SpawnRecipe = std::function<void(EntityBuilder&)>;

inline EcsEntity spawn(EcsWorld& world, std::string_view name, const SpawnRecipe& recipe) {
    EntityBuilder builder{world.entity(name)};
    recipe(builder);
    return builder.entity();
}

inline EcsEntity spawn(EcsWorld& world, const SpawnRecipe& recipe) {
    EntityBuilder builder{world.entity()};
    recipe(builder);
    return builder.entity();
}

} // namespace kin
