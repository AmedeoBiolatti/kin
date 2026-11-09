#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
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
        (std::string_view(stem) == "planar_reflection" ? out.reflection : out.window) = *made;
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
        (std::string_view(stem) == "planar_reflection" ? out.reflection : out.window) = *made;
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
    Renderer3D& renderer;
    PlanarSurfaces3D surfaces;
    Mesh3D quad, box;
    struct Entry {
        std::deque<View3D>
            views; // a mirror's one, a portal's a level each (a deque: a level's view stays put as more come)
        ViewOptions3D options;
        u64 frame = 0;
    };
    std::unordered_map<EcsId, Entry> entries;
    u64 frame = 0;
    u32 rendered = 0, skipped = 0;
    explicit Data(Renderer3D& r, PlanarSurfaces3D s) : renderer(r), surfaces(std::move(s)) {}

    // Keeps an entity's views for another frame (it is still there, if off screen).
    void keep(EcsId id) {
        if (auto it = entries.find(id); it != entries.end())
            it->second.frame = frame;
    }
    // The entity's `k`th view, `size`, with `options`: resized when its size changed, made again with its options.
    View3D* view(EcsId id, u32 k, Vec2i size, const ViewOptions3D& options) {
        Entry& e = entries[id];
        e.frame = frame;
        if (e.options.hdr != options.hdr || e.options.samples != options.samples ||
            e.options.background != options.background) {
            e.views.clear();
            e.options = options;
        }
        while (e.views.size() <= k) {
            auto made = renderer.create_view(size, options);
            if (!made)
                return nullptr;
            e.views.push_back(std::move(*made));
        }
        View3D& v = e.views[k];
        if (v.texture().size() != size && !renderer.resize(v, size))
            return nullptr;
        return &v;
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

Status3D PlanarViews3D::render(EcsWorld& world, Scene3D& scene, Vec2i view_size, const ViewOptions3D& like) {
    Data& d = *_data;
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

    // What is half-way through a portal, drawn at the exit too (portal_straddle): every copy of the scene's draws whose
    // bounds straddle an enabled portal's opening, plain, skinned (a character walking through: its pose as it is) or a
    // batch's, with the copy's id, so pick names it (Scene3D::draw_again). Neither half is cut by the pictures' clip
    // planes (DrawClip3D): cut, a half would show its hollow rim where the two meet. The image is drawn whole; the copy
    // itself is drawn again, whole, by the pictures only (an opaque one: a see-through one would show twice), the cut
    // one staying as it is. A skinned copy's bounds are its mesh's at rest (a character's T-pose: its widest); a
    // batch's groups far from every opening are passed over whole.
    {
        std::vector<std::pair<PortalPose3D, PortalPose3D>> pairs;
        world.each([&](flecs::entity entity, const Portal3D& c) {
            if (c.enabled)
                if (auto poses = portal_poses(EcsEntity{entity}))
                    pairs.push_back(*poses);
        });
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
        for (std::size_t d = 0; !pairs.empty() && d < draws.size(); ++d) {
            const Scene3D::Draw& draw = draws[d];
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
                for (const auto& [entry, exit] : pairs)
                    if (const auto through = portal_straddle(entry, exit, centre, b.radius() * stretch)) {
                        images.push_back({d, i, *through * m, DrawClip3D::whole,
                                          copy.history_id ? copy.history_id ^ 0x9e3779b97f4a7c15ull : 0});
                        if (draw.clip == DrawClip3D::camera &&
                            (draw.material.blend == Blend3D::Opaque || draw.material.blend == Blend3D::Mask))
                            images.push_back({d, i, m, DrawClip3D::whole_if_cut,
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
        const auto bounds = planar_on_screen(eye, aspect, at->position, at->rotation, m.size);
        if (bounds && d.surfaces.reflection) {
            // Only where the mirror shows (its ripples read a little round it: the margin), as sharp as the view.
            const PlanarPicture3D picture = planar_picture(*bounds, frame_size(c.resolution));
            m.view = d.view(m.id, 0, picture.size, options);
            if (m.view) {
                m.seen = mirror_view(eye, plane_through(at->position, at->rotation.rotate({0, 0, 1})), aspect);
                crop_planar_view(m.seen, picture.window, f32(picture.size.x) / f32(picture.size.y), true);
                m.real.surface = d.surfaces.reflection;
                m.real.emissive_texture = m.view->texture();
                planar_view_params(m.real.shader_params, m.seen.projector, c.distortion,
                                   std::clamp(c.strength, 0.0f, 1.0f));
                // Water you see into: what is below the surface, in a picture of the same place, in its base colour
                // map. Its murk is a height fog thickening fast under the surface (level water: its base height the
                // surface's), about two thirds gone looking straight down `murk_depth` deep.
                if (c.refraction > 0.0f && c.metallic < 1.0f)
                    m.below_view = d.view(m.id, 1, picture.size, options);
                if (m.below_view) {
                    m.below = refraction_view(eye, plane_through(at->position, at->rotation.rotate({0, 0, 1})), aspect);
                    crop_planar_view(m.below, picture.window, f32(picture.size.x) / f32(picture.size.y), false);
                    m.real.texture = m.below_view->texture();
                    planar_refraction_params(m.real.shader_params, m.below.projector, c.refraction_distortion,
                                             std::clamp(c.refraction, 0.0f, 1.0f));
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
            Camera3D from = eye; // the level before's whole frame (portal_chain made each level from it)
            Rectf within{0, 0, 1, 1};
            while (p.shown < p.chain.size()) {
                const auto bounds = planar_on_screen(from, aspect, p.pose.position, p.pose.rotation, p.pose.size);
                const auto seen = bounds ? intersect(*bounds, within) : std::nullopt;
                if (!seen)
                    break;
                const PlanarPicture3D picture = planar_picture(*seen, frame);
                View3D* view = d.view(p.id, p.shown, picture.size, options);
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
            planar_view_params(level.shader_params, p.chain[k].projector);
            p.levels.push_back(std::move(level));
        }
        // A box behind the opening, so that a camera a little way into it still sees through.
        p.draw = draw(d.box, portal_surface_transform(p.pose, 0.25f), p.stand_in, p.id);
        portals.push_back(std::move(p));
    });
    if (!errors.empty())
        return status3d(false, std::move(errors));

    // The pictures, each with the planar surfaces as stand-ins, but for a portal's own next level; without the overlay.
    const bool overlay = scene.show_overlay;
    scene.show_overlay = false;
    const auto picture = [&](View3D& view, const Camera3D& camera) {
        scene.camera = camera;
        if (!d.renderer.render(view, scene)) {
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
        if (m.view && !picture(*m.view, m.seen.camera))
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
    return status3d(errors.empty(), std::move(errors));
}

} // namespace kin
