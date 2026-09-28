#include <kin/renderer/render_profile.hpp>

namespace kin {

void RenderProfile::install(RenderGraph& graph) const {
    graph.add_default_passes();
}

RenderView RenderProfile::make_view(const Camera2D* camera, Rectf viewport) const {
    RenderView view{
        .projection = projection,
        .viewport = viewport,
        .camera = camera,
        .cull_padding = cull_padding,
        .culling_enabled = culling_enabled,
    };
    if (camera) {
        view.cull_rect = camera->visible_rect(cull_padding);
    }
    return view;
}

RenderProfile top_down_2d_profile() {
    return {
        .kind = RenderProfileKind::TopDown2D,
        .name = "top_down_2d",
        .sort = RenderSortMode::LayerThenY,
        .projection = RenderProjection::TopDown2D,
        .culling_enabled = true,
        .cull_padding = 32.0f,
    };
}

RenderProfile isometric_2d_profile() {
    return {
        .kind = RenderProfileKind::Isometric2D,
        .name = "isometric_2d",
        .sort = RenderSortMode::LayerThenY,
        .projection = RenderProjection::Isometric2D,
        .culling_enabled = true,
        .cull_padding = 64.0f,
    };
}

RenderProfile basic_3d_profile() {
    return {
        .kind = RenderProfileKind::Basic3D,
        .name = "basic_3d",
        .sort = RenderSortMode::LayerThenOrder,
        .projection = RenderProjection::Perspective3D,
        .culling_enabled = true,
        .cull_padding = 0.0f,
    };
}

} // namespace kin
