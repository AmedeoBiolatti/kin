#include <kin/prefab/prefab_pool2d.hpp>

#include <kin/ecs/lifecycle.hpp>

#include <algorithm>
#include <atomic>
#include <set>
#include <stdexcept>
#include <string>

namespace kin {

namespace {

// Marks the pool busy while it mutates (component hooks and the release
// callback run then), and clears the mark however that ends.
class BusyScope {
public:
    explicit BusyScope(bool& busy) : _busy(busy) { _busy = true; }
    ~BusyScope() { _busy = false; }
    BusyScope(const BusyScope&) = delete;
    BusyScope& operator=(const BusyScope&) = delete;

private:
    bool& _busy;
};

} // namespace

PrefabPool2D::PrefabPool2D(EcsWorld& world, PrefabAsset prefab, PrefabPoolOptions2D options)
    : _world(world), _prefab(std::move(prefab)), _options(std::move(options)) {
    static std::atomic<u64> next{1};
    _id = next.fetch_add(1);
    if (_options.capacity == 0 || _options.capacity > max_capacity)
        throw std::invalid_argument("pool capacity must be 1.." + std::to_string(max_capacity));
    try {
        _slots.reserve(_options.capacity);
        for (std::size_t i = 0; i < _options.capacity; ++i) {
            auto instance = instantiate_prefab(_world, _prefab, {}, _world.components());
            if (!instance.ok())
                throw std::runtime_error(instance.diagnostics.empty() ? "prefab prewarm failed"
                                                                      : instance.diagnostics.front());
            _slots.push_back({std::move(instance)});
            clear(_slots.back());
        }
    } catch (...) {
        shutdown();
        throw;
    }
}
PrefabPool2D::~PrefabPool2D() {
    shutdown();
}
std::size_t PrefabPool2D::active() const {
    return std::ranges::count(_slots, true, &Slot::active);
}
const PrefabInstance* PrefabPool2D::resolve(PrefabLease2D lease) const {
    if (lease.pool != _id || lease.slot >= _slots.size())
        return nullptr;
    const auto& slot = _slots[lease.slot];
    return slot.active && slot.generation == lease.generation && slot.instance.root ? &slot.instance : nullptr;
}
void PrefabPool2D::clear(Slot& slot) {
    std::set<EcsId> keep;
    for (auto entity : slot.instance.entities)
        keep.insert(entity.id());
    std::vector<EcsEntity> extra;
    for (auto entity : slot.instance.entities)
        if (entity)
            entity.raw().children([&](flecs::entity child) {
                if (!keep.contains(child.id()))
                    extra.emplace_back(child);
            });
    // Authored children may have been reparented under runtime-only nodes.
    // Detach them before deleting those subtrees so their IDs survive reuse.
    for (auto entity : slot.instance.entities)
        if (entity) {
            ecs_set_name(_world.raw().c_ptr(), entity.id(), nullptr);
            _world.entities().detach_parent(entity);
        }
    for (auto entity : extra)
        if (entity)
            _world.entities().destroy(entity);
    for (auto it = slot.instance.entities.rbegin(); it != slot.instance.entities.rend(); ++it)
        if (*it) {
            ecs_clear(_world.raw().c_ptr(), it->id());
            it->disable();
            _world.entities().register_existing(*it);
        }
}
std::optional<PrefabLease2D> PrefabPool2D::acquire(PrefabSpawn spawn) {
    if (_mutating) {
        _error = "reentrant pool mutation";
        return std::nullopt;
    }
    const BusyScope busy{_mutating};
    for (std::size_t i = 0; i < _slots.size(); ++i) {
        auto& slot = _slots[i];
        if (slot.active)
            continue;
        ++slot.generation;
        if (spawn.authored_id_prefix.empty())
            spawn.authored_id_prefix = "pool_" + std::to_string(_id) + "_" + std::to_string(i);
        if (!restore_prefab_instance(_world, _prefab, slot.instance, spawn)) {
            _error = slot.instance.diagnostics.empty() ? "pooled entity was removed or could not be reset"
                                                       : slot.instance.diagnostics.front();
            clear(slot);
            return std::nullopt;
        }
        for (auto entity : slot.instance.entities)
            entity.set(EntityLifetime{slot.generation});
        slot.active = true;
        _error.clear();
        return PrefabLease2D{_id, static_cast<u32>(i), slot.generation};
    }
    _error = "prefab pool exhausted";
    return std::nullopt;
}
bool PrefabPool2D::release(PrefabLease2D lease) {
    if (_mutating || !resolve(lease)) {
        _error = "invalid, stale or reentrant pool release";
        return false;
    }
    const BusyScope busy{_mutating};
    auto& slot = _slots[lease.slot];
    slot.active = false;
    bool clean = true;
    try {
        if (_options.on_release)
            _options.on_release(_world, slot.instance);
    } catch (...) {
        clean = false;
    }
    clear(slot);
    _error = clean ? "" : "pool release callback failed";
    return clean;
}
void PrefabPool2D::shutdown() {
    _mutating = true;
    for (auto& slot : _slots) {
        const bool active = slot.active;
        slot.active = false;
        if (active && _options.on_release)
            try {
                _options.on_release(_world, slot.instance);
            } catch (...) {
            }
        for (auto it = slot.instance.entities.rbegin(); it != slot.instance.entities.rend(); ++it)
            if (*it)
                _world.entities().destroy(*it);
    }
    _slots.clear();
}
} // namespace kin
