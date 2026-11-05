#include <kin/prefab/prefab_pool2d.hpp>

#include <kin/ecs/render.hpp>
#include <kin/scene/scene_asset.hpp>

#include <cassert>
#include <stdexcept>

using namespace kin;

namespace {

PrefabAsset two_entity_prefab(kin::EcsWorld& world) {
    auto parsed = parse_prefab_asset(R"({"schema":"kin.prefab/1","id":"bullet","root":"root","entities":[
      {"id":"root","name":"Bullet","components":{"Transform2D":{}}},
      {"id":"tail","parent":"root","components":{"Transform2D":{"pos":{"x":-2,"y":0}}}}]})",
                                     world.components());
    assert(parsed.ok());
    return *parsed.prefab;
}

bool throws_invalid_capacity(kin::EcsWorld& world, const PrefabAsset& prefab, std::size_t capacity) {
    try {
        PrefabPool2D pool{world, prefab, {.capacity = capacity}};
    } catch (const std::invalid_argument&) {
        return true;
    }
    return false;
}

void test_capacity_limits() {
    kin::EcsWorld world;
    register_builtin_editor_components(world);
    const PrefabAsset prefab = two_entity_prefab(world);
    assert(throws_invalid_capacity(world, prefab, 0));
    assert(throws_invalid_capacity(world, prefab, PrefabPool2D::max_capacity + 1));
    PrefabPool2D pool{world, prefab, {.capacity = 3}};
    assert(pool.capacity() == 3 && pool.active() == 0);
}

// A slot that cannot be restored reports the entity registry's reason, and the
// slot stays usable afterwards.
void test_restore_failure_reports_its_reason() {
    kin::EcsWorld world;
    register_builtin_editor_components(world);
    PrefabPool2D pool{world, two_entity_prefab(world), {.capacity = 1}};
    assert(world.entities().create({.name = "taken", .authored_id = "taken/root"}));
    assert(!pool.acquire({.authored_id_prefix = "taken"}));
    assert(pool.error() == "duplicate authored entity id 'taken/root'");
    const auto lease = pool.acquire({.pos = {4, 5}});
    assert(lease && pool.error().empty());
    const PrefabInstance* instance = pool.resolve(*lease);
    assert(instance && instance->root.get<Transform2D>()->pos.x == 4);
    assert(instance->by_authored_id.at("tail").parent().id() == instance->root.id());
}

} // namespace

int main() {
    test_capacity_limits();
    test_restore_failure_reports_its_reason();
}
