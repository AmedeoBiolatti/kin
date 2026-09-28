#pragma once

#include <kin/assets/asset_server.hpp>

namespace kin::runtime_detail {

// Both front-ends advance scenes through the same asset-pump policy: rendered
// loops stay non-blocking, while headless/server loops drain so simulation
// steps see a complete asset graph.
inline void pump_scene_assets(AssetServer* asset_server, bool render) {
    if (asset_server == nullptr) {
        return;
    }
    asset_server->pump(render ? AssetServer::PumpMode::Budgeted
                              : AssetServer::PumpMode::DrainToQuiescent);
}

} // namespace kin::runtime_detail
