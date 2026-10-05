#pragma once

// Draw tracing for the render probe (--probe-render): which entity or piece of
// code drew what, and where on screen. Everything here exists only in builds
// with KIN_ENABLE_RENDER_PROBE; without it the macros below expand to nothing.
//
// Code that draws without going through the ECS render components can say
// what it is drawing, so the probe can name it:
//
//     KIN_DRAW_SCOPE("hud.minimap");          // draws below are the minimap's
//     KIN_DRAW_ENTITY(entity, "Box");         // draws below are entity's Box
//                                             // (kin/ecs/render.hpp)

#define KIN_DRAW_TRACE_CONCAT_INNER(a, b) a##b
#define KIN_DRAW_TRACE_CONCAT(a, b) KIN_DRAW_TRACE_CONCAT_INNER(a, b)

#ifdef KIN_ENABLE_RENDER_PROBE

#include <kin/core/types.hpp>
#include <kin/renderer/color.hpp>

#include <atomic>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace kin {

enum class DrawKind : u8 {
    Fill,
    Outline,
    Line,
    Texture,
    Gradient,
    Shader,
};

std::string_view draw_kind_name(DrawKind kind);

// One draw that reached the screen (draws into render targets are not traced).
struct DrawRecord {
    Rectf bounds{};    // window pixels: the frame the render probe reads back
    Rectf region{};    // the texture region drawn, for Texture draws
    u64 texture = 0;   // texture identity; 0 for none
    Color color{};
    f32 rotation = 0.0f;
    u32 source = 0;    // DrawTrace source id; 0 when nothing claimed the draw
    DrawKind kind = DrawKind::Fill;
};

struct DrawSourceInfo {
    u64 entity = 0;        // 0 for a named scope
    std::string name;      // the entity's name (or "#id"), or the scope's label
    std::string component; // e.g. "SpriteRenderer"; empty for a scope
};

class DrawTrace {
public:
    // Sources, ids from 1, stable for the trace's life. Safe from any thread.
    u32 scope_source(std::string_view label);
    // `name()` is called only the first time the entity is seen.
    template<typename Name>
    u32 entity_source(u64 world, u64 entity, std::string_view component, Name&& name) {
        std::lock_guard lock{_mutex};
        const auto found = _entities.find(EntityKeyView{world, entity, component});
        if (found != _entities.end()) {
            return found->second;
        }
        _sources.push_back({.entity = entity, .name = std::string{name()}, .component = std::string{component}});
        const u32 id = static_cast<u32>(_sources.size());
        _entities.emplace(EntityKey{world, entity, std::string{component}}, id);
        return id;
    }
    // A copy of the source's details; empty (entity 0, no name) when unknown.
    DrawSourceInfo source_info(u32 id) const;

    // The current frame's draws (main thread).
    void begin_frame() { _draws.clear(); }
    void record(const DrawRecord& record) { _draws.push_back(record); }
    std::span<const DrawRecord> draws() const { return _draws; }

    // Sources of the instances of the next draw_sprites() call, which draws
    // sprites from several entities at once.
    void set_instance_sources(std::span<const u32> sources) { _instance_sources.assign(sources.begin(), sources.end()); }
    std::span<const u32> instance_sources() const { return _instance_sources; }
    void clear_instance_sources() { _instance_sources.clear(); }

private:
    struct EntityKey {
        u64 world = 0;
        u64 entity = 0;
        std::string component;
    };
    struct EntityKeyView {
        u64 world = 0;
        u64 entity = 0;
        std::string_view component;
    };
    struct KeyHash {
        using is_transparent = void;
        std::size_t operator()(const EntityKeyView& key) const;
        std::size_t operator()(const EntityKey& key) const {
            return (*this)(EntityKeyView{key.world, key.entity, key.component});
        }
    };
    struct KeyEqual {
        using is_transparent = void;
        static EntityKeyView view(const EntityKey& key) { return {key.world, key.entity, key.component}; }
        static EntityKeyView view(const EntityKeyView& key) { return key; }
        template<typename A, typename B>
        bool operator()(const A& a, const B& b) const {
            const EntityKeyView x = view(a);
            const EntityKeyView y = view(b);
            return x.world == y.world && x.entity == y.entity && x.component == y.component;
        }
    };

    mutable std::mutex _mutex;
    std::vector<DrawSourceInfo> _sources; // id - 1
    std::unordered_map<EntityKey, u32, KeyHash, KeyEqual> _entities;
    std::unordered_map<std::string, u32> _scopes;
    std::vector<DrawRecord> _draws;
    std::vector<u32> _instance_sources;
};

namespace draw_trace_detail {
// Inline so that the checks on hot paths (per sprite, per entity) are a load,
// not a call.
inline std::atomic<DrawTrace*> active{nullptr};
inline thread_local u32 current = 0;
} // namespace draw_trace_detail

// The trace draws are recorded into, or null (the usual case: no probe running).
inline DrawTrace* active_draw_trace() {
    return draw_trace_detail::active.load(std::memory_order_acquire);
}
inline void set_active_draw_trace(DrawTrace* trace) {
    draw_trace_detail::active.store(trace, std::memory_order_release);
}

// The source this thread's draws and queued commands belong to (0: none).
inline u32 current_draw_source() {
    return draw_trace_detail::current;
}

// Makes `source` this thread's current draw source for its scope; 0 leaves the
// current one alone.
class DrawSourceScope {
public:
    explicit DrawSourceScope(u32 source) {
        if (source != 0) {
            _previous = draw_trace_detail::current;
            draw_trace_detail::current = source;
            _set = true;
        }
    }
    ~DrawSourceScope() {
        if (_set) {
            draw_trace_detail::current = _previous;
        }
    }

    DrawSourceScope(const DrawSourceScope&) = delete;
    DrawSourceScope& operator=(const DrawSourceScope&) = delete;

private:
    u32 _previous = 0;
    bool _set = false;
};

// The source id of a named scope in the active trace (0 when none is active).
u32 draw_scope_source(std::string_view label);

} // namespace kin

#define KIN_DRAW_SCOPE(label)                                                                          \
    const ::kin::DrawSourceScope KIN_DRAW_TRACE_CONCAT(kin_draw_scope_, __LINE__) {                    \
        ::kin::draw_scope_source(label)                                                                \
    }

#else

#define KIN_DRAW_SCOPE(label) static_cast<void>(0)

#endif
