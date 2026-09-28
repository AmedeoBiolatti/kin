#pragma once

#include <memory>

namespace kin {

class IAssetStorage {
public:
    virtual ~IAssetStorage() = default;
    virtual bool loaded() const = 0;
    virtual void clear_loaded() = 0;
};

template<typename T>
struct AssetStorage final : IAssetStorage {
    T data{};
    bool is_loaded = false;

    bool loaded() const override { return is_loaded; }
    void clear_loaded() override { is_loaded = false; }
};

template<typename T>
class AssetHandle {
public:
    AssetHandle() = default;
    explicit AssetHandle(std::shared_ptr<AssetStorage<T>> storage)
        : _storage(std::move(storage)) {
    }

    bool valid() const { return _storage && _storage->is_loaded; }
    explicit operator bool() const { return valid(); }

    const T& get() const { return _storage->data; }
    const T& operator*() const { return get(); }
    const T* operator->() const { return &get(); }

    std::shared_ptr<const T> shared() const {
        return valid() ? std::shared_ptr<const T>{_storage, &_storage->data} : std::shared_ptr<const T>{};
    }

private:
    std::shared_ptr<AssetStorage<T>> _storage;
};

} // namespace kin
