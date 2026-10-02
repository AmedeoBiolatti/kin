#pragma once

#include <kin/assets/asset_server.hpp>

namespace kin {
struct SceneAppConfig;
class SceneManager;
struct StateCoverage;
} // namespace kin

namespace kin::runtime_detail {

#ifdef KIN_ENABLE_DETERMINISM_CHECK
// --check-determinism: runs the game several times as child processes in
// lockstep and reports where their states part. Returns the exit code.
int run_determinism_check(const SceneAppConfig& config);

// A --state-lockstep child's side, after each update: prints the state's hash
// and waits for the parent's command. False when the parent says to stop.
bool state_lockstep_step(SceneManager& scenes, i32 frame, StateCoverage& coverage);
// Prints what the run's states held that could not be compared.
void state_lockstep_finish(const StateCoverage& coverage);
#endif

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
