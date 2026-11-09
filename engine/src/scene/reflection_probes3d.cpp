#include <algorithm>
#include <cmath>
#include <kin/ecs/render3d.hpp>
#include <kin/scene/reflection_probes3d.hpp>
#include <string>
#include <unordered_map>
#include <vector>

namespace kin {

struct ReflectionProbes3D::Data {
    explicit Data(Renderer3D& r) : renderer(r) {}
    Renderer3D& renderer;
    struct Probe {
        ReflectionCapture3D capture;
        u32 side = 0;
        Vec3f position{};
        Quatf rotation{};
        u32 recapture = 0;
        f32 captured_at = 0.0f;
        bool captured = false;
        u64 seen = 0; // the update it was last in
    };
    std::unordered_map<u64, Probe> probes;
    u64 updates = 0;
    u32 placed = 0, captured = 0;
};

ReflectionProbes3D::ReflectionProbes3D(Renderer3D& renderer) : _data(std::make_unique<Data>(renderer)) {}
ReflectionProbes3D::~ReflectionProbes3D() = default;
ReflectionProbes3D::ReflectionProbes3D(ReflectionProbes3D&&) noexcept = default;
ReflectionProbes3D& ReflectionProbes3D::operator=(ReflectionProbes3D&&) noexcept = default;
u32 ReflectionProbes3D::probes() const {
    return _data->placed;
}
u32 ReflectionProbes3D::captures() const {
    return _data->captured;
}

Status3D ReflectionProbes3D::update(EcsWorld& world, Scene3D& scene) {
    Data& d = *_data;
    ++d.updates;
    d.placed = d.captured = 0;
    Diagnostics3D errors;
    struct Due {
        u64 id;
        u32 hidden;
        f32 z_near;
    };
    std::vector<Due> due;
    world.each([&](flecs::entity entity, const ReflectionProbe3D& c) {
        if (!c.enabled)
            return;
        const auto* render = entity.get<RenderTransform3D>();
        const auto* placed = entity.get<WorldTransform3D>();
        const Mat4f* m = render && render->enabled ? &render->matrix : placed ? &placed->matrix : nullptr;
        if (!m)
            return;
        const Vec3f x{m->m[0], m->m[1], m->m[2]}, y{m->m[4], m->m[5], m->m[6]}, z{m->m[8], m->m[9], m->m[10]};
        const Vec3f scale{length(x), length(y), length(z)};
        if (!(scale.x > 1e-6f && scale.y > 1e-6f && scale.z > 1e-6f) || !std::isfinite(scale.x + scale.y + scale.z))
            return;
        const Vec3f position{m->m[12], m->m[13], m->m[14]};
        const Quatf rotation = Quatf::look_rotation(-(z / scale.z), y / scale.y);
        Data::Probe& p = d.probes[entity.id()];
        p.seen = d.updates;
        const u32 side = std::clamp(c.resolution, 16u, 1024u);
        if (!p.capture || p.side != side) {
            auto made = d.renderer.create_reflection_capture(side);
            if (!made) {
                errors.insert(errors.end(), made.diagnostics.begin(), made.diagnostics.end());
                return;
            }
            p.capture = *made;
            p.side = side;
            p.captured = false;
        }
        const f32 turn = rotation.x * p.rotation.x + rotation.y * p.rotation.y + rotation.z * p.rotation.z +
                         rotation.w * p.rotation.w;
        const bool moved = length(position - p.position) > 1e-4f || std::abs(std::abs(turn) - 1.0f) > 1e-6f;
        const bool clock = c.interval > 0.0f && scene.time - p.captured_at >= c.interval;
        if (!p.captured || moved || c.recapture != p.recapture || clock || c.every_frame)
            due.push_back({entity.id(), c.hide_self ? static_cast<u32>(entity.id()) : 0u, std::max(c.z_near, 1e-4f)});
        p.position = position;
        p.rotation = rotation;
        p.recapture = c.recapture;
        const auto scaled = [&](Vec3f v) { return Vec3f{v.x * scale.x, v.y * scale.y, v.z * scale.z}; };
        scene.lights.reflection_probes.push_back({.position = position,
                                                  .rotation = rotation,
                                                  .box_offset = rotation.rotate(scaled(c.box_offset)),
                                                  .extent = scaled(c.extent),
                                                  .blend = c.blend,
                                                  .parallax_extent = scaled(c.parallax_extent),
                                                  .parallax = c.parallax,
                                                  .intensity = c.intensity,
                                                  .priority = c.priority,
                                                  .capture = p.capture});
        ++d.placed;
    });
    // Probes whose entities are gone: their captures go.
    for (auto it = d.probes.begin(); it != d.probes.end();)
        it = it->second.seen == d.updates ? std::next(it) : d.probes.erase(it);
    // The captures, of the scene with every probe placed (so each sees the others, as last captured).
    for (const Due& c : due) {
        Data::Probe& p = d.probes[c.id];
        if (!d.renderer.capture_reflection(p.capture, scene, p.position, {.z_near = c.z_near, .hidden_id = c.hidden})) {
            const auto& why = d.renderer.diagnostics();
            errors.insert(errors.end(), why.begin(), why.end());
            continue;
        }
        p.captured = true;
        p.captured_at = scene.time;
        ++d.captured;
    }
    return status3d(errors.empty(), std::move(errors));
}

} // namespace kin
