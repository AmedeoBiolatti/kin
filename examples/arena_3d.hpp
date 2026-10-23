#pragma once

#include "workloads.hpp"

#if defined(KIN_EXPERIMENTAL_3D)
#include <kin/render3d/renderer3d.hpp>
#include <memory>
#include <optional>
#include <vector>

namespace examples {

// Signal Siege in 3D (signal_siege --view 3d; docs/3d_plan.md, M1): the same
// simulation, drawn with kin's experimental 3D from a camera behind and above
// the player. The deck, its walls, the enemies, shots and cores are meshes
// made in code; the HUD goes on top as in 2D. Like ArenaPainter it only reads
// the simulation.
//
// The arena's 2D coordinates (pixels, y down) are the ground's x and z, a
// 64-pixel cell to a metre: up on the screen is -z, away from the camera.
class ArenaPainter3D {
public:
    static constexpr float pixels_per_metre = 64.0f;

    void init(Renderer2D& renderer);
    bool ready() const { return _r3d != nullptr; }

    // The arena into the playfield (`playfield`, logical coordinates).
    void draw(Renderer2D& renderer, const Arena& arena, Rectf playfield);
    // The arena point under `screen` (logical coordinates), on the plane shots
    // fly in: where the mouse aims.
    std::optional<Vec2f> screen_to_world(Vec2f screen) const;
    // Where an arena point shows on the screen, at `height` metres.
    std::optional<Vec2f> world_to_screen(Vec2f world, float height = 0.0f) const;

    const RenderStats3D& stats() const;
    bool available() const;

private:
    void fill_scene(const Arena& arena);

    std::unique_ptr<Renderer3D> _r3d;
    View3D _view;
    Scene3D _scene;
    Rectf _playfield{};
    Mesh3D _tile, _wall, _ship, _star, _plate, _ring, _shot, _core, _spark;
    std::vector<MeshInstance3D> _tiles, _walls;
    std::array<std::vector<MeshInstance3D>, 3> _enemies;
};

} // namespace examples
#endif
