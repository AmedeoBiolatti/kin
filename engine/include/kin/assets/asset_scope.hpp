#pragma once

#include <kin/assets/asset_manager.hpp>
#include <kin/assets/asset_server.hpp>

namespace kin {

// A scene/loading group's strong asset references. Releasing a scope makes its
// assets eligible for collection; other handles and shared dependencies survive.
class AssetScope {
public:
    template <typename T>
    AssetHandle<T> keep(AssetHandle<T> handle) {
        if (handle._storage) _assets.push_back(handle._storage);
        return handle;
    }
    template <typename T>
    AssetHandle<T> load(AssetManager& manager, std::string_view path) {
        return keep(manager.load_handle<T>(path));
    }
    template <typename T>
    AssetHandle<T> load_async(AssetServer& server, std::string_view path) {
        return keep(server.load_async<T>(path));
    }
    void clear() { _assets.clear(); }
    std::size_t size() const { return _assets.size(); }
private:
    std::vector<std::shared_ptr<IAssetStorage>> _assets;
};

} // namespace kin
