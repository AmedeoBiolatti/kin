#pragma once

#include <kin/core/types.hpp>

namespace kin {

struct ResourceGraphDesc;

// The stock 3D frame's coarse graph (Renderer3D::render with a native renderer): its passes, from clustering the
// lights to the output, and the tokens that order them (docs/render_graph.md). What the renderer compiles and runs
// each frame, for tools that show it: compile it (ResourceGraphCompiler, every feature available) for the order the
// renderer runs it in. RenderDebugStats3D::graph_pass_ms times its passes, by their index here.
inline constexpr u32 frame_graph_passes3d = 13;
const ResourceGraphDesc& stock_frame_graph3d();

} // namespace kin
