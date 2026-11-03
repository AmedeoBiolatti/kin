#include <algorithm>
#include <deque>
#include <kin/assets/asset_manager.hpp>
#include <map>
#include <set>

namespace kin {
namespace {
u64 plus(u64 a, u64 b) {
    return b > std::numeric_limits<u64>::max() - a ? std::numeric_limits<u64>::max() : a + b;
}
} // namespace
std::vector<AssetResidency> AssetManager::residency() const {
    std::map<std::string, std::vector<std::string>> chains;
    std::deque<std::string> pending;
    std::vector<std::string> roots;
    auto path_of = [&](const std::string& key) {
        const auto p = _loaded_paths.find(key);
        return p == _loaded_paths.end() ? key : p->second;
    };
    for (const auto& [key, storage] : _assets)
        if (storage.use_count() > 1)
            roots.push_back(key);
    std::ranges::sort(roots);
    for (const auto& root : roots) {
        chains.emplace(root, std::vector<std::string>{path_of(root)});
        pending.push_back(root);
    }
    while (!pending.empty()) {
        const auto key = pending.front();
        pending.pop_front();
        const auto found = _dependencies.find(key);
        if (found == _dependencies.end())
            continue;
        auto deps = found->second;
        std::ranges::sort(deps);
        for (const auto& child : deps)
            if (_assets.contains(child) && !chains.contains(child)) {
                auto chain = chains.at(key);
                chain.push_back(path_of(child));
                chains.emplace(child, std::move(chain));
                pending.push_back(child);
            }
    }
    std::vector<AssetResidency> rows;
    rows.reserve(_assets.size());
    for (const auto& [key, storage] : _assets) {
        AssetResidency row{.key = key,
                           .path = path_of(key),
                           .bytes = storage->resident_bytes,
                           .last_access = storage->last_access,
                           .external_references = static_cast<std::size_t>(storage.use_count() - 1),
                           .loaded = storage->loaded(),
                           .size_estimated = storage->size_estimated};
        if (chains.contains(key))
            row.retained_by = chains.at(key);
        if (const auto found = _dependencies.find(key); found != _dependencies.end())
            for (const auto& d : found->second)
                row.dependencies.push_back(path_of(d));
        std::ranges::sort(row.dependencies);
        rows.push_back(std::move(row));
    }
    std::ranges::sort(rows, {}, &AssetResidency::key);
    return rows;
}
AssetMemoryReport AssetManager::memory_report() const {
    AssetMemoryReport report{.budget_bytes = _memory_budget};
    for (const auto& row : residency()) {
        report.resident_bytes = plus(report.resident_bytes, row.bytes);
        if (!row.retained_by.empty())
            report.pinned_bytes = plus(report.pinned_bytes, row.bytes);
    }
    report.over_budget = _memory_budget && report.resident_bytes > _memory_budget;
    return report;
}
AssetMemoryReport AssetManager::trim_memory() {
    auto report = memory_report();
    std::size_t evicted = 0;
    if (!_memory_budget)
        return report;
    auto rows = residency();
    std::erase_if(rows, [](const auto& row) { return !row.retained_by.empty(); });
    std::ranges::sort(rows, [](const auto& a, const auto& b) {
        return a.last_access == b.last_access ? a.key < b.key : a.last_access < b.last_access;
    });
    for (const auto& candidate : rows) {
        if (!report.over_budget)
            break;
        if (!_assets.contains(candidate.key))
            continue;
        // Drop unowned reverse dependants too; a cached parent must never be
        // reused after one of its dependencies has been silently evicted.
        std::set<std::string> removed{candidate.key};
        bool changed = true;
        while (changed) {
            changed = false;
            for (const auto& [key, deps] : _dependencies)
                if (_assets.contains(key) && !removed.contains(key) &&
                    std::ranges::any_of(deps, [&](const auto& d) { return removed.contains(d); }))
                    changed |= removed.insert(key).second;
        }
        bool pinned = false;
        const auto current = residency();
        for (const auto& row : current)
            if (removed.contains(row.key) && !row.retained_by.empty())
                pinned = true;
        if (pinned)
            continue;
        for (const auto& key : removed) {
            if (_assets.erase(key))
                ++evicted;
            _dependencies.erase(key);
            _loaded_paths.erase(key);
        }
        report = memory_report();
    }
    // Keep path-level inventory coherent when several types share one path.
    for (auto& [path, metadata] : _metadata)
        if (metadata.status == AssetStatus::Loaded)
            metadata.status = AssetStatus::Discovered;
    for (const auto& [key, path] : _loaded_paths)
        if (_assets.at(key)->loaded() && _metadata.contains(path))
            _metadata[path].status = AssetStatus::Loaded;
    report.evicted = evicted;
    return report;
}
} // namespace kin
