#include <kin/ecs/render3d.hpp>
#include <kin/physics3d/terrain_shape3d.hpp>
#include <kin/physics3d/world3d.hpp>
#include <kin/scene/terrains3d.hpp>
#include <unordered_map>

namespace kin {

struct Terrains3D::State {
    Renderer3D* renderer = nullptr;
    std::filesystem::path content;
    SurfaceShader3D surface;
    bool surface_tried = false;
    struct Entry {
        Terrain3D terrain;
        std::string file; // the asset it was loaded from
        Vec3f base{};     // the file's origin (the entity's position moves it)
        Body3D body{};
        World3D* physics = nullptr;
        bool collision_dirty = true;
        Terrain3D::Changed changed{}; // the rectangle to update in place (when not made again)
        bool seen = false;
    };
    std::unordered_map<u64, Entry> entries;

    void drop_body(Entry& e) {
        if (e.physics && e.body.valid())
            (void)e.physics->destroy_body(e.body);
        e.body = {};
        e.physics = nullptr;
    }
};

Terrains3D::Terrains3D(Renderer3D& renderer, std::filesystem::path content, SurfaceShader3D surface)
    : _s(std::make_unique<State>()) {
    _s->renderer = &renderer;
    _s->content = std::move(content);
    _s->surface = std::move(surface);
}

Terrains3D::~Terrains3D() {
    if (_s)
        for (auto& [id, e] : _s->entries)
            _s->drop_body(e);
}

Status3D Terrains3D::update(EcsWorld& world, Scene3D& scene, World3D* physics) {
    State& s = *_s;
    Diagnostics3D errors;
    for (auto& [id, e] : s.entries)
        e.seen = false;
    world.each([&](flecs::entity entity, const TerrainRenderer3D& c) {
        if (!c.enabled || c.terrain.id.empty())
            return;
        auto it = s.entries.find(entity.id());
        if (it == s.entries.end() || it->second.file != c.terrain.id) {
            if (!s.surface.valid() && !s.surface_tried) {
                s.surface_tried = true;
                auto compiled = compile_terrain_surface(*s.renderer);
                if (compiled)
                    s.surface = *compiled;
                else
                    errors.insert(errors.end(), compiled.diagnostics.begin(), compiled.diagnostics.end());
            }
            if (!s.surface.valid())
                return;
            auto loaded = Terrain3D::load(*s.renderer, s.surface, s.content / c.terrain.id);
            if (!loaded) {
                errors.insert(errors.end(), loaded.diagnostics.begin(), loaded.diagnostics.end());
                return;
            }
            if (it != s.entries.end())
                s.drop_body(it->second);
            State::Entry e;
            e.base = loaded->desc().origin;
            e.terrain = std::move(*loaded);
            e.file = c.terrain.id;
            it = s.entries.insert_or_assign(entity.id(), std::move(e)).first;
        }
        State::Entry& e = it->second;
        e.seen = true;
        const auto* placed = entity.get<WorldTransform3D>();
        const Vec3f origin = e.base + (placed ? placed->position() : Vec3f{});
        if (origin != e.terrain.desc().origin) {
            e.terrain.set_origin(origin);
            e.collision_dirty = true;
        }
        const u32 pick = c.id ? c.id : static_cast<u32>(entity.id() & 0xFFFFFFFFu);
        if (auto drawn = e.terrain.draw(scene, pick); !drawn)
            errors.insert(errors.end(), drawn.diagnostics.begin(), drawn.diagnostics.end());
        // Collision: a static height field in this physics world.
        if (!c.collision || !physics) {
            s.drop_body(e);
            return;
        }
        if (e.physics != physics) {
            s.drop_body(e);
            e.collision_dirty = true;
        }
        if (!e.collision_dirty && !e.changed.empty() && e.body.valid()) {
            if (!update_terrain_collision(*physics, e.body, e.terrain, e.changed))
                e.collision_dirty = true;
            e.changed = {};
        }
        if (e.collision_dirty) {
            const Shape3D shape = terrain_shape3d(e.terrain);
            if (e.body.valid())
                (void)physics->set_shape(e.body, shape);
            else
                e.body = physics->create_body({.shape = shape, .motion = Body3DMotion::Static, .user_id = entity.id()});
            e.physics = physics;
            e.collision_dirty = false;
        }
    });
    for (auto it = s.entries.begin(); it != s.entries.end();)
        if (!it->second.seen) {
            s.drop_body(it->second);
            it = s.entries.erase(it);
        } else {
            ++it;
        }
    return status3d(errors.empty(), std::move(errors));
}

Terrain3D* Terrains3D::terrain(EcsEntity entity) {
    const auto it = _s->entries.find(entity.raw().id());
    return it == _s->entries.end() ? nullptr : &it->second.terrain;
}

void Terrains3D::heights_changed(EcsEntity entity, Terrain3D::Changed changed) {
    const auto it = _s->entries.find(entity.raw().id());
    if (it == _s->entries.end() || changed.empty())
        return;
    Terrain3D::Changed& c = it->second.changed;
    if (c.empty()) {
        c = changed;
        return;
    }
    const Vec2i lo{std::min(c.at.x, changed.at.x), std::min(c.at.y, changed.at.y)};
    const Vec2i hi{std::max(c.at.x + c.size.x, changed.at.x + changed.size.x),
                   std::max(c.at.y + c.size.y, changed.at.y + changed.size.y)};
    c = {lo, {hi.x - lo.x, hi.y - lo.y}};
}

void Terrains3D::heights_changed(EcsEntity entity) {
    if (const auto it = _s->entries.find(entity.raw().id()); it != _s->entries.end())
        it->second.collision_dirty = true;
}

std::size_t Terrains3D::size() const {
    return _s->entries.size();
}

} // namespace kin
