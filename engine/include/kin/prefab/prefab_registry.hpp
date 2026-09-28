#pragma once

#include <kin/core/types.hpp>
#include <kin/ecs/spawn.hpp>
#include <kin/ecs/world.hpp>

#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace kin {

struct PrefabSpawn {
    Vec2f pos{};
    std::string name;
    EcsEntity parent{};
    std::string authored_id_prefix;
};

class PrefabRegistry {
public:
    using Factory = std::function<EcsEntity(EcsWorld&, const PrefabSpawn&)>;
    using Recipe = std::function<void(EntityBuilder&, const PrefabSpawn&)>;

    bool add(std::string name, Factory factory) {
        if (name.empty() || !factory) {
            return false;
        }
        _factories[std::move(name)] = std::move(factory);
        return true;
    }

    bool add_recipe(std::string name, Recipe recipe) {
        if (!recipe) {
            return false;
        }
        return add(std::move(name), [recipe = std::move(recipe)](EcsWorld& world, const PrefabSpawn& spawn) {
            EntityBuilder builder{spawn.name.empty() ? world.entity() : world.entity(spawn.name)};
            recipe(builder, spawn);
            return builder.entity();
        });
    }

    bool has(std::string_view name) const {
        return _factories.find(std::string{name}) != _factories.end();
    }

    bool remove(std::string_view name) {
        return _factories.erase(std::string{name}) > 0;
    }

    void clear() {
        _factories.clear();
    }

    std::vector<std::string> names() const {
        std::vector<std::string> result;
        result.reserve(_factories.size());
        for (const auto& [name, factory] : _factories) {
            result.push_back(name);
        }
        return result;
    }

    EcsEntity spawn(EcsWorld& world, std::string_view name, PrefabSpawn spawn = {}) const {
        const auto found = _factories.find(std::string{name});
        if (found == _factories.end()) {
            return {};
        }

        EcsEntity entity = found->second(world, spawn);
        if (entity && spawn.parent) {
            entity.child_of(spawn.parent);
        }
        return entity;
    }

private:
    std::unordered_map<std::string, Factory> _factories;
};

} // namespace kin
