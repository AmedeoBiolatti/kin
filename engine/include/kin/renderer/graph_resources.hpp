#pragma once
#include <kin/renderer/data_buffer.hpp>
#include <kin/renderer/render_target.hpp>
#include <kin/renderer/resource_graph.hpp>

namespace kin {
class Renderer2D;
struct GraphImport {
    GraphResource resource = graph_no_slot;
    Texture texture;
    DataBuffer buffer;
};
// Reuses compatible SDL resource objects across disjoint logical lifetimes.
// Keep one instance per renderer/pipeline. Exported/persistent values have their
// own slots. Exports are current-frame handles, not immutable frame snapshots.
class GraphResources {
public:
    bool prepare(Renderer2D&, const ResourceGraphDesc&, const CompiledResourceGraph&,
                 std::span<const GraphImport> imports = {});
    const Texture& texture(GraphResource) const;
    const RenderTarget& target(GraphResource) const;
    const DataBuffer& buffer(GraphResource) const;
    const std::vector<std::string>& diagnostics() const { return _diagnostics; }
    void clear();

private:
    struct Slot {
        GraphResourceDesc desc;
        RenderTarget target;
        Texture texture;
        DataBuffer buffer;
    };
    std::vector<Slot> _slots;
    std::vector<GraphResourceLifetime> _lifetimes;
    std::vector<GraphImport> _imports;
    std::vector<std::string> _diagnostics;
    Renderer2D* _renderer = nullptr;
    i32 _color_space = -1;
    bool _hdr = false;
    u64 _plan_identity = 0;
};
u64 available_graph_features(const Renderer2D&);
} // namespace kin
