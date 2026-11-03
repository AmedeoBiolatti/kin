#pragma once
#include <functional>
#include <kin/prefab/prefab_asset.hpp>
#include <memory>

namespace kin {
struct PrefabLease2D {
    u64 pool = 0;
    u32 slot = 0;
    u64 generation = 0;
    friend bool operator==(PrefabLease2D, PrefabLease2D) = default;
};
struct PrefabPoolOptions2D {
    std::size_t capacity = 32;
    std::function<void(EcsWorld&, const PrefabInstance&)> on_acquire;
    // Release game-owned subscriptions/resources here. Owned components are
    // removed afterward, invoking native OnRemove cleanup (including physics).
    std::function<void(EcsWorld&, const PrefabInstance&)> on_release;
};

// Fixed-capacity, main-thread pool. The world must outlive the pool. Source
// recipes are retained in memory; acquire/release perform no source file I/O.
// Keep leases across frames, and resolve() just before use; raw entity handles
// intentionally retain their identity and cannot themselves reject old leases.
class PrefabPool2D {
public:
    PrefabPool2D(EcsWorld& world, PrefabAsset prefab, PrefabPoolOptions2D options = {});
    ~PrefabPool2D();
    PrefabPool2D(const PrefabPool2D&) = delete;
    PrefabPool2D& operator=(const PrefabPool2D&) = delete;
    std::optional<PrefabLease2D> acquire(PrefabSpawn spawn = {});
    bool release(PrefabLease2D lease);
    const PrefabInstance* resolve(PrefabLease2D lease) const;
    std::size_t capacity() const { return _slots.size(); }
    std::size_t active() const;
    const std::string& error() const { return _error; }

private:
    struct Slot {
        PrefabInstance instance;
        u64 generation = 0;
        bool active = false;
    };
    void clear(Slot& slot);
    void shutdown();
    EcsWorld& _world;
    PrefabAsset _prefab;
    PrefabPoolOptions2D _options;
    std::vector<Slot> _slots;
    u64 _id = 0;
    bool _mutating = false;
    std::string _error;
};
} // namespace kin
