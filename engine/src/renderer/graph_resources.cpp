#include <algorithm>
#include <kin/renderer/graph_resources.hpp>
#include <kin/renderer/renderer2d.hpp>
#include <stdexcept>

namespace kin {
u64 available_graph_features(const Renderer2D& renderer) {
    const auto caps = renderer.capabilities();
    return (caps.render_targets ? graph_feature::raster : 0) | (caps.compute ? graph_feature::compute : 0) |
           (caps.data_buffers ? graph_feature::buffers : 0) | (caps.rendering_3d ? graph_feature::rendering3d : 0) |
           (caps.materials_2d ? graph_feature::shaders : 0);
}
bool GraphResources::prepare(Renderer2D& renderer, const ResourceGraphDesc& desc, const CompiledResourceGraph& plan,
                             std::span<const GraphImport> imports) {
    _diagnostics.clear();
    if (!plan) {
        _diagnostics = plan.diagnostics;
        return false;
    }
    if (_renderer && _renderer != &renderer) {
        _diagnostics = {"graph resources belong to another renderer"};
        return false;
    }
    if (desc != plan.source) {
        _diagnostics = {"graph definition does not match the plan"};
        return false;
    }
    for (std::size_t i = 0; i < imports.size(); ++i) {
        const auto& value = imports[i];
        if (value.resource >= desc.resources.size() || !desc.resources[value.resource].imported) {
            _diagnostics = {"invalid or duplicate graph import"};
            return false;
        }
        for (std::size_t j = 0; j < i; ++j)
            if (imports[j].resource == value.resource) {
                _diagnostics = {"duplicate graph import"};
                return false;
            }
    }
    for (u32 id = 0; id < desc.resources.size(); ++id) {
        const auto& r = desc.resources[id];
        if (!r.imported && r.kind == GraphResourceKind::ColorTarget && plan.lifetimes[id].first != graph_no_slot &&
            r.hdr != renderer.hdr()) {
            _diagnostics = {r.name + ": colour target HDR declaration does not match renderer working space"};
            return false;
        }
        if (!r.imported || r.kind == GraphResourceKind::Token || plan.lifetimes[id].first == graph_no_slot)
            continue;
        const auto value = std::find_if(imports.begin(), imports.end(),
                                        [&](const GraphImport& imported) { return imported.resource == id; });
        const bool valid = value != imports.end() && (r.kind == GraphResourceKind::Buffer
                                                          ? value->buffer.valid() && value->buffer.size() == r.bytes
                                                          : value->texture && value->texture.size() == r.size &&
                                                                value->texture.format() == r.format);
        if (!valid) {
            _diagnostics = {r.name + ": missing or incompatible imported resource"};
            return false;
        }
    }
    if (plan.identity && _plan_identity == plan.identity && _color_space == static_cast<i32>(renderer.color_space()) &&
        _hdr == renderer.hdr()) {
        // Validated imports can change without allocating CPU or GPU storage.
        for (auto& value : _imports)
            value = {};
        for (const auto& value : imports)
            _imports[value.resource] = value;
        return true;
    }
    try {
        std::vector<GraphImport> resolved(desc.resources.size());
        for (const auto& value : imports)
            resolved[value.resource] = value;
        // Stage all allocations before publishing bindings or invoking any pass.
        std::vector<Slot> slots;
        slots.reserve(plan.slots.size());
        for (u32 i = 0; i < plan.slots.size(); ++i) {
            const auto& r = plan.slots[i];
            if (i < _slots.size() && _slots[i].desc == r &&
                (r.kind != GraphResourceKind::ColorTarget ||
                 (_color_space == static_cast<i32>(renderer.color_space()) && _hdr == renderer.hdr()))) {
                slots.push_back(_slots[i]);
                continue;
            }
            Slot slot{.desc = r};
            switch (r.kind) {
            case GraphResourceKind::ColorTarget:
                slot.target = renderer.create_render_target(r.size, ScaleMode::Nearest);
                slot.texture = slot.target.texture();
                break;
            case GraphResourceKind::StorageTexture:
                slot.texture = renderer.create_storage_texture(r.size, r.format);
                break;
            case GraphResourceKind::Buffer:
                slot.buffer = renderer.create_data_buffer(r.bytes);
                break;
            case GraphResourceKind::Token:
                break;
            }
            if ((r.kind == GraphResourceKind::Buffer && !slot.buffer.valid()) ||
                (r.kind != GraphResourceKind::Buffer && r.kind != GraphResourceKind::Token && !slot.texture)) {
                _diagnostics = {r.name + ": graph allocation failed or operation unsupported"};
                return false;
            }
            slots.push_back(std::move(slot));
        }
        _slots = std::move(slots);
        _imports = std::move(resolved);
        _lifetimes = plan.lifetimes;
        _renderer = &renderer;
        _color_space = static_cast<i32>(renderer.color_space());
        _hdr = renderer.hdr();
        _plan_identity = plan.identity;
        return true;
    } catch (const std::exception& error) {
        _diagnostics = {error.what()};
        return false;
    }
}
const Texture& GraphResources::texture(GraphResource id) const {
    if (id >= _lifetimes.size() || _lifetimes[id].first == graph_no_slot)
        throw std::out_of_range("unused graph texture");
    if (_lifetimes[id].slot == graph_no_slot) {
        if (!_imports[id].texture)
            throw std::invalid_argument("graph resource is not a texture");
        return _imports[id].texture;
    }
    const auto& slot = _slots[_lifetimes[id].slot];
    if (!slot.texture)
        throw std::invalid_argument("graph resource is not a texture");
    return slot.texture;
}
const RenderTarget& GraphResources::target(GraphResource id) const {
    if (id >= _lifetimes.size() || _lifetimes[id].slot == graph_no_slot)
        throw std::invalid_argument("graph resource is not a writable target");
    const auto& slot = _slots[_lifetimes[id].slot];
    if (!slot.target)
        throw std::invalid_argument("graph resource is not a writable target");
    return slot.target;
}
const DataBuffer& GraphResources::buffer(GraphResource id) const {
    if (id >= _lifetimes.size() || _lifetimes[id].first == graph_no_slot)
        throw std::out_of_range("unused graph buffer");
    if (_lifetimes[id].slot == graph_no_slot) {
        if (!_imports[id].buffer.valid())
            throw std::invalid_argument("graph resource is not a buffer");
        return _imports[id].buffer;
    }
    const auto& slot = _slots[_lifetimes[id].slot];
    if (!slot.buffer.valid())
        throw std::invalid_argument("graph resource is not a buffer");
    return slot.buffer;
}
void GraphResources::clear() {
    _slots.clear();
    _lifetimes.clear();
    _imports.clear();
    _diagnostics.clear();
    _renderer = nullptr;
    _color_space = -1;
    _hdr = false;
    _plan_identity = 0;
}
} // namespace kin
