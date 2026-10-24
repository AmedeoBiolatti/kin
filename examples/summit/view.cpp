#include "view.hpp"

#include "../sponza_walk/character.hpp"
#include "../sponza_walk/ik_rig.hpp"
#include "audio.hpp" // camp_fire_position

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <kin/assets/content.hpp>
#include <kin/assets/image.hpp>
#include <kin/ecs/scene3d.hpp>
#include <kin/render3d/timeofday3d.hpp>
#include <kin/renderer/renderer2d.hpp>
#include <kin/scene/scene_asset.hpp>

namespace summit {

namespace {

// A rotation taking +Y to the unit vector `d`.
Quatf from_up_to(Vec3f d) {
    const f32 c = std::clamp(d.y, -1.0f, 1.0f);
    if (c > 0.99999f)
        return {};
    if (c < -0.99999f)
        return Quatf::axis_angle({1.0f, 0.0f, 0.0f}, 180.0f);
    const Vec3f axis = cross({0.0f, 1.0f, 0.0f}, d);
    return Quatf::axis_angle(axis, std::acos(c) * 180.0f / pi);
}

f32 smooth(f32 a, f32 b, f32 x) {
    const f32 t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

u32 hash32(u32 x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

// ---- procedural textures
// --------------------------------------------------------------------------------------------------------

// Value noise on a lattice of `period` cells that wraps: a texture that tiles.
f32 periodic_noise(f32 x, f32 y, i32 period, u32 seed) {
    const auto lat = [&](i32 i, i32 j) {
        const i32 ii = ((i % period) + period) % period, jj = ((j % period) + period) % period;
        return f32(hash32(u32(ii) * 73856093u ^ u32(jj) * 19349663u ^ seed) & 0xFFFFu) / 65535.0f;
    };
    const i32 x0 = i32(std::floor(x)), y0 = i32(std::floor(y));
    const f32 fx = x - f32(x0), fy = y - f32(y0);
    const f32 sx = fx * fx * (3.0f - 2.0f * fx), sy = fy * fy * (3.0f - 2.0f * fy);
    const f32 a = lat(x0, y0), b = lat(x0 + 1, y0), c = lat(x0, y0 + 1), d = lat(x0 + 1, y0 + 1);
    return (a + (b - a) * sx) * (1.0f - sy) + (c + (d - c) * sx) * sy;
}

// Tileable cellular noise: the distance to the nearest and second nearest of one random point a cell, `period` cells a
// side (with the nearest cell's number for a per-block offset).
struct Cells {
    f32 f1 = 0.0f, f2 = 0.0f;
    u32 id = 0;
};
Cells cellular(f32 x, f32 y, i32 period_x, i32 period_y, u32 seed) {
    const i32 x0 = i32(std::floor(x)), y0 = i32(std::floor(y));
    Cells c;
    c.f1 = c.f2 = 1e9f;
    for (i32 j = -1; j <= 1; ++j) {
        for (i32 i = -1; i <= 1; ++i) {
            const i32 cx = x0 + i, cy = y0 + j;
            const i32 wx = ((cx % period_x) + period_x) % period_x, wy = ((cy % period_y) + period_y) % period_y;
            const u32 h = hash32(u32(wx) * 73856093u ^ u32(wy) * 19349663u ^ seed);
            const f32 px = f32(cx) + 0.15f + 0.7f * f32(h & 0xFFFFu) / 65535.0f;
            const f32 py = f32(cy) + 0.15f + 0.7f * f32((h >> 16) & 0xFFFFu) / 65535.0f;
            const f32 d = std::sqrt((px - x) * (px - x) + (py - y) * (py - y));
            if (d < c.f1) {
                c.f2 = c.f1;
                c.f1 = d;
                c.id = h;
            } else if (d < c.f2) {
                c.f2 = d;
            }
        }
    }
    return c;
}

// The rock's relief and fractures as a height 0..1 over one tile: broad lumps, blocks (cells with their own height)
// split by grooves, and grain.
f32 rock_height(f32 u, f32 v, u32 seed) {
    f32 h = 0.0f, amp = 0.5f, norm = 0.0f;
    for (i32 o = 0; o < 5; ++o) {
        const i32 period = 4 << o;
        h += amp * periodic_noise(u * f32(period), v * f32(period), period, seed + u32(o) * 101u);
        norm += amp;
        amp *= 0.55f;
    }
    h /= norm;
    f32 groove = 0.0f, block = 0.0f;
    // Beds wider than they are tall, jointed: the cells are stretched along x.
    const i32 px[2] = {2, 5}, py[2] = {3, 8};
    for (i32 k = 0; k < 2; ++k) {
        const Cells c = cellular(u * f32(px[k]), v * f32(py[k]), px[k], py[k], seed + 500u + u32(k) * 77u);
        groove = std::max(groove, (1.0f - smooth(0.0f, 0.08f - 0.03f * f32(k), c.f2 - c.f1)) * (k == 0 ? 1.0f : 0.3f));
        block += (f32(c.id & 0xFFFFu) / 65535.0f - 0.5f) * (k == 0 ? 0.28f : 0.14f);
    }
    return std::clamp(h + block - 0.16f * groove, 0.0f, 1.0f);
}

// Grass: fine blades and clumps, 0..1.
f32 grass_height(f32 u, f32 v, u32 seed) {
    f32 h = 0.0f, amp = 0.5f, norm = 0.0f;
    for (i32 o = 0; o < 5; ++o) {
        const i32 period = 8 << o;
        h += amp * periodic_noise(u * f32(period), v * f32(period), period, seed + u32(o) * 57u);
        norm += amp;
        amp *= 0.8f;
    }
    return h / norm;
}

// ---- the climber
// ------------------------------------------------------------------------------------------------------------

// The person: the parts from build_climber_meshes (climber.cpp, no renderer) uploaded, on person_rig()'s skeleton and
// clips.
std::shared_ptr<const Model3D> make_climber(Renderer3D& r3d, const ClimberStyle& style) {
    const sponza_walk::PersonRig rig = sponza_walk::person_rig();
    const Skeleton3D& sk = rig.skeleton;

    auto model = std::make_shared<Model3D>();
    model->skeletons.push_back(sk);
    model->clips = rig.clips;
    for (const ClimberPart& p : build_climber_meshes(style)) {
        ModelPart3D part;
        part.name = p.name;
        part.mesh = r3d.create_mesh(p.mesh).value_or({});
        part.material = {.color = p.colour, .roughness = p.roughness};
        part.material.metallic = p.metallic;
        part.skin = 0;
        model->parts.push_back(std::move(part));
    }
    model->bounds = {{-2.5f, -0.6f, -2.5f}, {2.5f, 3.0f, 2.5f}};
    return model;
}

// Replaces what the fixture's clips get wrong for a person whose arms hang: the walk and run swing the arms about Y
// with the arms out (a T), and the idle needs the arms down; a `Squash` layer for landings (additive: a crouch from
// rest) and the fall's flail.
void make_climber_clips(Model3D& model) {
    const Skeleton3D& sk = model.skeletons[0];
    const u32 n = sk.joint_count();
    const auto quat = [](Vec3f axis, f32 degrees) { return Quatf::axis_angle(axis, degrees); };
    const f32 down = 74.0f; // the arm's drop from the T pose (about Z, mirrored)
    const auto arm_rot = [&](bool left, f32 swing, f32 out_extra) {
        // First the arm comes down (about Z), then the swing is about the chest's X axis.
        const f32 s = left ? -1.0f : 1.0f;
        return quat({1, 0, 0}, swing) * quat({0, 0, 1}, s * (down - out_extra));
    };
    for (AnimClip3D& clip : model.clips) {
        if (clip.name == "Walk" || clip.name == "Run") {
            const f32 swing = clip.name == "Run" ? 55.0f : 30.0f;
            const f32 period = clip.duration;
            for (const bool left : {true, false}) {
                Track3D<Quatf>& t = clip.channels[std::size_t(sk.find(left ? "shoulder_L" : "shoulder_R"))].rotation;
                t.times.clear();
                t.values.clear();
                constexpr int keys = 8;
                for (int k = 0; k <= keys; ++k) {
                    const f32 phase = 6.2831853f * f32(k) / f32(keys);
                    t.times.push_back(period * f32(k) / f32(keys));
                    // The left arm swings with the right leg: opposite to the left hip.
                    t.values.push_back(arm_rot(left, (left ? 1.0f : -1.0f) * swing * std::sin(phase), 0.0f));
                }
                Track3D<Quatf>& e = clip.channels[std::size_t(sk.find(left ? "elbow_L" : "elbow_R"))].rotation;
                e.times = {0.0f, period};
                // The forearm bent forward a little, more when running (about the elbow's own axis, as the idle's).
                const f32 bend = clip.name == "Run" ? 55.0f : 20.0f;
                e.values = {quat({0, 1, 0}, left ? -bend : bend), quat({0, 1, 0}, left ? -bend : bend)};
            }
        } else if (clip.name == "Fall") {
            // A fall: the arms flung up and out.
            const auto hold = [&](const char* joint, Vec3f axis, f32 degrees) {
                Track3D<Quatf>& t = clip.channels[std::size_t(sk.find(joint))].rotation;
                t.times = {0.0f, 1.0f};
                t.values = {quat(axis, degrees), quat(axis, degrees)};
            };
            hold("shoulder_L", {0, 0, 1}, 40.0f);
            hold("shoulder_R", {0, 0, 1}, -40.0f);
            hold("elbow_L", {0, 1, 0}, -25.0f);
            hold("elbow_R", {0, 1, 0}, 25.0f);
        }
    }
    // Idle: arms down with a slow breath.
    {
        AnimClip3D idle;
        idle.name = "Idle";
        idle.duration = 4.0f;
        idle.channels.resize(n);
        const auto track = [&](const char* joint, auto&& value) {
            const i32 j = sk.find(joint);
            if (j < 0)
                return;
            Track3D<Quatf>& t = idle.channels[std::size_t(j)].rotation;
            t.times = {0.0f, 2.0f, 4.0f};
            t.values = {value(0.0f), value(1.0f), value(0.0f)};
        };
        track("shoulder_L", [&](f32 k) { return arm_rot(true, 0.0f, 0.0f) * quat({0, 0, 1}, k * -1.5f); });
        track("shoulder_R", [&](f32 k) { return arm_rot(false, 0.0f, 0.0f) * quat({0, 0, 1}, k * 1.5f); });
        track("elbow_L", [&](f32) { return quat({0, 1, 0}, -14.0f); });
        track("elbow_R", [&](f32) { return quat({0, 1, 0}, 14.0f); });
        track("chest", [&](f32 k) { return quat({1, 0, 0}, 1.5f * k); });
        track("head", [&](f32 k) { return quat({0, 1, 0}, 3.0f * k); });
        model.clips.push_back(std::move(idle));
    }
    // Squash: the first key is the rest pose, the second (after a millisecond) the crouch, and it stays: as an additive
    // layer its weight is how deep the crouch is. The pelvis goes down, thighs forward, knees bent, the spine leaning,
    // the arms out for balance.
    {
        AnimClip3D sq;
        sq.name = "Squash";
        sq.duration = 10000.0f;
        sq.channels.resize(n);
        const auto rot = [&](const char* joint, Vec3f axis, f32 degrees) {
            const i32 j = sk.find(joint);
            if (j < 0)
                return;
            Track3D<Quatf>& t = sq.channels[std::size_t(j)].rotation;
            t.times = {0.0f, 0.001f, sq.duration};
            t.values = {Quatf{}, quat(axis, degrees), quat(axis, degrees)};
        };
        rot("hip_L", {1, 0, 0}, -48.0f);
        rot("hip_R", {1, 0, 0}, -48.0f);
        rot("knee_L", {1, 0, 0}, 85.0f);
        rot("knee_R", {1, 0, 0}, 85.0f);
        rot("ankle_L", {1, 0, 0}, -30.0f);
        rot("ankle_R", {1, 0, 0}, -30.0f);
        rot("spine", {1, 0, 0}, 14.0f);
        rot("chest", {1, 0, 0}, 8.0f);
        const i32 p = sk.find("pelvis");
        if (p >= 0) {
            Track3D<Vec3f>& t = sq.channels[std::size_t(p)].translation;
            const Vec3f rest = sk.rest[std::size_t(p)].translation;
            t.times = {0.0f, 0.001f, sq.duration};
            t.values = {rest, rest + Vec3f{0.0f, -0.28f, 0.04f}, rest + Vec3f{0.0f, -0.28f, 0.04f}};
        }
        model.clips.push_back(std::move(sq));
    }
}

} // namespace

// ---- the view
// ---------------------------------------------------------------------------------------------------------------------

namespace {

Material3D rock_material(const RenderMeshData& m, const Texture& albedo, const Texture& normal, f32 normal_scale,
                         const Texture& orm = {}) {
    Material3D mat = m.material;
    mat.texture = albedo;
    mat.normal = normal;
    mat.normal_scale = normal_scale;
    if (orm) {
        mat.metallic_roughness = orm;
        mat.occlusion = orm;
        mat.occlusion_strength = 0.55f;
        mat.roughness = 1.0f;
    }
    // Tiling: the default is to clamp at the edge.
    mat.wrap_u = Wrap3D::Repeat;
    mat.wrap_v = Wrap3D::Repeat;
    return mat;
}

} // namespace

void SummitView::make_textures(bool external_assets) {
    if (external_assets && !content_root().empty()) {
        const auto load = [&](const char* name, bool srgb) {
            const auto path = content_root() / "ambientcg" / name;
            if (!content_file_exists(path))
                return Texture{};
            const Image image = load_image(path);
            if (!image.valid() || image.size != Vec2i{1024, 1024})
                return Texture{};
            Renderer2D& r = _r3d->renderer();
            Texture t = srgb ? r.create_texture_from_rgba(image.rgba.data(), image.size)
                             : r.create_texture(image.size, TextureFormat::Rgba8, image.rgba.data());
            if (t)
                r.set_scale_mode(t, ScaleMode::Mipmapped);
            return t;
        };
        _rock_albedo = load("Rock030-Color.jpg", true);
        _rock_normal = load("Rock030-NormalGL.jpg", false);
        _rock_orm = load("Rock030-ORM.png", false);
        _grass_albedo = load("Ground037-Color.jpg", true);
        _grass_normal = load("Ground037-NormalGL.jpg", false);
        _grass_orm = load("Ground037-ORM.png", false);
        if (_rock_albedo && _rock_normal && _rock_orm && _grass_albedo && _grass_normal && _grass_orm) {
            _imported_materials = true;
            std::printf("summit: imported PBR materials 2/2, six 1K maps, shared ORM/AO\n");
            return;
        }
        _rock_orm = _grass_orm = Texture{};
    }
#ifdef __EMSCRIPTEN__
    if (external_assets)
        throw std::runtime_error("Summit's bundled terrain materials are incomplete");
#endif
    constexpr i32 size = 512;
    std::vector<f32> h(std::size_t(size) * std::size_t(size));
    for (i32 y = 0; y < size; ++y) {
        for (i32 x = 0; x < size; ++x)
            h[std::size_t(y * size + x)] = rock_height(f32(x) / f32(size), f32(y) / f32(size), 1234u);
    }
    std::vector<u8> normal(std::size_t(size) * std::size_t(size) * 4), albedo(normal.size());
    const auto at = [&](i32 x, i32 y) { return h[std::size_t(((y + size) % size) * size + ((x + size) % size))]; };
    for (i32 y = 0; y < size; ++y) {
        for (i32 x = 0; x < size; ++x) {
            const f32 dx = (at(x + 1, y) - at(x - 1, y)) * 6.0f, dy = (at(x, y + 1) - at(x, y - 1)) * 6.0f;
            Vec3f n = normalize({-dx, -dy, 1.0f});
            const std::size_t i = std::size_t(y * size + x) * 4;
            normal[i] = u8(std::clamp(n.x * 0.5f + 0.5f, 0.0f, 1.0f) * 255.0f + 0.5f);
            normal[i + 1] = u8(std::clamp(n.y * 0.5f + 0.5f, 0.0f, 1.0f) * 255.0f + 0.5f);
            normal[i + 2] = u8(std::clamp(n.z * 0.5f + 0.5f, 0.0f, 1.0f) * 255.0f + 0.5f);
            normal[i + 3] = 255;
            // A detail albedo around 0.9: darker in the grooves and lows, a little lighter on the knobs.
            const f32 v = std::clamp(0.80f + 0.34f * (h[std::size_t(y * size + x)] - 0.4f), 0.45f, 1.0f);
            const u8 g = u8(v * 255.0f + 0.5f);
            albedo[i] = g;
            albedo[i + 1] = g;
            albedo[i + 2] = u8(std::clamp(v * 0.97f, 0.0f, 1.0f) * 255.0f + 0.5f);
            albedo[i + 3] = 255;
        }
    }
    // The grass: the same, finer, with more contrast between clumps and a weaker normal.
    {
        std::vector<f32> g(h.size());
        for (i32 y = 0; y < size; ++y) {
            for (i32 x = 0; x < size; ++x)
                g[std::size_t(y * size + x)] = grass_height(f32(x) / f32(size), f32(y) / f32(size), 777u);
        }
        std::vector<u8> gn(normal.size()), ga(normal.size());
        const auto gat = [&](i32 x, i32 y) { return g[std::size_t(((y + size) % size) * size + ((x + size) % size))]; };
        for (i32 y = 0; y < size; ++y) {
            for (i32 x = 0; x < size; ++x) {
                const f32 dx = (gat(x + 1, y) - gat(x - 1, y)) * 5.0f, dy = (gat(x, y + 1) - gat(x, y - 1)) * 5.0f;
                const Vec3f n = normalize({-dx, -dy, 1.0f});
                const std::size_t i = std::size_t(y * size + x) * 4;
                gn[i] = u8(std::clamp(n.x * 0.5f + 0.5f, 0.0f, 1.0f) * 255.0f + 0.5f);
                gn[i + 1] = u8(std::clamp(n.y * 0.5f + 0.5f, 0.0f, 1.0f) * 255.0f + 0.5f);
                gn[i + 2] = u8(std::clamp(n.z * 0.5f + 0.5f, 0.0f, 1.0f) * 255.0f + 0.5f);
                gn[i + 3] = 255;
                const f32 v = std::clamp(0.78f + 0.9f * (g[std::size_t(y * size + x)] - 0.5f), 0.4f, 1.0f);
                ga[i] = u8(v * 0.96f * 255.0f + 0.5f);
                ga[i + 1] = u8(v * 255.0f + 0.5f);
                ga[i + 2] = u8(v * 0.9f * 255.0f + 0.5f);
                ga[i + 3] = 255;
            }
        }
        Renderer2D& rg = _r3d->renderer();
        _grass_albedo = rg.create_texture_from_rgba(ga.data(), {size, size});
        _grass_normal = rg.create_texture(Vec2i{size, size}, TextureFormat::Rgba8, gn.data());
        if (_grass_albedo)
            rg.set_scale_mode(_grass_albedo, ScaleMode::Mipmapped);
        if (_grass_normal)
            rg.set_scale_mode(_grass_normal, ScaleMode::Mipmapped);
    }
    Renderer2D& r = _r3d->renderer();
    _rock_albedo = r.create_texture_from_rgba(albedo.data(), {size, size});
    _rock_normal = r.create_texture(Vec2i{size, size}, TextureFormat::Rgba8, normal.data());
    if (_rock_albedo)
        r.set_scale_mode(_rock_albedo, ScaleMode::Mipmapped);
    if (_rock_normal)
        r.set_scale_mode(_rock_normal, ScaleMode::Mipmapped);
}

SummitView::SummitView(Renderer3D& r3d, const SummitLevel& level, bool external_assets, bool core_quality)
    : _r3d(&r3d), _core_quality(core_quality), _assets(r3d, content_root()),
      _external_assets(_assets, level, external_assets), _look(&looks().front()), _bounds(level.bounds),
      _level_height(level.height()) {
    make_textures(external_assets);
    const auto upload = [&](std::vector<RenderMeshData> data, std::vector<Part>& into, bool textured,
                            f32 normal_scale) {
        for (const RenderMeshData& m : data) {
            const bool grass = m.name == "ground" || m.name == "terrain";
            Material3D mat =
                grass      ? rock_material(m, _grass_albedo, _grass_normal, _grass_orm ? 0.65f : 0.8f, _grass_orm)
                : textured ? rock_material(m, _rock_albedo, _rock_normal, _rock_orm ? 0.75f : normal_scale, _rock_orm)
                           : m.material;
            into.push_back({r3d.create_mesh(m.mesh).value_or({}), mat});
            _static_triangles += m.mesh.triangle_count();
        }
    };
    upload(build_render_meshes(level), _rock, true, 1.4f);
    upload(build_decor_meshes(level), _decor, true, 1.0f);
    upload(build_terrain_meshes(level), _terrain, false, 1.0f);
    _cylinder = r3d.create_mesh(meshes::cylinder(1.0f, 1.0f, 10)).value_or({});
    _cone = r3d.create_mesh(meshes::cone(1.0f, 1.0f, 10)).value_or({});
    _cone4 = r3d.create_mesh(meshes::cone(1.0f, 1.0f, 4)).value_or({});
    _box = r3d.create_mesh(meshes::box({1.0f, 1.0f, 1.0f})).value_or({});
    _sphere = r3d.create_mesh(meshes::sphere(1.0f, 12, 8)).value_or({});
    _camps = level.camps;
    _summit = level.summit;
    for (const Piece& p : level.pieces)
        if (p.kind == PieceKind::Equipment)
            _equipment.push_back(p);
    _fires.resize(_camps.size());
    for (std::size_t i = 0; i < _fires.size(); ++i) {
        Fire& f = _fires[i];
        f.flames.set_seed(100 + i);
        f.smoke.set_seed(200 + i);
        f.sparks.set_seed(300 + i);
        ParticleDesc3D& fl = f.flames.desc;
        fl.shape = ParticleShape3D::Cone;
        fl.extent = {0.14f, 0.0f, 0.0f};
        fl.cone_angle = 10.0f;
        fl.rate = 70.0f;
        fl.lifetime = {0.45f, 0.85f};
        fl.speed = {0.7f, 1.4f};
        fl.size = {0.22f, 0.34f};
        fl.size_start = 0.6f;
        fl.size_mid = 1.0f;
        fl.size_end = 0.15f;
        fl.colour_start = {1.7f, 0.75f, 0.2f};
        fl.colour_end = {0.6f, 0.08f, 0.02f};
        fl.alpha_start = 0.9f;
        fl.alpha_end = 0.0f;
        fl.fade_in = 0.1f;
        fl.drag = 1.2f;
        fl.wind_influence = 0.5f;
        fl.look.blend = ParticleBlend3D::Additive;
        fl.look.softness = 1.0f;
        fl.look.sort = false;
        ParticleDesc3D& sm = f.smoke.desc;
        sm.shape = ParticleShape3D::Disc;
        sm.extent = {0.1f, 0.0f, 0.0f};
        sm.rate = 9.0f;
        sm.lifetime = {2.2f, 3.6f};
        sm.speed = {0.5f, 0.9f};
        sm.size = {0.3f, 0.5f};
        sm.size_start = 0.5f;
        sm.size_mid = 1.3f;
        sm.size_end = 2.4f;
        sm.colour_start = {0.32f, 0.31f, 0.30f};
        sm.colour_end = {0.5f, 0.5f, 0.52f};
        sm.alpha_start = 0.28f;
        sm.alpha_end = 0.0f;
        sm.fade_in = 0.12f;
        sm.drag = 0.8f;
        sm.wind_influence = 1.0f;
        sm.look.blend = ParticleBlend3D::Alpha;
        sm.look.lit = 1.0f;
        ParticleDesc3D& sp = f.sparks.desc;
        sp.shape = ParticleShape3D::Cone;
        sp.extent = {0.1f, 0.0f, 0.0f};
        sp.cone_angle = 28.0f;
        sp.rate = 7.0f;
        sp.lifetime = {0.7f, 1.4f};
        sp.speed = {1.6f, 3.2f};
        sp.size = {0.025f, 0.04f};
        sp.gravity = {0.0f, -2.5f, 0.0f};
        sp.drag = 0.4f;
        sp.colour_start = {2.5f, 1.1f, 0.25f};
        sp.colour_end = {1.5f, 0.2f, 0.0f};
        sp.alpha_start = 1.0f;
        sp.alpha_end = 0.0f;
        sp.look.blend = ParticleBlend3D::Additive;
        sp.look.facing = ParticleFacing3D::Stretched;
        sp.look.stretch = 0.05f;
        sp.look.sort = false;
    }
    {
        ParticleDesc3D& d = _dust.desc;
        d.shape = ParticleShape3D::Point;
        d.spread = 80.0f;
        d.rate = 0.0f;
        d.max_particles = 600;
        d.lifetime = {0.5f, 1.1f};
        d.speed = {0.8f, 2.2f};
        d.size = {0.22f, 0.4f};
        d.size_start = 0.5f;
        d.size_mid = 1.0f;
        d.size_end = 1.6f;
        d.colour_start = {0.62f, 0.55f, 0.45f};
        d.colour_end = {0.6f, 0.56f, 0.5f};
        d.alpha_start = 0.5f;
        d.alpha_end = 0.0f;
        d.fade_in = 0.05f;
        d.drag = 3.0f;
        d.gravity = {0.0f, 0.3f, 0.0f};
        d.look.lit = 1.0f;
        ParticleDesc3D& b = _breath.desc;
        b.shape = ParticleShape3D::Point;
        b.spread = 22.0f;
        b.rate = 0.0f;
        b.max_particles = 80;
        b.lifetime = {0.9f, 1.5f};
        b.speed = {0.35f, 0.7f};
        b.size = {0.07f, 0.1f};
        b.size_start = 0.5f;
        b.size_mid = 1.4f;
        b.size_end = 2.6f;
        b.colour_start = {0.9f, 0.92f, 0.96f};
        b.colour_end = {0.9f, 0.92f, 0.96f};
        b.alpha_start = 0.3f;
        b.alpha_end = 0.0f;
        b.fade_in = 0.15f;
        b.drag = 1.5f;
        b.look.lit = 1.0f;
    }
    if (r3d.capabilities().skinning) {
        _ecs = std::make_unique<::kin::EcsWorld>();
        register_builtin_editor_components(*_ecs);
        std::shared_ptr<const Model3D> imported;
        if (external_assets && !content_root().empty()) {
            const auto prefab = load_prefab_asset(content_root() / "prefabs/survivor.kinprefab", _ecs->components());
            for (const auto& error : prefab.diagnostics)
                std::fprintf(stderr, "summit: %s\n", error.c_str());
            if (prefab.ok()) {
                auto instance = instantiate_prefab(*_ecs, *prefab.prefab, {}, _ecs->components());
                for (const auto& error : instance.diagnostics)
                    std::fprintf(stderr, "summit: %s\n", error.c_str());
                const auto errors = resolve_render3d_assets(*_ecs, _assets).diagnostics;
                for (const auto& error : errors)
                    std::fprintf(stderr, "summit: %s\n", error.c_str());
                const auto* renderer = instance.root.get<SkinnedMeshRenderer3D>();
                auto model = renderer ? renderer->model : nullptr;
                const Skeleton3D canonical = sponza_walk::person_rig().skeleton;
                // The offline fit keeps the exact game rig. Reject an incompatible replacement rather than breaking IK
                // or ragdolls.
                if (model && model->valid() && model->skeletons.size() == 1 &&
                    model->skeletons[0].names == canonical.names && model->skeletons[0].parents == canonical.parents &&
                    model->find_clip("Idle") && model->find_clip("Walk") && model->find_clip("Run") &&
                    model->find_clip("Fall")) {
                    imported = std::move(model);
                    _person = instance.root.raw();
                } else {
                    std::fprintf(stderr,
                                 "summit: survivor prefab unavailable or incompatible; using procedural climber\n");
                    for (auto entity : instance.entities)
                        if (entity.alive())
                            entity.raw().destruct();
                }
            }
        }
        if (imported) {
            _imported_person = true;
            std::printf("summit: authored survivor loaded, canonical 25-joint rig, five clips\n");
            _person_model = std::move(imported);
        } else {
#ifdef __EMSCRIPTEN__
            if (external_assets)
                throw std::runtime_error("Summit's bundled survivor rig/animations are incomplete");
#endif
            auto model = std::make_shared<Model3D>(*make_climber(r3d, style_for_player(0)));
            make_climber_clips(*model);
            _person_model = std::move(model);
            _person = _ecs->raw().entity().set(Transform3D{}).set(SkinnedMeshRenderer3D{.model = _person_model});
        }
        _state = std::make_unique<WorldRenderState3D>(*_ecs);
        const auto* rig_asset = _person.get<AnimatorAsset3D>();
        const bool authored_rig = rig_asset && rig_asset->configuration;
        Animator3D animator = locomotion_animator({.idle = "Idle", .walk = "Walk", .run = "Run", .fall = "Fall"});
        if (authored_rig) {
            animator = *_person.get<Animator3D>();
            std::printf(
                "summit: authored animator loaded, shared kinanim machine, speed blend, three IK constraints\n");
        } else {
            if (const auto* authored = _person.get<Animator3D>()) {
                animator.clip = authored->clip;
                animator.speed = authored->speed;
                animator.loop = authored->loop;
                animator.playing = authored->playing;
                animator.fade_seconds = authored->fade_seconds;
                animator.apply_root_motion = authored->apply_root_motion;
                animator.remove_root_motion = authored->remove_root_motion;
            }
            IKConstraint3D feet = sponza_walk::person_constraints(nullptr)[0];
            feet.fade_seconds = 0.12f;
            animator.add_ik(feet);
            for (const char* side : {"L", "R"}) {
                IKConstraint3D hand;
                hand.name = std::string("hand_") + side;
                hand.kind = IKKind3D::TwoBone;
                hand.joints = {std::string("shoulder_") + side, std::string("elbow_") + side,
                               std::string("hand_") + side};
                hand.weight = 0.0f;
                hand.fade_seconds = 0.12f;
                hand.pole = IKConstraint3D::Pole::ModelDirection;
                hand.pole_vector = {side[0] == 'L' ? 0.5f : -0.5f, -0.7f, -0.5f};
                hand.two_bone.max_stretch = 1.12f;
                animator.add_ik(hand);
            }
            Animator3D::LayerConfig squash;
            squash.clip = "Squash";
            squash.weight = 0.0f;
            squash.additive = true;
            squash.loop = true;
            if (_person_model->find_clip("Squash"))
                animator.layers.push_back(squash);
        }
        _person.set(static_cast<const Animator3D&>(animator));
        _person_ok = true;
    }
}

bool SummitView::set_look(const std::string& name) {
    const Look* l = find_look(name);
    if (l == nullptr)
        return false;
    _look = l;
    return true;
}

void SummitView::set_effects(const EffectSettings& effects, f32 time_hours_per_minute) {
    _effects = effects;
    if (_core_quality || !_r3d->capabilities().particles) {
        _effects.fires = false;
        _effects.puffs = false;
        _effects.wind_particles = false;
    }
    _time_speed = time_hours_per_minute;
}

ViewOptions3D SummitView::view_options(u32 samples) const {
    if (_core_quality)
        return {.samples = 1, .background = Color::rgb(120, 150, 190)};
    return {.samples = samples,
            .background = Color::rgb(120, 150, 190),
            .hdr = true,
            .exposure = _look->exposure,
            .bloom = _look->bloom,
            .bloom_threshold = 1.0f};
}

u32 SummitView::particles_alive() const {
    std::size_t n = _dust.count() + _breath.count();
    for (const Fire& f : _fires)
        n += f.flames.count() + f.smoke.count() + f.sparks.count();
    return u32(n);
}

void SummitView::on_step(const Game& game, i32 player) {
    const CharacterState3D cs = game.character_state(player);
    for (const GameEvent& e : game.events()) {
        if (e.player != player)
            continue;
        if (e.kind == GameEventKind::Landed && e.value >= 2.5f) {
            if (_pending_dust.size() < 8) {
                _pending_dust.push_back(e.point + Vec3f{0.0f, 0.1f, 0.0f});
                _pending_dust_size.push_back(e.value);
            }
            _squash = std::max(_squash, std::clamp((e.value - 1.5f) / 5.0f, 0.35f, 1.0f));
        } else if (e.kind == GameEventKind::Jumped) {
            _squash = std::max(_squash, 0.3f);
        }
    }
    (void)cs;
}

void SummitView::drive_person(const Game& game, i32 player, f32 dt) {
    if (!_person_ok || !_person.is_alive())
        return;
    const Player& p = game.player(player);
    const CharacterState3D cs = game.character_state(player);
    const TraversalState3D ts = game.traversal(player);
    Transform3D* t = _person.get_mut<Transform3D>();
    // Between steps the person is drawn where the camera's instant puts it (a person at the latest step against a
    // camera between two jumps sideways by a step's travel at the step rate).
    const Vec3f feet_now = _pose_set ? _pose_feet : cs.position;
    t->position = feet_now;
    t->rotation = Quatf::axis_angle({0.0f, 1.0f, 0.0f}, _pose_set ? _pose_facing : p.facing_yaw);
    // While it hangs free the body sags below the capsule until its arms are straight to the holds (the arm is 0.95 m;
    // a hold nearer than that bends it). With feet on the ground nothing moves.
    f32 sag_target = 0.0f;
    if (p.state == PlayerState::Alive && ts.mode == TraversalMode3D::Hanging &&
        cs.ground != CharacterGround3D::OnGround) {
        f32 want = 0.0f;
        i32 n = 0;
        for (std::size_t h = 0; h < 2; ++h) {
            const GripState3D& g = ts.grips[h];
            if (g.phase != GripPhase3D::Held && g.phase != GripPhase3D::Slipping)
                continue;
            const f32 side = h == 0 ? 0.2f : -0.2f;
            const Vec3f shoulder = t->position + t->rotation.rotate({side, 1.65f, 0.0f});
            const Vec3f hold = g.point + g.normal * 0.05f;
            const Vec3f d = hold - shoulder;
            const f32 horizontal2 = d.x * d.x + d.z * d.z;
            const f32 reach = 0.93f;
            // The vertical offset at which the arm is straight: sqrt(reach^2 - h^2), against what there is now.
            const f32 need = std::sqrt(std::max(0.0f, reach * reach - horizontal2));
            want += std::max(0.0f, need - d.y);
            ++n;
        }
        if (n > 0) {
            sag_target = std::min(0.55f, want / f32(n));
            // Not into the ground: what is under the feet limits it.
            QueryFilter3D filter;
            filter.mask = layer_bit(layers3d::Static);
            const Vec3f feet = feet_now;
            if (const auto hit =
                    game.world().raycast(feet + Vec3f{0.0f, 0.1f, 0.0f}, feet - Vec3f{0.0f, 0.8f, 0.0f}, filter)) {
                sag_target = std::min(sag_target, std::max(0.0f, hit->fraction * 0.9f - 0.12f));
            }
        }
    }
    _hang_sag += (sag_target - _hang_sag) * std::min(1.0f, dt * 8.0f);
    t->position.y -= _hang_sag;
    Animator3D* a = _person.get_mut<Animator3D>();
    CharacterAnimationLink3D link;
    link.hands = {"hand_L", "hand_R"};
    link.hand_clearance = 0.05f;
    link.ground_mask = layer_bit(layers3d::Static);
    drive_character_animator(*a, cs, game.world(), link);
    drive_traversal_animator(*a, ts, link, {0.0f, 1.0f, 0.0f});
    // The landing's crouch (an additive layer): decays over about a third of a second; none while it hangs or lies.
    _squash = std::max(0.0f, _squash - dt * 2.6f);
    const bool crouch_ok = p.state == PlayerState::Alive && ts.mode == TraversalMode3D::Walking;
    if (!a->layers.empty())
        a->layers[0].weight = crouch_ok ? _squash : 0.0f;
    // The ragdoll's pose over the clips while it is the body (and while it blends back).
    game.ragdoll_pose(player, _bones);
    PoseOverride3D& o = a->pose_override;
    bool any = false;
    for (const RagdollBonePose3D& b : _bones)
        any = any || b.weight > 0.0f;
    if (!any) {
        o.clear();
    } else {
        const u32 joints = game.skeleton_joints();
        o.weights.assign(joints, 0.0f);
        o.position.assign(joints, Vec3f{});
        o.rotation.assign(joints, Quatf{});
        const std::span<const i32> map = game.ragdoll_joints();
        for (std::size_t k = 0; k < map.size() && k < _bones.size(); ++k) {
            const std::size_t j = std::size_t(map[k]);
            o.weights[j] = _bones[k].weight;
            o.position[j] = _bones[k].position;
            o.rotation[j] = _bones[k].rotation;
        }
    }
    _state->animate(dt);
}

void SummitView::draw_expedition(Scene3D& scene, const Game& game) {
    const ExpeditionLayout& e = game.level().expedition;
    const bool online = game.player(0).expedition.phase >= ExpeditionPhase::Extraction;
    const Color gold = Color::rgb(236, 185, 60), blue = Color::rgb(65, 179, 224), orange = Color::rgb(235, 105, 48);
    for (const Piece& p : _equipment) {
        Material3D mat{.color = p.tag == 1   ? Color::rgb(104, 75, 48)
                                : p.tag == 3 ? Color::rgb(36, 62, 74)
                                             : Color::rgb(72, 83, 96),
                       .roughness = p.tag == 2 ? 0.35f : 0.8f};
        mat.metallic = p.tag == 2 ? 0.7f : 0.0f;
        (void)scene.draw(_box, Mat4f::trs(p.centre, p.rotation, p.half * 2.0f), mat);
    }
    const auto trail = [&](const std::vector<Vec3f>& points, Color colour) {
        for (std::size_t i = 1; i < points.size(); ++i) {
            const Vec3f d = points[i] - points[i - 1];
            // A steep climb has no painted walking line through its wall.
            if (std::abs(d.y) > 0.7f * std::sqrt(d.x * d.x + d.z * d.z))
                continue;
            const int n = std::max(1, int(length(d) / 2.5f));
            for (int j = 0; j <= n; ++j) {
                const Vec3f point = points[i - 1] + d * (f32(j) / f32(n));
                (void)scene.draw(_cylinder, Mat4f::trs(point + Vec3f{0, 0.10f, 0}, {}, {0.22f, 0.03f, 0.22f}),
                                 Material3D{.color = colour, .emissive = 0.25f, .roughness = 0.8f});
            }
        }
    };
    trail(e.direct_trail, orange);
    trail(e.sheltered_trail, blue);
    for (const Leg& l : game.level().route) {
        if (l.kind != LegKind::Walk || (l.to.y < 18 && l.to != e.fork))
            continue;
        if (l.from.y >= 18 || l.from == game.level().start)
            trail({l.from, l.to}, gold);
    }
    // A gold approach leads out of base camp to the fork.
    trail({game.level().start, game.level().route[0].to, e.fork}, gold);
    // Trailhead signs: coloured slats at the fork and at both cave openings, away from the walk lines.
    for (const auto& [point, colour] :
         std::vector<std::pair<Vec3f, Color>>{{e.fork + e.rotation.rotate({-2, 0, 0}), orange},
                                              {e.sheltered_trail[1] + e.rotation.rotate({3, 0, 0}), blue},
                                              {e.sheltered_trail[4] + e.rotation.rotate({3, 0, 0}), blue}}) {
        (void)scene.draw(_box, Mat4f::trs(point + Vec3f{0, 1.4f, 0}, e.rotation, {1.4f, 0.24f, 0.05f}),
                         Material3D{.color = colour, .emissive = 0.15f, .roughness = 0.75f});
        (void)scene.draw(_cylinder, Mat4f::trs(point + Vec3f{0, 0.7f, 0}, {}, {0.035f, 1.4f, 0.035f}),
                         Material3D{.color = Color::rgb(105, 85, 64), .roughness = 0.9f});
    }
    const Vec3f lamp = e.shelter_centre + e.rotation.rotate({-3.2f, 0.3f, 0});
    (void)scene.draw(_sphere, Mat4f::trs(lamp, {}, {0.12f, 0.18f, 0.12f}),
                     Material3D{.color = Color::rgb(250, 205, 121), .emissive = 3.0f, .roughness = 0.4f});
    PointLightSource3D cave_light;
    cave_light.position = lamp;
    cave_light.color = {8.0f, 5.0f, 2.5f};
    cave_light.radius = 12;
    cave_light.size = 0.2f;
    cave_light.id = 3000;
    scene.add_point_light(cave_light);
    const Vec3f signal = e.beacon + e.rotation.rotate({1.9f, 5.6f, 0});
    const Color signal_colour = online ? Color::rgb(93, 237, 158) : gold;
    (void)scene.draw(_sphere, Mat4f::trs(signal, {}, {0.22f, 0.22f, 0.22f}),
                     Material3D{.color = signal_colour, .emissive = online ? 4.0f : 1.0f, .roughness = 0.4f});
    // The extraction pad's cross and perimeter switch on only after the beacon is restored.
    const Material3D pad{.color = online ? Color::rgb(93, 237, 158) : Color::rgb(130, 144, 152),
                         .emissive = online ? 0.8f : 0.0f,
                         .roughness = 0.8f};
    for (Vec3f size : {Vec3f{3.0f, 0.025f, 0.4f}, Vec3f{0.4f, 0.025f, 3.0f}})
        (void)scene.draw(_box, Mat4f::trs(e.extraction + Vec3f{0, 0.09f, 0}, e.rotation, size), pad);
    for (int x : {-1, 1})
        for (int z : {-1, 1})
            (void)scene.draw(_sphere,
                             Mat4f::trs(e.extraction + e.rotation.rotate({f32(x) * 2.6f, 0.12f, f32(z) * 2.6f}), {},
                                        {0.08f, 0.08f, 0.08f}),
                             pad);
    ++_lights_added;
}

void SummitView::fill(Scene3D& scene, const Game& game, const CameraView& camera, f32 dt, i32 player) {
    scene.clear();
    scene.camera = camera.camera();
    _clock += f64(dt);
    Lights3D& l = scene.lights;
    l = Lights3D{};
    Look k = *_look;
    if (_start_hours >= 0.0f)
        k.hours = _start_hours;
    const Wind& wind = game.wind();

    EnvironmentInput in;
    in.look = &k;
    in.altitude = game.altitude(player);
    in.height = _level_height;
    in.game_seconds = game.time();
    in.wind_speed = wind.speed;
    in.effects = _effects;
    in.time_speed = _time_speed;
    _env = evaluate_environment(in);
    const EnvironmentState& e = _env;

    scene.sky = {.enabled = true,
                 .turbidity = e.turbidity,
                 .sun_intensity = e.sun_intensity,
                 .intensity = 1.0f,
                 .sun_size = 1.0f,
                 .sun_disc = 30.0f,
                 .stars = !_core_quality && k.stars,
                 .moon = false};
    scene.fog = {.enabled = true,
                 .density = e.fog_density,
                 .height_density = e.fog_height_density,
                 .height_falloff = 0.02f,
                 .base_height = game.level().start.y,
                 .colour_from_sky = true,
                 .sun_scatter = 0.25f,
                 .sun_anisotropy = 0.6f};
    l.sky_lights_scene = true;
    TimeOfDay3D day{.hours = e.hours, .day_of_year = 172.0f, .latitude = 46.0f};
    day.compute_ambient = !_core_quality;
    const SkyState3D sky_state = kin::apply(day, l, scene.sky, scene.fog);
    if (_core_quality) {
        _core_ambient.update(scene.sky, l.sun.direction);
        l.sky_lights_scene = false;
        l.ambient = _core_ambient.ambient;
        l.ground_ambient = {.enabled = true, .color = _core_ambient.ground};
    }
    l.shadow_area = _bounds;
    l.shadow_resolution = 2048;
    // Four cascades over 64 m: the climber's wall is a few metres away and the sun often grazes it; wider maps show the
    // texels as stripes on such a face.
    l.shadow_cascades = 4;
    if (_core_quality) {
        l.shadow_resolution = 1024;
        l.shadow_cascades = 2;
    }
    l.shadow_distance = 64.0f;
    Weather3D& w = scene.weather;
    w = Weather3D{};
    w.rain.intensity = e.rain;
    w.snow.intensity = e.snow;
    w.mist.intensity = e.mist;
    w.dust.intensity = e.dust;
    if (in_expedition_shelter(game.level(), game.feet(player))) {
        w.rain.intensity = w.snow.intensity = w.mist.intensity = w.dust.intensity = 0.0f;
    }
    const f32 a = wind.direction * pi / 180.0f;
    w.wind.direction = {std::cos(a), 0.0f, std::sin(a)};
    w.wind.speed = wind.speed;
    w.wind.gust_strength = e.gust_strength;
    w.wind.turbulence = e.turbulence;
    w.time = game.time();
    if (w.active()) {
        const Fog3D base = scene.fog;
        w.apply(l, scene.fog, base);
    }
    if (_core_quality) {
        // Keep weather's fog/light changes, without the native weather sprites.
        w.rain.intensity = w.snow.intensity = w.mist.intensity = w.dust.intensity = 0;
    }

    for (const Part& p : _terrain)
        (void)scene.draw(p.mesh, Mat4f::identity(), p.material);
    for (const Part& p : _rock)
        (void)scene.draw(p.mesh, Mat4f::identity(), p.material);
    for (const Part& p : _decor)
        (void)scene.draw(p.mesh, Mat4f::identity(), p.material);
    _external_assets.draw_scenery(scene);

    // Camps: a tent, a groundsheet, a flag, a fire ring with logs; the fire's light, a lantern, flames and smoke.
    const Material3D tent{.color = Color::rgb(214, 96, 60), .double_sided = true, .roughness = 0.85f};
    const Material3D tent_dark{.color = Color::rgb(120, 54, 38), .roughness = 0.9f};
    const Material3D pole{.color = Color::rgb(215, 215, 210), .roughness = 0.5f};
    const Material3D flag{.color = Color::rgb(235, 190, 40), .double_sided = true, .roughness = 0.7f};
    const Material3D stone{.color = Color::rgb(112, 108, 102), .roughness = 0.95f};
    const Material3D log_mat{.color = Color::rgb(70, 46, 28), .roughness = 0.9f};
    const Material3D ember{.color = Color::rgb(255, 120, 40), .emissive = 6.0f, .unlit = false, .roughness = 0.9f};
    const Material3D lantern{.color = Color::rgb(255, 214, 140), .emissive = 2.0f, .roughness = 0.5f};
    _lights_added = 0;
    const f32 fire_strength = _effects.camp_lights ? fire_light_strength(sky_state.daylight) : 0.0f;
    const Vec3f cam = camera.position;
    for (std::size_t i = 0; i < _camps.size(); ++i) {
        const Vec3f c = _camps[i].marker;
        const Vec3f fire = camp_fire_position(_camps[i]);
        const f32 yaw = f32(i) * 40.0f + 15.0f;
        const Quatf q = Quatf::axis_angle({0, 1, 0}, yaw);
        // Authored camp props, with the original primitive tent as a missing-content fallback.
        if (!_external_assets.draw(scene, ExternalAssets::Tent, c, 2.3f, yaw)) {
            (void)scene.draw(_cone4,
                             Mat4f::trs(c + Vec3f{0.0f, 0.62f, 0.0f}, q * Quatf::axis_angle({0, 1, 0}, 45.0f),
                                        {1.55f, 1.25f, 1.55f}),
                             tent);
            (void)scene.draw(_box, Mat4f::trs(c + Vec3f{0.0f, 0.03f, 0.0f}, q, {2.3f, 0.06f, 2.3f}), tent_dark);
        }
        for (const PropPlacement& prop : camp_props(_camps[i], i)) {
            if (prop.kind == Tent || prop.kind == Firepit)
                continue;
            (void)_external_assets.draw(scene, prop.kind, prop.feet, prop.span, prop.yaw);
        }
        (void)scene.draw(_cylinder, Mat4f::trs(c + Vec3f{-1.7f, 1.3f, 0.0f}, {}, {0.035f, 2.6f, 0.035f}), pole);
        (void)scene.draw(_box, Mat4f::trs(c + Vec3f{-1.7f, 2.2f, 0.34f}, {}, {0.02f, 0.42f, 0.68f}), flag);
        // The fire ring: stones round a hearth, two logs across, embers at the middle.
        const bool imported_fire = _external_assets.draw(scene, ExternalAssets::Firepit, fire, 0.95f, yaw);
        if (!imported_fire) {
            for (i32 s = 0; s < 8; ++s) {
                const f32 ang = f32(s) * 0.7853982f + 0.2f * f32(i);
                (void)scene.draw(_sphere,
                                 Mat4f::trs(fire + Vec3f{std::cos(ang) * 0.42f, 0.06f, std::sin(ang) * 0.42f}, {},
                                            {0.13f, 0.09f, 0.12f}),
                                 stone);
            }
            (void)scene.draw(_cylinder,
                             Mat4f::trs(fire + Vec3f{0.0f, 0.1f, 0.0f}, q * Quatf::axis_angle({0, 0, 1}, 80.0f),
                                        {0.06f, 0.8f, 0.06f}),
                             log_mat);
            (void)scene.draw(_cylinder,
                             Mat4f::trs(fire + Vec3f{0.0f, 0.13f, 0.0f},
                                        q * Quatf::axis_angle({0, 1, 0}, 70.0f) * Quatf::axis_angle({0, 0, 1}, 84.0f),
                                        {0.055f, 0.7f, 0.055f}),
                             log_mat);
        }
        (void)scene.draw(_sphere, Mat4f::trs(fire + Vec3f{0.0f, 0.06f, 0.0f}, {}, {0.18f, 0.05f, 0.18f}), ember);
        // A lantern on the flag's pole.
        (void)scene.draw(_sphere, Mat4f::trs(c + Vec3f{-1.7f, 1.75f, -0.12f}, {}, {0.07f, 0.1f, 0.07f}), lantern);

        const f32 dist = length(fire - cam);
        Fire& f = _fires[i];
        const bool near = dist < 120.0f;
        f.flames.emitting = f.smoke.emitting = f.sparks.emitting = _effects.fires && near;
        if (_effects.fires && near) {
            Wind3D air = w.wind;
            f.flames.transform = f.smoke.transform = f.sparks.transform =
                Mat4f::translation(fire + Vec3f{0.0f, 0.12f, 0.0f});
            f.smoke.transform = Mat4f::translation(fire + Vec3f{0.0f, 0.65f, 0.0f});
            f.flames.update(f64(dt), &air);
            f.smoke.update(f64(dt), &air);
            f.sparks.update(f64(dt), &air);
            (void)scene.draw_particles(f.smoke);
            (void)scene.draw_particles(f.flames);
            (void)scene.draw_particles(f.sparks);
        }
        if (fire_strength > 0.0f && dist < 90.0f) {
            // A flicker from a few sines, so that no two fires beat together.
            const f32 t = f32(_clock);
            const f32 ph = f32(i) * 1.7f;
            const f32 flicker = 1.0f + 0.10f * std::sin(t * 11.0f + ph) + 0.07f * std::sin(t * 17.3f + ph * 2.0f) +
                                0.05f * std::sin(t * 5.1f + ph * 0.5f);
            PointLightSource3D pl;
            pl.position = fire + Vec3f{0.0f, 0.7f, 0.0f};
            pl.color = kin::LinearRgb{1.0f, 0.52f, 0.2f} * (14.0f * fire_strength * flicker);
            pl.radius = 16.0f;
            pl.size = 0.3f;
            pl.id = u32(1000 + i);
            scene.add_point_light(pl);
            SpotLightSource3D sl;
            sl.position = c + Vec3f{-1.7f, 1.7f, -0.12f};
            sl.direction = {0.55f, -0.8f, 0.1f};
            sl.color = kin::LinearRgb{1.0f, 0.78f, 0.5f} * (9.0f * fire_strength);
            sl.radius = 9.0f;
            sl.size = 0.1f;
            sl.inner_angle = 28.0f;
            sl.outer_angle = 55.0f;
            sl.id = u32(2000 + i);
            if (!_core_quality)
                scene.add_spot_light(sl);
            _lights_added += _core_quality ? 1 : 2;
        }
    }
    if (game.config().expedition)
        draw_expedition(scene, game);
    else {
        (void)scene.draw(_cylinder, Mat4f::trs(_summit + Vec3f{0.0f, 2.0f, 0.0f}, {}, {0.06f, 4.0f, 0.06f}), pole);
        (void)scene.draw(_box, Mat4f::trs(_summit + Vec3f{0.0f, 3.4f, 0.55f}, {}, {0.03f, 0.7f, 1.1f}),
                         Material3D{.color = Color::rgb(220, 40, 40), .double_sided = true, .roughness = 0.7f});
        // A cairn of stones under the pole.
        for (i32 s = 0; s < 5; ++s) {
            const f32 ang = f32(s) * 1.2566f;
            const f32 r = s == 0 ? 0.0f : 0.5f;
            (void)scene.draw(
                _sphere,
                Mat4f::trs(_summit + Vec3f{std::cos(ang) * r, 0.18f + (s == 0 ? 0.25f : 0.0f), std::sin(ang) * r}, {},
                           {0.4f, 0.28f, 0.38f}),
                stone);
        }
    }

    // The rope(s): a cylinder to each segment.
    const Material3D rope{.color = Color::rgb(170, 130, 80), .roughness = 0.9f};
    for (const RopeHandle3D r : game.ropes()) {
        const f32 radius = game.world().rope_radius(r);
        for (const RopeSegment3D& s : game.world().rope_segments(r)) {
            const Vec3f d = s.b - s.a;
            const f32 len = length(d);
            if (len < 1e-4f)
                continue;
            (void)scene.draw(_cylinder,
                             Mat4f::trs((s.a + s.b) * 0.5f, from_up_to(d * (1.0f / len)),
                                        {radius * 1.3f, len + radius * 2.0f, radius * 1.3f}),
                             rope);
        }
    }

    // Puffs: dust where it landed, breath in the cold.
    if (_effects.puffs) {
        Wind3D air = w.wind;
        for (std::size_t i = 0; i < _pending_dust.size(); ++i) {
            _dust.burst_at(_pending_dust[i], u32(6.0f + 3.0f * _pending_dust_size[i]));
        }
        _pending_dust.clear();
        _pending_dust_size.clear();
        _dust.update(f64(dt), &air);
        // The ground's colour: snow's white above the line.
        _dust.desc.colour_start = e.snow_share > 0.5f ? LinearRgb{0.85f, 0.88f, 0.94f} : LinearRgb{0.62f, 0.55f, 0.45f};
        (void)scene.draw_particles(_dust);
        // Breath: a puff each few seconds above a third of the way up (cold air), faster when sprinting or tired.
        const Player& pl = game.player(player);
        const f32 cold = smooth(0.25f, 0.5f, e.altitude_fraction);
        if (cold > 0.0f && pl.state == PlayerState::Alive) {
            const TraversalState3D ts = game.traversal(player);
            const f32 tired = ts.resource.max > 0.0f ? 1.0f - ts.resource.value / ts.resource.max : 0.0f;
            _breath_timer += dt * (0.6f + 1.2f * tired + (pl.command.sprint ? 0.6f : 0.0f));
            if (_breath_timer > 2.2f) {
                _breath_timer = 0.0f;
                const Vec3f head = game.head(player);
                const Vec3f fwd = Quatf::axis_angle({0, 1, 0}, pl.facing_yaw).rotate({0.0f, 0.0f, 1.0f});
                _breath.burst_at(head + fwd * 0.18f + Vec3f{0.0f, -0.08f, 0.0f}, 3);
            }
        }
        _breath.update(f64(dt), &air);
        (void)scene.draw_particles(_breath);
    }

    // The person.
    drive_person(game, player, dt);
    if (_person_ok) {
        _state->propagate_transforms();
        _state->collect(scene);
    } else {
        // Without skinning: a capsule's worth of box where it stands.
        const Vec3f f = game.feet(player);
        (void)scene.draw(_box, Mat4f::trs(f + Vec3f{0.0f, 0.95f, 0.0f}, {}, {0.5f, 1.9f, 0.4f}),
                         Material3D{.color = Color::rgb(215, 150, 110)});
    }

    // Colliders, when asked for: the static bodies near the camera as wireframes (the same bodies the physics uses).
    _collider_mesh = Mesh3D{};
    if (_show_colliders) {
        DebugDraw3D lines;
        const Vec3f eye = scene.camera.position;
        const World3D& world = game.world();
        const auto add = [&](Body3D b, f32 reach, Color colour) {
            if (length(world.position(b) - eye) > _collider_radius + reach)
                return;
            lines.shape(world.shape(b), world.position(b), world.rotation(b), colour);
        };
        const SummitLevel& level = game.level();
        for (std::size_t i = 0; i < level.pieces.size(); ++i) {
            add(game.piece_body(i), length(level.pieces[i].half), Color::rgb(130, 130, 140));
        }
        for (std::size_t j = 0; j < game.decor().size(); ++j) {
            const DecorChunk& c = game.decor()[j];
            add(game.decor_body(j), length(c.half),
                c.kind == DecorKind::Buttress ? Color::rgb(255, 150, 40) : Color::rgb(60, 220, 255));
        }
        for (std::size_t j = 0; j < game.props().size(); ++j)
            add(game.prop_body(j), game.props()[j].span, Color::rgb(110, 240, 130));
        _collider_mesh = draw_debug_lines(*_r3d, scene, lines.lines());
    }
}

} // namespace summit
