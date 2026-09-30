#pragma once

#include <kin/runtime/scene_app.hpp>
#include <kin/save/save_store.hpp>

#include <filesystem>

#include <memory>
#include <string>

namespace examples {
using namespace kin;

struct Options {
    int enemies = 300, runs = 1000, capture_frame = 120;
    int width = 1280, height = 800;
    float ui_scale = 0; // zero follows the window's current display
    bool benchmark = false, help = false, vsync = false, power_grid = false, mute = false;
    std::string scenario = "live", screenshot;
};

// Saves the frame as --screenshot once `ticks` reaches --capture-frame.
void capture(SceneContext& ctx, const Options& options, int ticks, bool& captured);
// The UI scale (--ui-scale, or the window's display scale); keeps the window at
// least 640 x 480 density-independent units.
float update_ui_scale(Window& window, const Options& options, Vec2i& last_minimum);

// A finished Signal Siege run, and the best one kept between sessions in a
// kin::SaveStore settings file (under the user data directory unless `root`).
struct RunResult {
    bool won = false;
    float time = 0;
    int kills = 0, cores = 0, upgrades = 0;
};
class BestRunStore {
public:
    explicit BestRunStore(std::filesystem::path root = {});
    const RunResult& best() const { return _best; }
    // Keeps `run` if it beats the best (a win, then longer, then more kills).
    bool record(const RunResult& run);
private:
    std::unique_ptr<SaveStore> _saves;
    RunResult _best;
};

// Signal Siege's first scene: the title screen, or the arena for headless,
// benchmark, screenshot and --power-grid runs. Tool runs (those, and server
// runs driven by agents) are silent and never touch the player's saves.
std::unique_ptr<Scene> make_signal_siege(const Options& options, bool start_in_arena, bool tool_run);

} // namespace examples
