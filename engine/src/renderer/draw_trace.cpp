#include <kin/renderer/draw_trace.hpp>

#include <functional>

namespace kin {

std::string_view draw_kind_name(DrawKind kind) {
    switch (kind) {
    case DrawKind::Fill:
        return "fill";
    case DrawKind::Outline:
        return "outline";
    case DrawKind::Line:
        return "line";
    case DrawKind::Texture:
        return "texture";
    case DrawKind::Gradient:
        return "gradient";
    case DrawKind::Shader:
        return "shader";
    }
    return "unknown";
}

std::size_t DrawTrace::KeyHash::operator()(const EntityKeyView& key) const {
    std::size_t hash = std::hash<u64>{}(key.world);
    hash ^= std::hash<u64>{}(key.entity) + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
    hash ^= std::hash<std::string_view>{}(key.component) + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
    return hash;
}

u32 DrawTrace::scope_source(std::string_view label) {
    std::lock_guard lock{_mutex};
    if (const auto found = _scopes.find(std::string{label}); found != _scopes.end()) {
        return found->second;
    }
    _sources.push_back({.name = std::string{label}});
    const u32 id = static_cast<u32>(_sources.size());
    _scopes.emplace(std::string{label}, id);
    return id;
}

DrawSourceInfo DrawTrace::source_info(u32 id) const {
    std::lock_guard lock{_mutex};
    if (id == 0 || id > _sources.size()) {
        return {};
    }
    return _sources[id - 1];
}

u32 draw_scope_source(std::string_view label) {
    DrawTrace* trace = active_draw_trace();
    return trace ? trace->scope_source(label) : 0;
}

} // namespace kin
