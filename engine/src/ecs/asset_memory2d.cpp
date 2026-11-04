#include <kin/ecs/assets2d.hpp>

#include <kin/renderer/renderer2d.hpp>

#include <algorithm>

namespace kin {

Texture Assets2D::data_texture(std::string_view path) {
    if (path.empty()) {
        return {};
    }
    if (const auto found = _data_textures.find(path); found != _data_textures.end()) {
        found->second.last_access = ++_texture_clock;
        return found->second.texture;
    }
    const auto source = image(path);
    if (!source) {
        return {};
    }
    auto result = _renderer.create_texture(source->size, TextureFormat::Rgba8, source->rgba.data());
    if (result) {
        _data_textures.emplace(std::string(path), Made{result, file_time(path), false, ++_texture_clock});
        ++_version;
    }
    return result;
}

AssetResidency Assets2D::texture_row(std::string_view kind, const std::string& path, const Made& made) const {
    const auto references = static_cast<std::size_t>(made.texture.backend().use_count() - 1);
    AssetResidency row{.key = std::string(kind) + path,
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
    return row;
}

std::vector<AssetResidency> Assets2D::texture_residency() const {
    std::vector<AssetResidency> rows;
    for (const auto& [cache, kind] : {std::pair{&_textures, "color:"}, std::pair{&_data_textures, "data:"}}) {
        for (const auto& [path, made] : *cache) {
            if (made.texture) {
                rows.push_back(texture_row(kind, path, made));
            }
        }
    }
    std::ranges::sort(rows, {}, &AssetResidency::key);
    return rows;
}

AssetMemoryReport Assets2D::texture_memory_report() const {
    return summarize_residency(texture_residency(), _texture_budget);
}

bool Assets2D::over_texture_budget() const {
    if (!_texture_budget) {
        return false;
    }
    u64 resident = 0;
    for (const TextureCache* cache : {&_textures, &_data_textures}) {
        for (const auto& [path, made] : *cache) {
            if (made.texture) {
                resident += texture_level_bytes(made.texture.format(), made.texture.size());
            }
        }
    }
    return resident > _texture_budget;
}

void Assets2D::set_texture_budget(u64 bytes) {
    _texture_budget = bytes;
    trim_texture_memory();
}

AssetMemoryReport Assets2D::trim_texture_memory() {
    AssetMemoryReport report = texture_memory_report();
    if (!report.over_budget) {
        return report;
    }
    struct Candidate {
        AssetResidency row;
        TextureCache* cache = nullptr;
    };
    std::vector<Candidate> candidates;
    for (const auto& [cache, kind] : {std::pair{&_textures, "color:"}, std::pair{&_data_textures, "data:"}}) {
        for (const auto& [path, made] : *cache) {
            if (made.texture && made.texture.backend().use_count() == 1) {
                candidates.push_back({texture_row(kind, path, made), cache});
            }
        }
    }
    std::ranges::sort(candidates,
                      [](const Candidate& a, const Candidate& b) { return least_recent_first(a.row, b.row); });
    u64 resident = report.resident_bytes;
    std::size_t evicted = 0;
    for (const Candidate& candidate : candidates) {
        if (resident <= _texture_budget) {
            break;
        }
        candidate.cache->erase(candidate.row.path);
        resident -= std::min(resident, candidate.row.bytes);
        ++evicted;
    }
    report = texture_memory_report();
    report.evicted = evicted;
    return report;
}

} // namespace kin
