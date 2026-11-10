#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <kin/ecs/physics3d.hpp>
#include <kin/ecs/render3d.hpp>
#include <kin/render3d/mesh3d.hpp>
#include <kin/scene/planar_views3d.hpp>
#include <string>
#include <unordered_map>
#include <vector>

namespace kin {

namespace {

// A surface's variants in SurfaceVariant3D's order, as kin_compile_surface_shader names them (its twelve: no cut-outs
// or vertex offsets).
constexpr std::array<const char*, surface_mask_variant3d> surface_variants{
    "plain",    "fog",          "lights",          "lights_fog",
    "velocity", "velocity_fog", "velocity_lights", "velocity_lights_fog",
    "material", "material_fog", "material_lights", "material_lights_fog"};

// An entity's placing (its RenderTransform3D when enabled, else its WorldTransform3D): where, how turned, and its X and
// Y scales. Nothing without one, or squashed flat.
struct Placed {
    Vec3f position;
    Quatf rotation;
    Vec2f scale;
};
std::optional<Placed> placed(EcsEntity entity) {
    const Mat4f* m = nullptr;
    if (const auto* render = entity.get<RenderTransform3D>(); render && render->enabled)
        m = &render->matrix;
    else if (const auto* world = entity.get<WorldTransform3D>())
        m = &world->matrix;
    if (!m)
        return std::nullopt;
    const Vec3f x{m->m[0], m->m[1], m->m[2]}, y{m->m[4], m->m[5], m->m[6]}, z{m->m[8], m->m[9], m->m[10]};
    const f32 sx = length(x), sy = length(y), sz = length(z);
    if (!(sx > 1e-6f && sy > 1e-6f && sz > 1e-6f) || !std::isfinite(sx + sy + sz))
        return std::nullopt;
    return Placed{{m->m[12], m->m[13], m->m[14]}, Quatf::look_rotation(-(z / sz), y / sy), {sx, sy}};
}

Vec2f opening(Vec2f size, Vec2f scale) {
    return {std::max(size.x, 0.0f) * scale.x, std::max(size.y, 0.0f) * scale.y};
}

// Marks a stand-in apart from the next surface's (two equal materials drawn one after another would merge into one
// draw, whose material set_draw_material could not give each back): the last of the 64 parameters, which no stock
// material reads.
void tag(Material3D& material, std::size_t index) {
    material.shader_params[surface_param_count3d - 1] = f32(index + 1);
}

} // namespace

Result3D<PlanarSurfaces3D> create_planar_surfaces(Renderer3D& renderer,
                                                  const std::function<std::span<const u8>(std::string_view)>& spirv) {
    PlanarSurfaces3D out;
    Diagnostics3D errors;
    for (const char* stem : {"planar_reflection", "planar_window"}) {
        std::array<std::span<const u8>, surface_variant_count3d> variants{};
        bool complete = true;
        for (u32 i = 0; i < surface_variants.size(); ++i) {
            const std::string name = std::string(stem) + "." + surface_variants[i] + ".spv";
            variants[i] = spirv ? spirv(name) : std::span<const u8>{};
            if (variants[i].empty()) {
                errors.push_back("planar surfaces: no " + name);
                complete = false;
            }
        }
        if (!complete)
            continue;
        auto made = renderer.create_surface_shader(variants);
        if (!made) {
            errors.insert(errors.end(), made.diagnostics.begin(), made.diagnostics.end());
            continue;
        }
        // (Their look is a picture of the view, new each frame: the GI takes their material's own fields.)
        (std::string_view(stem) == "planar_reflection" ? out.reflection : out.window) =
            SurfaceShader3D{made->backend(), {.gi = SurfaceGi3D::Fields}};
    }
    return result3d(out, std::move(errors));
}

Result3D<PlanarSurfaces3D> compile_planar_surfaces(Renderer3D& renderer) {
#ifdef KIN_SHADER_INCLUDE_DIR
    PlanarSurfaces3D out;
    Diagnostics3D errors;
    const std::filesystem::path folder{KIN_SHADER_INCLUDE_DIR};
    for (const char* stem : {"planar_reflection", "planar_window"}) {
        auto made = renderer.compile_surface_shader(folder / (std::string(stem) + ".surface.glsl"));
        if (!made) {
            errors.insert(errors.end(), made.diagnostics.begin(), made.diagnostics.end());
            continue;
        }
        (std::string_view(stem) == "planar_reflection" ? out.reflection : out.window) =
            SurfaceShader3D{made->backend(), {.gi = SurfaceGi3D::Fields}};
    }
    return result3d(out, std::move(errors));
#else
    (void)renderer;
    return result3d(PlanarSurfaces3D{}, Diagnostics3D{"planar surfaces: this build has no glslc (shipping, or none "
                                                      "found): build them with kin_planar_surfaces"});
#endif
}

std::optional<std::pair<PortalPose3D, PortalPose3D>> portal_poses(EcsEntity portal) {
    const auto* c = portal ? portal.get<Portal3D>() : nullptr;
    if (!c || c->exit.id == 0)
        return std::nullopt;
    const EcsEntity exit{flecs::entity(portal.raw().world(), static_cast<flecs::entity_t>(c->exit.id))};
    if (!exit)
        return std::nullopt;
    const auto in = placed(portal), out = placed(exit);
    if (!in || !out)
        return std::nullopt;
    const auto* other = exit.get<Portal3D>();
    return std::pair{PortalPose3D{in->position, in->rotation, opening(c->size, in->scale)},
                     PortalPose3D{out->position, out->rotation, opening(other ? other->size : c->size, out->scale)}};
}

std::optional<PortalStep3D> portal_step(EcsWorld& world, Vec3f from, Vec3f to) {
    std::optional<PortalStep3D> best;
    f32 first = 2.0f;
    world.each([&](flecs::entity entity, const Portal3D& portal) {
        if (!portal.enabled)
            return;
        const auto poses = portal_poses(EcsEntity{entity});
        if (!poses)
            return;
        const auto t = portal_crossing(poses->first, from, to);
        if (!t || *t >= first)
            return;
        first = *t;
        const Quatf turn = portal_rotation(poses->first, poses->second);
        const Vec3f at = from + (to - from) * *t;
        best = PortalStep3D{EcsEntity{entity},
                            EcsEntity{flecs::entity(entity.world(), static_cast<flecs::entity_t>(portal.exit.id))},
                            portal_point(poses->first, poses->second, at) + turn.rotate(to - at), turn};
    });
    return best;
}

struct PlanarViews3D::Data {
    // A view's sizing: since when it has been much larger than its pictures, and the largest picture since.
    struct Room {
        u64 since = 0;
        Vec2i peak{};
    };
    Renderer3D& renderer;
    PlanarSurfaces3D surfaces;
    Mesh3D quad, box;
    struct Entry {
        std::deque<View3D>
            views; // a mirror's one, a portal's a level each (a deque: a level's view stays put as more come)
        std::deque<Room> room; // each view's
        ViewOptions3D options;
        u64 frame = 0;
    };
    std::unordered_map<EcsId, Entry> entries;
    // Where things went through each portal lately (Portal3D::effect answers them): the point on its surface (its own
    // x, y, metres from its middle), when (Scene3D::time) and how strongly (their size); the four latest.
    struct Disturbance {
        Vec2f at{};
        f32 born = -1e9f;
        f32 strength = 0.0f;
    };
    struct Disturbances {
        std::array<Disturbance, 4> latest{};
        std::size_t next = 0;
        u64 frame = 0;
    };
    std::unordered_map<EcsId, Disturbances> disturbances;
    // A thing at `at` on portal `id`'s surface: a new disturbance, unless one began within 0.3 s and 0.35 m of it (a
    // thing going through slowly leaves a trail of them, a quick one one).
    void disturb(EcsId id, Vec2f at, f32 strength, f32 now) {
        Disturbances& d = disturbances[id];
        d.frame = frame;
        for (const Disturbance& old : d.latest)
            if (now - old.born < 0.3f && std::hypot(old.at.x - at.x, old.at.y - at.y) < 0.35f)
                return;
        d.latest[d.next] = {at, now, strength};
        d.next = (d.next + 1) % d.latest.size();
    }
    u64 frame = 0;
    u32 rendered = 0, skipped = 0;
    const View3D* lit_by = nullptr; // this render's (PlanarViews3D::render)
    explicit Data(Renderer3D& r, PlanarSurfaces3D s) : renderer(r), surfaces(std::move(s)) {}

    // Keeps an entity's views for another frame (it is still there, if off screen).
    void keep(EcsId id) {
        if (auto it = entries.find(id); it != entries.end())
            it->second.frame = frame;
    }
    // The entity's `k`th view for a picture `size` (at most `limit`), with `options`, to render `scene` (made again
    // with other options). As the eye moves, a picture changes size (in steps of 64) many times a second; resizing a
    // view makes all its targets again (the atrium walked through made hundreds of textures a second, and memory grew
    // with them). Where the renderer can render part of a view (Renderer3D::can_render_part), a view's targets only
    // grow, to the largest picture it has shown (an eighth or 64 pixels more; smaller only after ten seconds of all its
    // pictures under half of it), and each render fills its picture's own size (View3D::set_render_size; the surface
    // reads that part: part_of). Where it cannot (temporal anti-aliasing, GI, ...), the view is the picture's size.
    View3D* view(EcsId id, u32 k, Vec2i size, Vec2i limit, const ViewOptions3D& options, const Scene3D& scene) {
        Entry& e = entries[id];
        e.frame = frame;
        if (e.options.hdr != options.hdr || e.options.samples != options.samples ||
            e.options.background != options.background) {
            e.views.clear();
            e.room.clear();
            e.options = options;
        }
        const auto grown = [&](int want, int most) {
            return std::clamp((want + std::max(want / 8, 64) + 63) / 64 * 64, want, std::max(most, want));
        };
        const auto fitted = [&](int want, int most) { return std::clamp(want + 64, want, std::max(most, want)); };
        while (e.views.size() <= k) {
            auto made = renderer.create_view(size, options);
            if (!made)
                return nullptr;
            e.views.push_back(std::move(*made));
            e.room.push_back({});
        }
        View3D& v = e.views[k];
        Room& room = e.room[k];
        const Vec2i have = v.texture().size();
        Vec2i want = size;
        if (this->lit_by ? renderer.can_render_part(v, scene, *this->lit_by) : renderer.can_render_part(v, scene)) {
            const auto roomy = [](int peak, int at) { return at > 2 * peak + 128; };
            room.peak = {std::max(room.peak.x, size.x), std::max(room.peak.y, size.y)};
            if (!(roomy(room.peak.x, have.x) || roomy(room.peak.y, have.y)))
                room = {frame, size};
            want = have;
            if (have.x < size.x || have.y < size.y) {
                want = {have.x >= size.x ? have.x : grown(size.x, limit.x),
                        have.y >= size.y ? have.y : grown(size.y, limit.y)};
                room = {frame, size};
            } else if (frame - room.since >= 600) {
                want = {roomy(room.peak.x, have.x) ? fitted(room.peak.x, limit.x) : have.x,
                        roomy(room.peak.y, have.y) ? fitted(room.peak.y, limit.y) : have.y};
                room = {frame, size};
            }
        }
        if (have != want && !renderer.resize(v, want))
            return nullptr;
        v.set_render_size(want == size ? Vec2i{} : size);
        return &v;
    }
    // How much of `view`'s texture its picture fills, from the top-left (planar_view_params' `share`).
    static Vec2f part_of(const View3D& view) {
        const Vec2i whole = view.size(), part = view.render_size();
        return {whole.x > 0 ? f32(part.x) / f32(whole.x) : 1.0f, whole.y > 0 ? f32(part.y) / f32(whole.y) : 1.0f};
    }
};

namespace {
std::optional<Rectf> intersect(Rectf a, Rectf b) {
    const f32 x0 = std::max(a.x, b.x), y0 = std::max(a.y, b.y);
    const f32 x1 = std::min(a.x + a.w, b.x + b.w), y1 = std::min(a.y + a.h, b.y + b.h);
    if (!(x0 < x1 && y0 < y1))
        return std::nullopt;
    return Rectf{x0, y0, x1 - x0, y1 - y0};
}
} // namespace

PlanarViews3D::PlanarViews3D(Renderer3D& renderer, PlanarSurfaces3D surfaces)
    : _data(std::make_unique<Data>(renderer, std::move(surfaces))) {}
PlanarViews3D::~PlanarViews3D() = default;
PlanarViews3D::PlanarViews3D(PlanarViews3D&&) noexcept = default;
PlanarViews3D& PlanarViews3D::operator=(PlanarViews3D&&) noexcept = default;

void PlanarViews3D::set_surfaces(PlanarSurfaces3D surfaces) {
    _data->surfaces = std::move(surfaces);
}
u32 PlanarViews3D::views_rendered() const {
    return _data->rendered;
}
u32 PlanarViews3D::views_skipped() const {
    return _data->skipped;
}
void PlanarViews3D::clear() {
    _data->entries.clear();
}

Status3D PlanarViews3D::render(EcsWorld& world, Scene3D& scene, Vec2i view_size, const ViewOptions3D& like,
                               const View3D* lit_by) {
    Data& d = *_data;
    d.lit_by = lit_by && lit_by->valid() ? lit_by : nullptr;
    d.rendered = d.skipped = 0;
    ++d.frame;
    Diagnostics3D errors;
    if (view_size.x <= 0 || view_size.y <= 0)
        return status3d(false, {"planar views: the view size must be positive"});
    if (!d.quad) {
        d.quad = d.renderer.create_mesh(meshes::plane({1, 1})).value_or({});
        d.box = d.renderer.create_mesh(meshes::box({1, 1, 1})).value_or({});
        if (!d.quad || !d.box)
            return status3d(false, {"planar views: cannot make their meshes"});
    }
    const Camera3D eye = scene.camera;
    const f32 aspect = f32(view_size.x) / f32(view_size.y);
    // The pictures as sharp as the view: its samples, and its pixels' density (Mirror3D/Portal3D::resolution 1).
    ViewOptions3D options = like;
    options.bloom = 0;
    options.pick = false;
    // A picture goes through the 2D pipeline's texture format: an HDR one would come out tonemapped, and the view
    // would tonemap the reflection again. In linear light, clamped at white, the view tonemaps it once.
    options.hdr = false;
    options.exposure = 1;
    // And what is past white kept (x / (1 + x)): a sun glint in a mirror or through a portal stays as bright as the
    // view would show it (it was cut to white, then tonemapped grey, and did not bloom).
    options.compress = true;

    struct Mirror {
        EcsId id;
        Placed at;
        Vec2f size;
        std::size_t draw;
        Material3D real, stand_in;
        PlanarView3D seen;
        View3D* view = nullptr;
        // Water you see into (Mirror3D::refraction): what is below, through its own view, fogged by its murk.
        PlanarView3D below{};
        View3D* below_view = nullptr;
        Fog3D murk{};
        // Its camera's whole frame (before the cut to where it shows), and the portals in its picture: each one's
        // picture as the mirror's camera sees through it, drawn on the portal while the mirror's picture is.
        Camera3D frame_camera{};
        struct Through {
            std::size_t portal;
            View3D* view;
            Camera3D camera;
            Material3D level;
        };
        std::vector<Through> through;
    };
    struct Portal {
        EcsId id;
        PortalPose3D pose, exit;
        std::size_t draw;
        std::vector<Material3D> levels;
        Material3D stand_in;
        std::vector<PlanarView3D> chain;
        std::vector<View3D*> views;
        u32 shown = 0;
        Vec2i frame{}; // its pictures' frame (Portal3D::resolution)
        Portal3D::Effect effect = Portal3D::Effect::None;
        f32 effect_strength = 1.0f;
    };
    std::vector<Mirror> mirrors;
    std::vector<Portal> portals;
    std::size_t index = 0; // the surfaces' order, for their stand-ins' tags
    // The frame in a picture's pixels: the view's times the surface's resolution.
    const auto frame_size = [&](f32 resolution) {
        const f32 share = std::isfinite(resolution) ? std::clamp(resolution, 0.05f, 1.0f) : 1.0f;
        return Vec2i{std::max(1, int(f32(view_size.x) * share)), std::max(1, int(f32(view_size.y) * share))};
    };
    const auto draw = [&](const Mesh3D& mesh, const Mat4f& transform, const Material3D& material, EcsId id) {
        if (!scene.draw(mesh, transform, material, colors::white, u32(id), id))
            errors.emplace_back("planar views: a surface's draw was refused");
        return scene.draws().empty() ? 0 : scene.draws().size() - 1;
    };

    // A portal's effect in its surface's parameters (kin_planar_window reads them): the effect and its strength, the
    // opening's size; its middle and its x and y axes (where a fragment is on it); its latest disturbances (x, y on it,
    // when, how strongly). The strength fades out as the camera that sees it (`viewer`) comes within a metre and a half
    // of the opening's plane, gone by a quarter of a metre: a camera at the opening (a follow camera as its character
    // walks through) sees the room beyond as it is, the whole screen otherwise one effect.
    const auto effect_params = [&](Material3D& level, const Portal& p, Vec3f viewer) {
        auto& q = level.shader_params;
        const f32 off = std::abs(dot(viewer - p.pose.position, p.pose.rotation.rotate({0, 0, 1})));
        const f32 fade = std::clamp((off - 0.25f) / 1.25f, 0.0f, 1.0f);
        q[44] = f32(u32(p.effect));
        q[45] = std::max(p.effect_strength, 0.0f) * fade * fade * (3.0f - 2.0f * fade);
        q[46] = p.pose.size.x;
        q[47] = p.pose.size.y;
        const Vec3f x = p.pose.rotation.rotate({1, 0, 0}), y = p.pose.rotation.rotate({0, 1, 0});
        for (int i = 0; i < 3; ++i) {
            q[20 + i] = (&p.pose.position.x)[i];
            q[24 + i] = (&x.x)[i];
            q[28 + i] = (&y.x)[i];
        }
        if (const auto found = d.disturbances.find(p.id); found != d.disturbances.end())
            for (std::size_t k = 0; k < found->second.latest.size(); ++k) {
                const Data::Disturbance& t = found->second.latest[k];
                q[48 + 4 * k] = t.at.x;
                q[49 + 4 * k] = t.at.y;
                q[50 + 4 * k] = t.born;
                q[51 + 4 * k] = t.strength;
            }
        else
            for (std::size_t k = 0; k < 4; ++k)
                q[50 + 4 * k] = -1e9f;
    };

    // What is half-way through a portal, drawn at the exit too (portal_straddle): every copy of the scene's draws whose
    // bounds straddle an enabled portal's opening, plain, skinned (a character walking through: its pose as it is) or a
    // batch's, with the copy's id, so pick names it (Scene3D::draw_again). Neither half is cut by the pictures' clip
    // planes (DrawClip3D): cut, a half would show its hollow rim where the two meet. The image is drawn whole; the copy
    // itself is drawn again, whole, by the pictures only (an opaque one: a see-through one would show twice), the cut
    // one staying as it is. A skinned copy's bounds are its mesh's at rest (a character's T-pose: its widest); a
    // batch's groups far from every opening are passed over whole.
    {
        std::vector<std::pair<PortalPose3D, PortalPose3D>> pairs;
        std::vector<EcsId> ids; // each pair's entry
        world.each([&](flecs::entity entity, const Portal3D& c) {
            if (c.enabled)
                if (auto poses = portal_poses(EcsEntity{entity})) {
                    pairs.push_back(*poses);
                    ids.push_back(entity.id());
                }
        });
        // The portal whose surface is `pose` (an exit is the entry of another pair, where it is one).
        const auto portal_at = [&](const PortalPose3D& pose) -> std::optional<EcsId> {
            for (std::size_t i = 0; i < pairs.size(); ++i)
                if (length(pairs[i].first.position - pose.position) < 1e-3f)
                    return ids[i];
            return std::nullopt;
        };
        const auto on_surface = [](const PortalPose3D& pose, Vec3f p) {
            const Vec3f local = pose.rotation.conjugate().rotate(p - pose.position);
            return Vec2f{local.x, local.y};
        };
        // Whether a sphere comes near any opening (inside its slab, within its size and the radius round it).
        const auto near_any = [&](Vec3f centre, f32 radius) {
            for (const auto& [entry, exit] : pairs) {
                const Vec3f local = entry.rotation.conjugate().rotate(centre - entry.position);
                if (std::abs(local.z) < radius + 0.01f &&
                    (entry.size.x <= 0.0f || std::abs(local.x) < entry.size.x * 0.5f + radius) &&
                    (entry.size.y <= 0.0f || std::abs(local.y) < entry.size.y * 0.5f + radius))
                    return true;
            }
            return false;
        };
        struct Image {
            std::size_t draw;
            u32 copy;
            Mat4f transform;
            DrawClip3D clip;
            u64 history_id;
        };
        std::vector<Image> images;
        const auto& draws = scene.draws();
        for (std::size_t di = 0; !pairs.empty() && di < draws.size(); ++di) {
            const Scene3D::Draw& draw = draws[di];
            const std::span<const MeshInstance3D> copies = draw.batch
                                                               ? draw.batch.instances().subspan(draw.first, draw.count)
                                                               : scene.instances().subspan(draw.first, draw.count);
            const Bounds3D& b = draw.mesh.bounds();
            const auto consider = [&](u32 i) {
                const MeshInstance3D& copy = copies[i];
                const Mat4f& m = copy.transform;
                const f32 stretch =
                    std::max({length(Vec3f{m.m[0], m.m[1], m.m[2]}), length(Vec3f{m.m[4], m.m[5], m.m[6]}),
                              length(Vec3f{m.m[8], m.m[9], m.m[10]})});
                const Vec3f centre = m.apply(b.center());
                for (std::size_t k = 0; k < pairs.size(); ++k) {
                    const auto& [entry, exit] = pairs[k];
                    const auto through = portal_straddle(entry, exit, centre, b.radius() * stretch);
                    if (!through)
                        continue;
                    // Where it goes through, on both surfaces (the exit's where it comes out): once its middle is
                    // near (a character's bounds, arms out, reach far round it).
                    if (std::abs(entry.rotation.conjugate().rotate(centre - entry.position).z) < 0.5f) {
                        const f32 strength = std::clamp(b.radius() * stretch, 0.1f, 1.0f);
                        d.disturb(ids[k], on_surface(entry, centre), strength, scene.time);
                        if (const auto other = portal_at(exit))
                            d.disturb(*other, on_surface(exit, portal_point(entry, exit, centre)), strength,
                                      scene.time);
                    }
                    images.push_back({di, i, *through * m, DrawClip3D::whole,
                                      copy.history_id ? copy.history_id ^ 0x9e3779b97f4a7c15ull : 0});
                    if (draw.clip == DrawClip3D::camera &&
                        (draw.material.blend == Blend3D::Opaque || draw.material.blend == Blend3D::Mask))
                        images.push_back({di, i, m, DrawClip3D::whole_if_cut,
                                          copy.history_id ? copy.history_id ^ 0x632be59bd9b4e019ull : 0});
                }
            };
            const auto groups = draw.batch ? draw.batch.groups() : std::span<const InstanceGroup3D>{};
            if (!groups.empty()) {
                for (const InstanceGroup3D& g : groups) {
                    const Bounds3D& w = g.world_bounds;
                    const bool bounded =
                        w.min.x <= w.max.x && w.min.y <= w.max.y && w.min.z <= w.max.z && std::isfinite(w.radius());
                    if (bounded && !near_any(w.center(), w.radius()))
                        continue;
                    for (u32 i = g.first; i < g.first + g.count && i < copies.size(); ++i)
                        consider(i);
                }
            } else {
                for (u32 i = 0; i < copies.size(); ++i)
                    consider(i);
            }
        }
        for (const Image& image : images)
            if (!scene.draw_again(image.draw, image.copy, image.transform, image.clip, image.history_id))
                errors.emplace_back("planar views: an image through a portal was refused");
    }

    // The surfaces, drawn as their stand-ins (the pictures come first); which pictures this frame needs, from where.
    world.each([&](flecs::entity entity, const Mirror3D& c) {
        const auto at = c.enabled ? placed(EcsEntity{entity}) : std::nullopt;
        if (!at)
            return;
        Mirror m{entity.id(), *at, opening(c.size, at->scale), 0, {}, {}, {}, nullptr};
        d.keep(m.id);
        Material3D base{.color = c.color,
                        .metallic = std::clamp(c.metallic, 0.0f, 1.0f),
                        .roughness = std::clamp(c.roughness, 0.045f, 1.0f)};
        if (c.normal) {
            base.normal = c.normal;
            base.normal_scale = c.normal_scale;
            base.wrap_u = base.wrap_v = Wrap3D::Repeat;
            base.uv_scale = c.uv_scale;
            base.uv_offset = {std::fmod(c.flow.x * scene.time, 1.0f), std::fmod(c.flow.y * scene.time, 1.0f)};
        }
        tag(base, index++);
        m.stand_in = m.real = base;
        // Water seen in another planar view (a portal's picture, a mirror's) has no picture of what is below it of its
        // own: its stand-in is its murk, as deep water mostly shows, a little see-through where it refracts (the floor
        // under shallow water, faintly), in place of a dark lid.
        if (c.refraction > 0.0f && c.metallic < 1.0f) {
            m.stand_in.color = c.murk;
            m.stand_in.color.a = u8(std::lround(255.0f * (1.0f - 0.3f * std::clamp(c.refraction, 0.0f, 1.0f))));
            m.stand_in.blend = Blend3D::Alpha;
        }
        const auto bounds = planar_on_screen(eye, aspect, at->position, at->rotation, m.size);
        if (bounds && d.surfaces.reflection) {
            // Only where the mirror shows (its ripples read a little round it: the margin), as sharp as the view.
            const PlanarPicture3D picture = planar_picture(*bounds, frame_size(c.resolution));
            m.view = d.view(m.id, 0, picture.size, frame_size(c.resolution), options, scene);
            if (m.view) {
                m.seen = mirror_view(eye, plane_through(at->position, at->rotation.rotate({0, 0, 1})), aspect);
                m.frame_camera = m.seen.camera;
                crop_planar_view(m.seen, picture.window, f32(picture.size.x) / f32(picture.size.y), true);
                m.real.surface = d.surfaces.reflection;
                // (Water the GI sees through: the light comes down into the pool, not off a dark lid.)
                if (c.refraction > 0.0f && c.metallic < 1.0f)
                    m.real.surface = SurfaceShader3D{d.surfaces.reflection.backend(), {.gi = SurfaceGi3D::None}};
                m.real.emissive_texture = m.view->texture();
                planar_view_params(m.real.shader_params, m.seen.projector, c.distortion,
                                   std::clamp(c.strength, 0.0f, 1.0f), Data::part_of(*m.view), true);
                // Water you see into: what is below the surface, in a picture of the same place, in its base colour
                // map. Its murk is a height fog thickening fast under the surface (level water: its base height the
                // surface's), about two thirds gone looking straight down `murk_depth` deep.
                if (c.refraction > 0.0f && c.metallic < 1.0f)
                    m.below_view = d.view(m.id, 1, picture.size, frame_size(c.resolution), options, scene);
                if (m.below_view) {
                    m.below = refraction_view(eye, plane_through(at->position, at->rotation.rotate({0, 0, 1})), aspect);
                    crop_planar_view(m.below, picture.window, f32(picture.size.x) / f32(picture.size.y), false);
                    m.real.texture = m.below_view->texture();
                    planar_refraction_params(m.real.shader_params, m.below.projector, c.refraction_distortion,
                                             std::clamp(c.refraction, 0.0f, 1.0f), Data::part_of(*m.below_view), true);
                    const f32 k = 3.0f / std::max(c.murk_depth, 0.01f);
                    m.murk = Fog3D{.enabled = true,
                                   .height_density = k / (std::exp(3.0f) - 1.0f),
                                   .height_falloff = k,
                                   .base_height = at->position.y,
                                   .colour = to_linear_rgb(c.murk)};
                }
            }
        } else
            ++d.skipped;
        // (A square faces +Y: turned to face the entity's +Z.)
        m.draw = draw(
            d.quad, Mat4f::trs(at->position, at->rotation * Quatf::axis_angle({1, 0, 0}, 90), {m.size.x, 1, m.size.y}),
            m.stand_in, m.id);
        mirrors.push_back(std::move(m));
    });
    world.each([&](flecs::entity entity, const Portal3D& c) {
        const auto poses = c.enabled ? portal_poses(EcsEntity{entity}) : std::nullopt;
        if (!poses)
            return;
        Portal p{entity.id(), poses->first, poses->second, 0, {}, {}, {}, {}, 0};
        p.effect = c.effect;
        p.effect_strength = c.effect_strength;
        d.keep(p.id);
        p.stand_in = {.color = c.stand_in, .unlit = true, .double_sided = true, .casts_shadow = false};
        tag(p.stand_in, index++);
        const u32 depth = std::clamp(c.depth, 1u, 8u);
        if (d.surfaces.window) {
            // Level by level while the level before sees the portal: each cut to where it shows within the window of
            // the level before (a portal seen through itself is smaller each time).
            p.chain = portal_chain(eye, p.pose, p.exit, aspect, depth);
            const Mat4f through = Mat4f::translation(p.exit.position) *
                                  Mat4f::rotation(portal_rotation(p.pose, p.exit)) *
                                  Mat4f::translation(-p.pose.position);
            const Vec2i frame = frame_size(c.resolution);
            p.frame = frame;
            Camera3D from = eye; // the level before's whole frame (portal_chain made each level from it)
            Rectf within{0, 0, 1, 1};
            while (p.shown < p.chain.size()) {
                const auto bounds = planar_on_screen(from, aspect, p.pose.position, p.pose.rotation, p.pose.size);
                const auto seen = bounds ? intersect(*bounds, within) : std::nullopt;
                if (!seen)
                    break;
                const PlanarPicture3D picture = planar_picture(*seen, frame);
                View3D* view = d.view(p.id, p.shown, picture.size, frame, options, scene);
                if (!view)
                    break;
                PlanarView3D& level = p.chain[p.shown];
                from = level.camera;
                crop_planar_view(level, picture.window, f32(picture.size.x) / f32(picture.size.y), false, through);
                p.views.push_back(view);
                within = *seen;
                ++p.shown;
            }
            d.skipped += depth - p.shown;
        } else
            ++d.skipped;
        for (u32 k = 0; k < p.shown; ++k) {
            Material3D level = p.stand_in;
            level.color = colors::white;
            level.surface = d.surfaces.window;
            level.emissive_texture = p.views[k]->texture();
            planar_view_params(level.shader_params, p.chain[k].projector, 0.0f, 1.0f, Data::part_of(*p.views[k]), true);
            // (Level k is seen from the level before's camera; the first from the eye.)
            effect_params(level, p, k == 0 ? eye.position : p.chain[k - 1].camera.position);
            p.levels.push_back(std::move(level));
        }
        // A box behind the opening, so that a camera a little way into it still sees through.
        p.draw = draw(d.box, portal_surface_transform(p.pose, 0.25f), p.stand_in, p.id);
        portals.push_back(std::move(p));
    });
    if (!errors.empty())
        return status3d(false, std::move(errors));

    // Portals in mirrors: where a portal shows in a mirror's picture, a picture of what is beyond it as the mirror's
    // camera sees it (one level: the portals in that are stand-ins), for the mirror's picture to show.
    if (d.surfaces.window)
        for (Mirror& m : mirrors) {
            if (!m.view)
                continue;
            const Rectf within = m.seen.camera.window; // where the mirror shows, in its camera's frame
            for (std::size_t i = 0; i < portals.size(); ++i) {
                const Portal& p = portals[i];
                if (p.frame.x <= 0)
                    continue;
                const auto bounds =
                    planar_on_screen(m.frame_camera, aspect, p.pose.position, p.pose.rotation, p.pose.size);
                const auto seen = bounds ? intersect(*bounds, within) : std::nullopt;
                if (!seen)
                    continue;
                const PlanarPicture3D picture = planar_picture(*seen, p.frame);
                // (Its views are the portal's own entry's, keyed by the mirror too.)
                View3D* view = d.view(p.id ^ (m.id * 0x9e3779b97f4a7c15ull), 0, picture.size, p.frame, options, scene);
                if (!view)
                    continue;
                PlanarView3D level = portal_view(m.frame_camera, p.pose, p.exit, aspect);
                const Mat4f through = Mat4f::translation(p.exit.position) *
                                      Mat4f::rotation(portal_rotation(p.pose, p.exit)) *
                                      Mat4f::translation(-p.pose.position);
                crop_planar_view(level, picture.window, f32(picture.size.x) / f32(picture.size.y), false, through);
                Material3D shown = p.stand_in;
                shown.color = colors::white;
                shown.surface = d.surfaces.window;
                shown.emissive_texture = view->texture();
                planar_view_params(shown.shader_params, level.projector, 0.0f, 1.0f, Data::part_of(*view), true);
                effect_params(shown, p, m.frame_camera.position);
                m.through.push_back({i, view, level.camera, std::move(shown)});
            }
        }

    // The pictures, each with the planar surfaces as stand-ins, but for a portal's own next level; without the overlay.
    const bool overlay = scene.show_overlay;
    scene.show_overlay = false;
    const auto picture = [&](View3D& view, const Camera3D& camera) {
        scene.camera = camera;
        if (!(d.lit_by ? d.renderer.render(view, scene, *d.lit_by) : d.renderer.render(view, scene))) {
            const auto& why = d.renderer.diagnostics();
            errors.insert(errors.end(), why.begin(), why.end());
            if (why.empty())
                errors.emplace_back("planar views: a picture's render failed");
            return false;
        }
        ++d.rendered;
        return true;
    };
    for (const Mirror& m : mirrors) {
        // The portals it sees first (with the planar surfaces as stand-ins), then its own picture with them on them.
        bool ok = true;
        for (const Mirror::Through& t : m.through)
            ok = ok && picture(*t.view, t.camera);
        for (const Mirror::Through& t : m.through)
            (void)scene.set_draw_material(portals[t.portal].draw, t.level);
        if (ok && m.view)
            ok = picture(*m.view, m.seen.camera);
        for (const Mirror::Through& t : m.through)
            (void)scene.set_draw_material(portals[t.portal].draw, portals[t.portal].stand_in);
        if (!ok)
            break;
        if (m.below_view) { // (with the water's murk for fog)
            const Fog3D fog = scene.fog;
            scene.fog = m.murk;
            const bool ok = picture(*m.below_view, m.below.camera);
            scene.fog = fog;
            if (!ok)
                break;
        }
    }
    for (Portal& p : portals) {
        for (u32 k = p.shown; k-- > 0 && errors.empty();) {
            (void)scene.set_draw_material(p.draw, k + 1 < p.shown ? p.levels[k + 1] : p.stand_in);
            if (!picture(*p.views[k], p.chain[k].camera))
                break;
        }
        (void)scene.set_draw_material(p.draw, p.stand_in);
    }
    // The view's: the surfaces as themselves.
    scene.show_overlay = overlay;
    scene.camera = eye;
    for (const Mirror& m : mirrors)
        (void)scene.set_draw_material(m.draw, m.real);
    for (const Portal& p : portals)
        (void)scene.set_draw_material(p.draw, p.shown ? p.levels[0] : p.stand_in);
    std::erase_if(d.entries, [&](const auto& e) { return e.second.frame != d.frame; });
    std::erase_if(d.disturbances, [&](const auto& e) { return d.frame - e.second.frame > 600; });
    return status3d(errors.empty(), std::move(errors));
}

Status3D PortalTravel3D::step(EcsWorld& world, WorldPhysicsState3D* physics) {
    _crossings.clear();
    Diagnostics3D errors;
    std::unordered_map<u64, Vec3f> points;
    world.each([&](flecs::entity entity, const PortalTraveller3D& c) {
        if (!c.enabled)
            return;
        const auto* placed = entity.get<WorldTransform3D>();
        const auto* local = entity.get<Transform3D>();
        if (!local)
            return;
        // (A traveller has no parent, most often: its own Transform3D is its place, fresh from this step.)
        const Mat4f world_matrix = entity.parent().is_valid() && placed ? placed->matrix : Mat4f{};
        const Vec3f origin = entity.parent().is_valid() && placed
                                 ? Vec3f{world_matrix.m[12], world_matrix.m[13], world_matrix.m[14]}
                                 : local->position;
        const Vec3f offset{0.0f, c.height, 0.0f};
        Vec3f point = origin + offset;
        if (const auto before = _points.find(entity.id()); before != _points.end())
            if (const auto through = portal_step(world, before->second, point)) {
                const Vec3f turned = through->turn.rotate(offset);
                const Vec3f to = through->position - turned;
                bool carried = false;
                if (physics) {
                    const auto status = physics->carry(entity, to, through->turn);
                    carried = bool(status);
                }
                if (!carried && !entity.parent().is_valid()) {
                    // No body: the transform itself.
                    Transform3D* t = EcsEntity{entity}.get_mut<Transform3D>();
                    t->position = to;
                    t->rotation = (through->turn * t->rotation).normalized();
                    carried = true;
                }
                if (carried) {
                    point = through->position;
                    _crossings.push_back({EcsEntity{entity}, through->portal, through->exit, through->turn});
                } else {
                    errors.push_back("portal travel: an entity under a parent, without a body, cannot be carried");
                }
            }
        points[entity.id()] = point;
    });
    _points = std::move(points);
    return status3d(errors.empty(), std::move(errors));
}

} // namespace kin
