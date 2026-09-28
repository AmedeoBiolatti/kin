#pragma once

#include <kin/renderer/render_graph.hpp>

#include <string>

namespace kin {

enum class RenderProfileKind {
    TopDown2D,
    Isometric2D,
    Basic3D,
};

struct RenderProfile {
    RenderProfileKind kind = RenderProfileKind::TopDown2D;
    std::string name = "top_down_2d";
    RenderSortMode sort = RenderSortMode::LayerThenY;
    RenderProjection projection = RenderProjection::TopDown2D;
    bool culling_enabled = true;
    f32 cull_padding = 32.0f;

    void install(RenderGraph& graph) const;
    RenderView make_view(const Camera2D* camera = nullptr, Rectf viewport = {}) const;
};

RenderProfile top_down_2d_profile();
RenderProfile isometric_2d_profile();
RenderProfile basic_3d_profile();

} // namespace kin
