#include <algorithm>
#include <kin/ecs/assets2d.hpp>
#include <kin/renderer/renderer2d.hpp>
#include <limits>

namespace kin {
Texture Assets2D::data_texture(std::string_view path) {
    if (path.empty())
        return {};
    if (const auto found = _data_textures.find(path); found != _data_textures.end()) {
        found->second.last_access = ++_texture_clock;
        return found->second.texture;
    }
    const auto source = image(path);
    if (!source)
        return {};
    auto result = _renderer.create_texture(source->size, TextureFormat::Rgba8, source->rgba.data());
    if (result) {
        _data_textures.emplace(std::string(path), Made{result, file_time(path), false, ++_texture_clock});
        ++_version;
    }
    return result;
}
std::vector<AssetResidency> Assets2D::texture_residency() const {
    std::vector<AssetResidency> result;
    const auto append = [&](const auto& cache, std::string_view prefix) {
        for (const auto& [path, made] : cache) {
            if (!made.texture)
                continue;
            const auto references = static_cast<std::size_t>(made.texture.backend().use_count() - 1);
            AssetResidency row{.key = std::string(prefix) + path,
                               .path = path,
                               .bytes = texture_level_bytes(made.texture.format(), made.texture.size()),
                               .last_access = made.last_access,
                               .external_references = references,
                               .loaded = true,
                               .size_estimated = true};
            if (references) {
                const auto reason = _texture_reasons.find(made.texture.backend().get());
                row.retained_by = {reason == _texture_reasons.end()
                                       ? "live texture handles (components, catalogs or queued draws)"
                                       : reason->second,
                                   path};
            }
            result.push_back(std::move(row));
        }
    };
    append(_textures, "color:");
    append(_data_textures, "data:");
    std::ranges::sort(result, {}, &AssetResidency::key);
    return result;
}
AssetMemoryReport Assets2D::texture_memory_report() const {
    AssetMemoryReport report{.budget_bytes = _texture_budget};
    const auto add = [](u64 a, u64 b) {
        return b > std::numeric_limits<u64>::max() - a ? std::numeric_limits<u64>::max() : a + b;
    };
    for (const auto& row : texture_residency()) {
        report.resident_bytes = add(report.resident_bytes, row.bytes);
        if (!row.retained_by.empty())
            report.pinned_bytes = add(report.pinned_bytes, row.bytes);
    }
    report.over_budget = _texture_budget && report.resident_bytes > _texture_budget;
    return report;
}
void Assets2D::set_texture_budget(u64 bytes) {
    _texture_budget = bytes;
    trim_texture_memory();
}
AssetMemoryReport Assets2D::trim_texture_memory() {
    auto report = texture_memory_report();
    if (!_texture_budget || !report.over_budget)
        return report;
    auto rows = texture_residency();
    std::ranges::sort(rows, [](const auto& a, const auto& b) {
        return a.last_access == b.last_access ? a.key < b.key : a.last_access < b.last_access;
    });
    std::size_t evicted = 0;
    for (const auto& row : rows) {
        if (!report.over_budget)
            break;
        if (!row.retained_by.empty())
            continue;
        auto& cache = row.key.starts_with("data:") ? _data_textures : _textures;
        if (cache.erase(row.path))
            ++evicted;
        report = texture_memory_report();
    }
    report.evicted = evicted;
    return report;
}
} // namespace kin
