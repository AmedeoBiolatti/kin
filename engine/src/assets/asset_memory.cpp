#include <kin/assets/asset_manager.hpp>

#include <algorithm>
#include <deque>
#include <limits>
#include <unordered_set>

namespace kin {

namespace {

u64 saturating_add(u64 a, u64 b) {
    return b > std::numeric_limits<u64>::max() - a ? std::numeric_limits<u64>::max() : a + b;
}

} // namespace

AssetMemoryReport summarize_residency(std::span<const AssetResidency> rows, u64 budget_bytes) {
    AssetMemoryReport report{.budget_bytes = budget_bytes};
    for (const AssetResidency& row : rows) {
        report.resident_bytes = saturating_add(report.resident_bytes, row.bytes);
        if (!row.retained_by.empty()) {
            report.pinned_bytes = saturating_add(report.pinned_bytes, row.bytes);
        }
    }
    report.over_budget = budget_bytes && report.resident_bytes > budget_bytes;
    return report;
}

bool least_recent_first(const AssetResidency& a, const AssetResidency& b) {
    return a.last_access == b.last_access ? a.key < b.key : a.last_access < b.last_access;
}

AssetManager::Retainers AssetManager::retainers() const {
    std::vector<std::string> owners;
    for (const auto& [key, storage] : _assets) {
        // Handles, shared() views and pending loads all share the storage.
        if (storage.use_count() > 1) {
            owners.push_back(key);
        }
    }
    std::ranges::sort(owners);
    Retainers retained;
    std::deque<std::string> pending;
    for (std::string& owner : owners) {
        retained.emplace(owner, std::string{});
        pending.push_back(std::move(owner));
    }
    while (!pending.empty()) {
        const std::string key = std::move(pending.front());
        pending.pop_front();
        const auto edges = _dependencies.find(key);
        if (edges == _dependencies.end()) {
            continue;
        }
        std::vector<std::string> children = edges->second;
        std::ranges::sort(children);
        for (std::string& child : children) {
            if (_assets.contains(child) && retained.emplace(child, key).second) {
                pending.push_back(std::move(child));
            }
        }
    }
    return retained;
}

std::vector<AssetResidency> AssetManager::residency() const {
    const Retainers retained = retainers();
    const auto path_of = [&](const std::string& key) {
        const auto path = _loaded_paths.find(key);
        return path == _loaded_paths.end() ? key : path->second;
    };
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
        if (retained.contains(key)) {
            // Up the retention chain to its owner, then root first.
            for (std::string at = key; !at.empty(); at = retained.at(at)) {
                row.retained_by.push_back(path_of(at));
            }
            std::ranges::reverse(row.retained_by);
        }
        if (const auto edges = _dependencies.find(key); edges != _dependencies.end()) {
            for (const std::string& dependency : edges->second) {
                row.dependencies.push_back(path_of(dependency));
            }
        }
        std::ranges::sort(row.dependencies);
        rows.push_back(std::move(row));
    }
    std::ranges::sort(rows, {}, &AssetResidency::key);
    return rows;
}

AssetMemoryReport AssetManager::memory_totals(const Retainers& retained) const {
    AssetMemoryReport report{.budget_bytes = _memory_budget};
    for (const auto& [key, storage] : _assets) {
        report.resident_bytes = saturating_add(report.resident_bytes, storage->resident_bytes);
        if (retained.contains(key)) {
            report.pinned_bytes = saturating_add(report.pinned_bytes, storage->resident_bytes);
        }
    }
    report.over_budget = _memory_budget && report.resident_bytes > _memory_budget;
    return report;
}

AssetMemoryReport AssetManager::memory_report() const {
    return memory_totals(retainers());
}

bool AssetManager::over_memory_budget() const {
    if (!_memory_budget) {
        return false;
    }
    u64 resident = 0;
    for (const auto& [key, storage] : _assets) {
        resident = saturating_add(resident, storage->resident_bytes);
    }
    return resident > _memory_budget;
}

AssetMemoryReport AssetManager::trim_memory() {
    const Retainers retained = retainers();
    AssetMemoryReport report = memory_totals(retained);
    if (!report.over_budget) {
        return report;
    }
    std::vector<AssetResidency> candidates;
    for (const auto& [key, storage] : _assets) {
        if (!retained.contains(key)) {
            candidates.push_back({.key = key, .bytes = storage->resident_bytes, .last_access = storage->last_access});
        }
    }
    std::ranges::sort(candidates, least_recent_first);
    // Evicting an entry evicts what depends on it too: a cached parent must
    // never be reused after one of its dependencies was silently dropped. A
    // dependant of an unretained entry is unretained itself (retention follows
    // edges to dependencies), so no live owner loses anything.
    std::unordered_map<std::string, std::vector<std::string>> dependants;
    for (const auto& [key, edges] : _dependencies) {
        if (_assets.contains(key)) {
            for (const std::string& dependency : edges) {
                dependants[dependency].push_back(key);
            }
        }
    }
    u64 resident = report.resident_bytes;
    std::unordered_set<std::string> evicting;
    std::vector<std::string> evicted;
    for (const AssetResidency& candidate : candidates) {
        if (resident <= _memory_budget) {
            break;
        }
        std::vector<std::string> pending{candidate.key};
        while (!pending.empty()) {
            std::string key = std::move(pending.back());
            pending.pop_back();
            if (!evicting.insert(key).second) {
                continue;
            }
            resident -= std::min(resident, _assets.at(key)->resident_bytes);
            if (const auto found = dependants.find(key); found != dependants.end()) {
                pending.insert(pending.end(), found->second.begin(), found->second.end());
            }
            evicted.push_back(std::move(key));
        }
    }
    erase_entries(evicted);
    report = memory_report();
    report.evicted = evicted.size();
    return report;
}

} // namespace kin
