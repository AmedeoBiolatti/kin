#include <kin/anim/player.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <variant>

namespace kin {

// Internal flecs-typed resolver; the public resolve_animation_target (EcsEntity
// in/out) wraps it. Keeps flecs off the kin/anim/player.hpp surface while the
// hot sampling paths below stay flecs-native.
flecs::entity resolve_animation_target_raw(flecs::entity root, AnimationPlayer& player, std::string_view target);

namespace {

constexpr i32 max_advance_steps = 64;

struct AdvanceContext {
    flecs::entity entity;
    AnimationPlayer& player;
    std::unordered_set<std::string> resolving;
    std::vector<AnimationEvent>* events = nullptr;
};

struct ActiveClip {
    const Clip* clip = nullptr;
    NodeCursor* cursor = nullptr;
};

struct PropertySample {
    const PropertyTrack* track = nullptr;
    NodeCursor* cursor = nullptr;
    std::size_t index = 0;
    const PlayerLayer* layer = nullptr;
    flecs::entity target_entity;
};

struct PropertySampleStack {
    std::vector<PropertySample> samples;
};

struct SpriteSample {
    const SpriteTrack* track = nullptr;
    NodeCursor* cursor = nullptr;
    const PlayerLayer* layer = nullptr;
    flecs::entity target_entity;
};

struct QueuedAnimationEvent {
    flecs::entity entity;
    AnimationEvent event;
};

bool replace_base(AnimationPlayer& player, std::shared_ptr<const Animation> animation) {
    if (!animation) {
        return false;
    }
    PlayerLayer layer;
    layer.kind = LayerKind::Base;
    layer.animation = std::move(animation);
    player.layers.clear();
    player.layers.push_back(std::move(layer));
    return true;
}

const std::string* state_animation_name(const AnimationStateMachine& machine, std::string_view state) {
    const auto found = machine.states.find(std::string{state});
    return found != machine.states.end() ? &found->second : nullptr;
}

std::shared_ptr<const Animation> resolve_bound_animation(AnimationPlayer& player, std::string_view animation_name) {
    if (!player.registry) {
        return nullptr;
    }
    std::string resolved_name;
    std::string error;
    if (!substitute(animation_name, player.bindings, resolved_name, error)) {
        return nullptr;
    }
    return player.registry->resolve(resolved_name, player.bindings);
}

bool replace_base_by_name(AnimationPlayer& player, std::string_view animation_name) {
    return replace_base(player, resolve_bound_animation(player, animation_name));
}

bool enter_state(AnimationPlayer& player, const AnimationStateMachine& machine, std::string_view state) {
    const std::string* animation_name = state_animation_name(machine, state);
    if (!animation_name || !replace_base_by_name(player, *animation_name)) {
        return false;
    }
    player.state = std::string{state};
    return true;
}

std::string track_key(std::string_view target, std::size_t index) {
    std::string key{target};
    key.push_back('\0');
    key += std::to_string(index);
    return key;
}

std::string sample_key(std::string_view target, std::string_view property) {
    std::string key{target};
    key.push_back('\0');
    key += property;
    return key;
}

void reset_cursor(NodeCursor& cursor) {
    cursor.time = 0.0f;
    cursor.active_child = 0;
    cursor.remaining = 0;
    cursor.children.clear();
    cursor.child.reset();
    cursor.finished = false;
    cursor.ref_name.clear();
    cursor.ref_animation.reset();
    cursor.relative_base.clear();
    cursor.event_cursor.clear();
}

bool resolve_ref_name(std::string_view name,
                      const Bindings& bindings,
                      const std::unordered_set<std::string>& resolving,
                      std::string& out) {
    std::string error;
    if (!substitute(name, bindings, out, error)) {
        return false;
    }
    return !resolving.contains(out);
}

f32 node_duration(const AnimationNode& node, AnimationRegistry* registry, const Bindings& bindings, std::unordered_set<std::string>& resolving) {
    if (const auto* clip = std::get_if<Clip>(&node.value)) {
        return clip->duration;
    }
    if (const auto* ref = std::get_if<Ref>(&node.value)) {
        std::string resolved_name;
        if (!registry || !resolve_ref_name(ref->name, bindings, resolving, resolved_name)) {
            return 0.0f;
        }
        resolving.insert(resolved_name);
        const auto animation = registry->resolve(resolved_name, bindings);
        const f32 result = animation ? node_duration(animation->root, registry, bindings, resolving) : 0.0f;
        resolving.erase(resolved_name);
        return result;
    }
    if (const auto* sequence = std::get_if<Sequence>(&node.value)) {
        f32 total = 0.0f;
        for (const AnimationNode& child : sequence->children) {
            total += node_duration(child, registry, bindings, resolving);
        }
        return total;
    }
    if (const auto* parallel = std::get_if<Parallel>(&node.value)) {
        if (parallel->children.empty()) {
            return 0.0f;
        }
        if (parallel->end == ParallelEnd::Primary) {
            return parallel->primary >= 0 && parallel->primary < static_cast<i32>(parallel->children.size())
                ? node_duration(parallel->children[static_cast<std::size_t>(parallel->primary)], registry, bindings, resolving)
                : 0.0f;
        }
        f32 result = parallel->end == ParallelEnd::All ? 0.0f : std::numeric_limits<f32>::max();
        for (const AnimationNode& child : parallel->children) {
            const f32 duration = node_duration(child, registry, bindings, resolving);
            result = parallel->end == ParallelEnd::All ? std::max(result, duration) : std::min(result, duration);
        }
        return result == std::numeric_limits<f32>::max() ? 0.0f : result;
    }
    if (const auto* repeat = std::get_if<Repeat>(&node.value)) {
        if (!repeat->child) {
            return 0.0f;
        }
        const f32 child_duration = node_duration(*repeat->child, registry, bindings, resolving);
        return repeat->count > 0 ? child_duration * static_cast<f32>(repeat->count) : child_duration;
    }
    return 0.0f;
}

// Memoized node_duration: caches the result on `cursor` so the per-frame
// sequence/repeat carry-over math no longer re-walks the subtree or re-resolves
// refs through the registry on every step.
f32 cursor_duration(const AnimationNode& node, NodeCursor& cursor, AdvanceContext& ctx) {
    if (cursor.cached_duration < 0.0f) {
        cursor.cached_duration = node_duration(node, ctx.player.registry, ctx.player.bindings, ctx.resolving);
    }
    return cursor.cached_duration;
}

AnimValue add_values(const AnimValue& a, const AnimValue& b) {
    if (a.index() != b.index()) {
        return a;
    }
    switch (value_kind(a)) {
    case AnimValueKind::Float:
        return std::get<f32>(a) + std::get<f32>(b);
    case AnimValueKind::Vec2: {
        const Vec2f av = std::get<Vec2f>(a);
        const Vec2f bv = std::get<Vec2f>(b);
        return Vec2f{av.x + bv.x, av.y + bv.y};
    }
    case AnimValueKind::Color: {
        const Color av = std::get<Color>(a);
        const Color bv = std::get<Color>(b);
        return Color{
            static_cast<u8>(std::clamp(static_cast<i32>(av.r) + static_cast<i32>(bv.r), 0, 255)),
            static_cast<u8>(std::clamp(static_cast<i32>(av.g) + static_cast<i32>(bv.g), 0, 255)),
            static_cast<u8>(std::clamp(static_cast<i32>(av.b) + static_cast<i32>(bv.b), 0, 255)),
            static_cast<u8>(std::clamp(static_cast<i32>(av.a) + static_cast<i32>(bv.a), 0, 255)),
        };
    }
    case AnimValueKind::Int:
        return std::get<i32>(a) + std::get<i32>(b);
    case AnimValueKind::Bool:
    case AnimValueKind::String:
        return a;
    }
    return a;
}

bool is_relative_supported(const AnimValue& value) {
    const AnimValueKind kind = value_kind(value);
    return kind == AnimValueKind::Float
        || kind == AnimValueKind::Vec2
        || kind == AnimValueKind::Color
        || kind == AnimValueKind::Int;
}

bool is_blendable(const AnimValue& value) {
    return is_relative_supported(value);
}

f32 effective_weight(const PlayerLayer& layer) {
    const f32 target = std::max(0.0f, layer.target_weight);
    if (layer.kind != LayerKind::Override || layer.blend_in <= 0.0f) {
        return target;
    }
    return target * std::clamp(layer.age / layer.blend_in, 0.0f, 1.0f);
}

bool blend_values(const std::vector<AnimValue>& values, const std::vector<f32>& weights, AnimValue& out) {
    if (values.empty() || values.size() != weights.size() || !is_blendable(values.front())) {
        return false;
    }
    const AnimValueKind kind = value_kind(values.front());
    f32 total = 0.0f;
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (value_kind(values[i]) != kind || weights[i] <= 0.0f) {
            continue;
        }
        total += weights[i];
    }
    if (total <= 0.0f) {
        return false;
    }

    f32 float_sum = 0.0f;
    Vec2f vec_sum{};
    f32 color_r = 0.0f;
    f32 color_g = 0.0f;
    f32 color_b = 0.0f;
    f32 color_a = 0.0f;
    f32 int_sum = 0.0f;
    bool contributed = false;
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (value_kind(values[i]) != kind || weights[i] <= 0.0f) {
            continue;
        }
        const f32 normalized = weights[i] / total;
        contributed = true;
        switch (kind) {
        case AnimValueKind::Float:
            float_sum += std::get<f32>(values[i]) * normalized;
            break;
        case AnimValueKind::Vec2: {
            const Vec2f v = std::get<Vec2f>(values[i]);
            vec_sum.x += v.x * normalized;
            vec_sum.y += v.y * normalized;
            break;
        }
        case AnimValueKind::Color: {
            const Color c = std::get<Color>(values[i]);
            color_r += static_cast<f32>(c.r) * normalized;
            color_g += static_cast<f32>(c.g) * normalized;
            color_b += static_cast<f32>(c.b) * normalized;
            color_a += static_cast<f32>(c.a) * normalized;
            break;
        }
        case AnimValueKind::Int:
            int_sum += static_cast<f32>(std::get<i32>(values[i])) * normalized;
            break;
        case AnimValueKind::Bool:
        case AnimValueKind::String:
            return false;
        }
    }
    if (!contributed) {
        return false;
    }

    switch (kind) {
    case AnimValueKind::Float:
        out = float_sum;
        return true;
    case AnimValueKind::Vec2:
        out = vec_sum;
        return true;
    case AnimValueKind::Color:
        out = Color{
            static_cast<u8>(std::clamp(std::lround(color_r), 0l, 255l)),
            static_cast<u8>(std::clamp(std::lround(color_g), 0l, 255l)),
            static_cast<u8>(std::clamp(std::lround(color_b), 0l, 255l)),
            static_cast<u8>(std::clamp(std::lround(color_a), 0l, 255l)),
        };
        return true;
    case AnimValueKind::Int:
        out = static_cast<i32>(std::lround(int_sum));
        return true;
    case AnimValueKind::Bool:
    case AnimValueKind::String:
        return false;
    }
    return false;
}

void* component_ptr(flecs::entity entity, flecs::id_t component) {
    return entity.get_mut(component);
}

const void* component_ptr_const(flecs::entity entity, flecs::id_t component) {
    return entity.get(component);
}

flecs::entity find_child_by_name(flecs::entity parent, std::string_view name) {
    if (!parent || name.empty()) {
        return {};
    }

    return parent.lookup(std::string{name}.c_str());
}

flecs::entity resolve_uncached_target(flecs::entity root, std::string_view target) {
    if (!root || target.empty()) {
        return root;
    }

    flecs::entity current = root;
    std::size_t start = 0;
    while (start <= target.size()) {
        const std::size_t slash = target.find('/', start);
        const std::size_t end = slash == std::string_view::npos ? target.size() : slash;
        const std::string_view segment = target.substr(start, end - start);
        if (segment.empty()) {
            return {};
        }
        current = find_child_by_name(current, segment);
        if (!current || !current.is_alive()) {
            return {};
        }
        if (slash == std::string_view::npos) {
            break;
        }
        start = slash + 1;
    }
    return current;
}

void capture_relative_bases(flecs::entity entity,
                            AnimationPlayer& player,
                            const Clip& clip,
                            NodeCursor& cursor,
                            const PropertyRegistry& registry) {
    for (std::size_t i = 0; i < clip.properties.size(); ++i) {
        const PropertyTrack& track = clip.properties[i];
        if (track.space != TrackSpace::Relative) {
            continue;
        }
        const std::string key = track_key(track.target, i);
        if (cursor.relative_base.contains(key)) {
            continue;
        }
        const flecs::entity target_entity = resolve_animation_target_raw(entity, player, track.target);
        if (!target_entity) {
            continue;
        }
        const PropertyAccessor* accessor = registry.find(track.property);
        if (!accessor) {
            continue;
        }
        const void* component = component_ptr_const(target_entity, accessor->component);
        if (!component) {
            continue;
        }
        cursor.relative_base[key] = accessor->get(component);
    }
}

bool advance_node(const AnimationNode& node, NodeCursor& cursor, f32 dt, AdvanceContext& ctx);

void ensure_event_cursor_count(NodeCursor& cursor, std::size_t count) {
    if (cursor.event_cursor.size() != count) {
        cursor.event_cursor.clear();
        cursor.event_cursor.resize(count);
    }
}

AnimationEvent track_event(const EventTrack& track, const AnimationEvent& source) {
    AnimationEvent event = source;
    if (event.target.empty()) {
        event.target = track.target;
    }
    return event;
}

void emit_clip_events(const Clip& clip, NodeCursor& cursor, f32 from, f32 to, AdvanceContext& ctx) {
    ensure_event_cursor_count(cursor, clip.events.size());
    for (std::size_t track_index = 0; track_index < clip.events.size(); ++track_index) {
        const EventTrack& track = clip.events[track_index];
        std::size_t& key_index = cursor.event_cursor[track_index];
        while (key_index < track.keys.size()) {
            const EventKey& key = track.keys[key_index];
            if (key.time > to) {
                break;
            }
            if (key.time == 0.0f || key.time > from) {
                ctx.events->push_back(track_event(track, key.event));
            }
            ++key_index;
        }
    }
}

bool advance_clip(const Clip& clip, NodeCursor& cursor, f32 dt, AdvanceContext& ctx) {
    const f32 before = cursor.time;
    if (cursor.time == 0.0f && ctx.player.properties) {
        capture_relative_bases(ctx.entity, ctx.player, clip, cursor, *ctx.player.properties);
    }
    cursor.time += std::max(0.0f, dt);
    if (ctx.events) {
        emit_clip_events(clip, cursor, before, std::min(cursor.time, clip.duration), ctx);
    }
    cursor.finished = cursor.time >= clip.duration;
    if (cursor.finished) {
        cursor.time = std::max(cursor.time, clip.duration);
    }
    return cursor.finished;
}

bool advance_ref(const Ref& ref, NodeCursor& cursor, f32 dt, AdvanceContext& ctx) {
    if (!ctx.player.registry) {
        cursor.finished = true;
        return true;
    }
    std::string resolved_name;
    if (!resolve_ref_name(ref.name, ctx.player.bindings, ctx.resolving, resolved_name)) {
        cursor.finished = true;
        return true;
    }
    if (cursor.ref_name != resolved_name || !cursor.ref_animation) {
        cursor.ref_name = resolved_name;
        cursor.ref_animation = ctx.player.registry->resolve(resolved_name, ctx.player.bindings);
        cursor.child = std::make_unique<NodeCursor>();
    }
    if (!cursor.ref_animation) {
        cursor.finished = true;
        return true;
    }
    if (!cursor.child) {
        cursor.child = std::make_unique<NodeCursor>();
    }
    ctx.resolving.insert(resolved_name);
    const bool finished = advance_node(cursor.ref_animation->root, *cursor.child, dt, ctx);
    ctx.resolving.erase(resolved_name);
    cursor.finished = finished;
    return finished;
}

void ensure_child_count(NodeCursor& cursor, std::size_t count) {
    if (cursor.children.size() != count) {
        cursor.children.clear();
        cursor.children.resize(count);
        cursor.active_child = 0;
    }
}

bool advance_sequence(const Sequence& sequence, NodeCursor& cursor, f32 dt, AdvanceContext& ctx) {
    ensure_child_count(cursor, sequence.children.size());
    f32 remaining_dt = std::max(0.0f, dt);
    i32 steps = max_advance_steps;
    while (cursor.active_child < static_cast<i32>(sequence.children.size()) && steps-- > 0) {
        const std::size_t index = static_cast<std::size_t>(cursor.active_child);
        const f32 duration = cursor_duration(sequence.children[index], cursor.children[index], ctx);
        const f32 before = duration > 0.0f ? cursor.children[index].time : 0.0f;
        const bool finished = advance_node(sequence.children[index], cursor.children[index], remaining_dt, ctx);
        if (!finished) {
            cursor.finished = false;
            return false;
        }
        remaining_dt = duration > 0.0f ? std::max(0.0f, before + remaining_dt - duration) : 0.0f;
        ++cursor.active_child;
        if (remaining_dt <= 0.0f) {
            break;
        }
    }
    cursor.finished = cursor.active_child >= static_cast<i32>(sequence.children.size());
    return cursor.finished;
}

bool advance_parallel(const Parallel& parallel, NodeCursor& cursor, f32 dt, AdvanceContext& ctx) {
    ensure_child_count(cursor, parallel.children.size());
    if (parallel.children.empty()) {
        cursor.finished = true;
        return true;
    }

    bool any_finished = false;
    bool all_finished = true;
    for (std::size_t i = 0; i < parallel.children.size(); ++i) {
        if (!cursor.children[i].finished) {
            advance_node(parallel.children[i], cursor.children[i], dt, ctx);
        }
        any_finished = any_finished || cursor.children[i].finished;
        all_finished = all_finished && cursor.children[i].finished;
    }

    switch (parallel.end) {
    case ParallelEnd::All:
        cursor.finished = all_finished;
        break;
    case ParallelEnd::Any:
        cursor.finished = any_finished;
        break;
    case ParallelEnd::Primary:
        cursor.finished = parallel.primary >= 0
            && parallel.primary < static_cast<i32>(cursor.children.size())
            && cursor.children[static_cast<std::size_t>(parallel.primary)].finished;
        break;
    }
    return cursor.finished;
}

bool advance_repeat(const Repeat& repeat, NodeCursor& cursor, f32 dt, AdvanceContext& ctx) {
    if (!repeat.child) {
        cursor.finished = true;
        return true;
    }
    if (!cursor.child) {
        cursor.child = std::make_unique<NodeCursor>();
        cursor.remaining = repeat.count == 0 ? -1 : repeat.count;
    }

    f32 remaining_dt = std::max(0.0f, dt);
    i32 steps = max_advance_steps;
    // The child's duration is constant across repetitions; cache it on the repeat
    // cursor (which is never reset between loops, unlike *cursor.child).
    const f32 duration = cursor_duration(*repeat.child, cursor, ctx);
    while (steps-- > 0) {
        const f32 before = duration > 0.0f ? cursor.child->time : 0.0f;
        const bool child_finished = advance_node(*repeat.child, *cursor.child, remaining_dt, ctx);
        if (!child_finished) {
            cursor.finished = false;
            return false;
        }

        remaining_dt = duration > 0.0f ? std::max(0.0f, before + remaining_dt - duration) : 0.0f;
        if (cursor.remaining > 0) {
            --cursor.remaining;
        }
        if (cursor.remaining == 0) {
            cursor.finished = true;
            return true;
        }
        reset_cursor(*cursor.child);
        if (remaining_dt <= 0.0f) {
            cursor.finished = false;
            return false;
        }
    }

    cursor.finished = false;
    return false;
}

bool advance_node(const AnimationNode& node, NodeCursor& cursor, f32 dt, AdvanceContext& ctx) {
    if (cursor.finished) {
        return true;
    }
    if (const auto* clip = std::get_if<Clip>(&node.value)) {
        return advance_clip(*clip, cursor, dt, ctx);
    }
    if (const auto* ref = std::get_if<Ref>(&node.value)) {
        return advance_ref(*ref, cursor, dt, ctx);
    }
    if (const auto* sequence = std::get_if<Sequence>(&node.value)) {
        return advance_sequence(*sequence, cursor, dt, ctx);
    }
    if (const auto* parallel = std::get_if<Parallel>(&node.value)) {
        return advance_parallel(*parallel, cursor, dt, ctx);
    }
    return advance_repeat(std::get<Repeat>(node.value), cursor, dt, ctx);
}

void collect_active_clips(const AnimationNode& node, NodeCursor& cursor, std::vector<ActiveClip>& out) {
    if (const auto* clip = std::get_if<Clip>(&node.value)) {
        out.push_back({.clip = clip, .cursor = &cursor});
        return;
    }
    if (const auto* ref = std::get_if<Ref>(&node.value)) {
        (void)ref;
        if (cursor.ref_animation && cursor.child && !cursor.finished) {
            collect_active_clips(cursor.ref_animation->root, *cursor.child, out);
        }
        return;
    }
    if (const auto* sequence = std::get_if<Sequence>(&node.value)) {
        ensure_child_count(cursor, sequence->children.size());
        if (cursor.active_child >= 0 && cursor.active_child < static_cast<i32>(sequence->children.size())
            && cursor.children.size() == sequence->children.size()) {
            const std::size_t index = static_cast<std::size_t>(cursor.active_child);
            collect_active_clips(sequence->children[index], cursor.children[index], out);
        }
        return;
    }
    if (const auto* parallel = std::get_if<Parallel>(&node.value)) {
        ensure_child_count(cursor, parallel->children.size());
        if (cursor.children.size() != parallel->children.size()) {
            return;
        }
        for (std::size_t i = 0; i < parallel->children.size(); ++i) {
            if (!cursor.children[i].finished) {
                collect_active_clips(parallel->children[i], cursor.children[i], out);
            }
        }
        return;
    }
    const Repeat& repeat = std::get<Repeat>(node.value);
    if (repeat.child && !cursor.child) {
        cursor.child = std::make_unique<NodeCursor>();
        cursor.remaining = repeat.count == 0 ? -1 : repeat.count;
    }
    if (repeat.child && cursor.child && !cursor.finished) {
        collect_active_clips(*repeat.child, *cursor.child, out);
    }
}

AnimValue sampled_value(const PropertyTrack& track, NodeCursor& cursor, std::size_t index) {
    AnimValue value = sample(track, cursor.time);
    if (track.space == TrackSpace::Relative && is_relative_supported(value)) {
        const auto found = cursor.relative_base.find(track_key(track.target, index));
        if (found != cursor.relative_base.end()) {
            value = add_values(value, found->second);
        }
    }
    return value;
}

// Steps one player's state machine. Shared by the standalone
// update_animation_state_machines() (for isolated stepping/tests) and by
// advance_animation_players(), which runs it inline so the per-frame pipeline is
// a single pass over AnimationPlayer rather than two.
void run_state_machine(AnimationPlayer& player) {
    if (!player.machine) {
        return;
    }

    const AnimationStateMachine& machine = *player.machine;
    if (player.state.empty()) {
        if (machine.initial.empty() || !enter_state(player, machine, machine.initial)) {
            return;
        }
    }

    AnimationParams evaluated_params = player.params;
    const AnimTransition* transition = evaluate(machine, player.state, evaluated_params);
    if (!transition) {
        return;
    }

    const std::string* animation_name = state_animation_name(machine, transition->to);
    if (!animation_name) {
        return;
    }

    switch (transition->mode) {
    case TransitionMode::ReplaceBase:
        if (replace_base_by_name(player, *animation_name)) {
            player.state = transition->to;
            player.params = std::move(evaluated_params);
        }
        break;
    case TransitionMode::PushOverride: {
        const std::size_t before = player.layers.size();
        push(player, *animation_name);
        if (player.layers.size() > before) {
            player.params = std::move(evaluated_params);
        }
        break;
    }
    }
}

} // namespace

bool PropertyMask::allows(std::string_view key) const {
    return empty() || std::ranges::find(properties, key) != properties.end();
}

AnimationPlayer make_animation_player(AnimationPlayerConfig config) {
    AnimationPlayer player{
        .registry = config.registry,
        .properties = config.properties,
        .sprite_catalog = config.sprite_catalog,
        .bindings = std::move(config.bindings),
        .machine = std::move(config.machine),
    };
    if (!config.base_animation.empty()) {
        set_base(player, config.base_animation);
    }
    return player;
}

AnimationPlayerSnapshot snapshot(const AnimationPlayer& player) {
    AnimationPlayerSnapshot out{
        .state = player.state,
        .bindings = player.bindings,
        .playing = player.playing,
    };
    out.layers.reserve(player.layers.size());
    for (const PlayerLayer& layer : player.layers) {
        out.layers.push_back(AnimationLayerSnapshot{
            .kind = layer.kind,
            .animation = layer.animation ? layer.animation->name : std::string{},
            .time = layer.cursor.time,
            .speed = layer.speed,
            .weight = effective_weight(layer),
            .done = layer.done,
        });
    }
    out.triggers.reserve(player.params.triggers.size());
    for (const std::string& trigger : player.params.triggers) {
        out.triggers.push_back(trigger);
    }
    std::ranges::sort(out.triggers);
    return out;
}

flecs::entity resolve_animation_target_raw(flecs::entity root, AnimationPlayer& player, std::string_view target) {
    if (!root || target.empty()) {
        return root;
    }

    // Transparent lookup: no temporary std::string is allocated on a cache hit.
    const auto cached = player.target_cache.find(target);
    if (cached != player.target_cache.end()) {
        flecs::entity entity = root.world().entity(cached->second);
        if (entity && entity.is_alive()) {
            return entity;
        }
        player.target_cache.erase(cached);
    }

    flecs::entity resolved = resolve_uncached_target(root, target);
    if (resolved && resolved.is_alive()) {
        player.target_cache.emplace(std::string{target}, resolved.id());
    }
    return resolved;
}

EcsEntity resolve_animation_target(EcsEntity root, AnimationPlayer& player, std::string_view target) {
    return EcsEntity{resolve_animation_target_raw(root.raw(), player, target)};
}

void set_base(AnimationPlayer& player, std::string_view animation_name) {
    if (!player.registry) {
        player.layers.clear();
        return;
    }
    if (!replace_base(player, resolve_bound_animation(player, animation_name))) {
        player.layers.clear();
    }
}

void push(AnimationPlayer& player, std::string_view animation_name, PushOptions options) {
    if (!player.registry) {
        return;
    }
    push(player, resolve_bound_animation(player, animation_name), std::move(options));
}

void push(AnimationPlayer& player, std::shared_ptr<const Animation> animation, PushOptions options) {
    if (!animation) {
        return;
    }
    PlayerLayer layer;
    layer.kind = LayerKind::Override;
    layer.animation = std::move(animation);
    layer.speed = options.speed;
    layer.weight = options.weight;
    layer.target_weight = options.weight;
    layer.blend_in = options.blend_in;
    layer.mask = std::move(options.mask);
    player.layers.push_back(std::move(layer));
}

void clear_overrides(AnimationPlayer& player) {
    if (player.layers.size() > 1) {
        player.layers.erase(player.layers.begin() + 1, player.layers.end());
    }
}

void update_animation_state_machines(EcsWorld& world) {
    world.raw().each([](AnimationPlayer& player) {
        run_state_machine(player);
    });
}

void advance_animation_players(EcsWorld& world, f32 dt) {
    std::vector<QueuedAnimationEvent> queued_events;

    world.raw().each([dt, &queued_events](flecs::entity entity, AnimationPlayer& player) {
        // SM step fused into the advance pass (formerly a separate full-world
        // iteration). Per-player and independent, so interleaving is equivalent.
        run_state_machine(player);
        if (!player.playing || player.layers.empty()) {
            return;
        }

        for (PlayerLayer& layer : player.layers) {
            if (!layer.animation || layer.done) {
                continue;
            }
            std::vector<AnimationEvent> layer_events;
            AdvanceContext ctx{.entity = entity, .player = player, .events = &layer_events};
            const bool finished = advance_node(layer.animation->root, layer.cursor, dt * layer.speed, ctx);
            layer.age += std::max(0.0f, dt);
            layer.weight = effective_weight(layer);
            for (AnimationEvent& event : layer_events) {
                queued_events.push_back({.entity = entity, .event = std::move(event)});
            }
            if (layer.kind == LayerKind::Override && finished) {
                layer.done = true;
            }
        }

        while (player.layers.size() > 1 && player.layers.back().done) {
            player.layers.pop_back();
        }
    });

    for (QueuedAnimationEvent& queued : queued_events) {
        queued.entity.ensure<AnimationEventQueue>().pending.push_back(std::move(queued.event));
    }
}

void sample_animation_players(EcsWorld& world) {
    // Reused per-entity scratch. world.each here is single-threaded, so a single
    // set of buffers cleared per entity keeps capacity and makes the steady state
    // allocation-free (vs. constructing maps/vectors per entity per frame).
    std::vector<ActiveClip> clips;
    std::unordered_map<std::string, PropertySampleStack> property_samples;
    std::unordered_map<std::string, SpriteSample> sprite_samples;
    std::vector<AnimValue> values;
    std::vector<f32> weights;

    world.each([&](flecs::entity entity, AnimationPlayer& player) {
        if (player.layers.empty()) {
            return;
        }

        // Fast path: a single unmasked layer (the common idle/walk base). No
        // cross-layer blending is possible, so write each track straight to its
        // target with no grouping maps or per-property vectors. Multiple active
        // clips (a Parallel base) are processed in order, so a later clip writing
        // the same property wins -- matching the slow path's topmost-wins.
        if (player.layers.size() == 1 && player.layers.front().mask.empty()) {
            PlayerLayer& layer = player.layers.front();
            if (!layer.animation || layer.done) {
                return;
            }
            clips.clear();
            collect_active_clips(layer.animation->root, layer.cursor, clips);
            for (const ActiveClip& active : clips) {
                if (player.properties) {
                    for (std::size_t i = 0; i < active.clip->properties.size(); ++i) {
                        const PropertyTrack& track = active.clip->properties[i];
                        const PropertyAccessor* accessor = player.properties->find(track.property);
                        if (!accessor) {
                            continue;
                        }
                        const flecs::entity target_entity = resolve_animation_target_raw(entity, player, track.target);
                        if (!target_entity) {
                            continue;
                        }
                        void* component = component_ptr(target_entity, accessor->component);
                        if (!component) {
                            continue;
                        }
                        accessor->set(component, sampled_value(track, *active.cursor, i));
                    }
                }
                for (const SpriteTrack& track : active.clip->sprites) {
                    const i32 index = active_sprite_key(track, active.cursor->time);
                    if (index < 0) {
                        continue;
                    }
                    const flecs::entity target_entity = resolve_animation_target_raw(entity, player, track.target);
                    if (!target_entity) {
                        continue;
                    }
                    const SpriteKey& key = track.keys[static_cast<std::size_t>(index)];
                    set_sprite_renderer_sprite(EcsEntity{target_entity}, player.sprite_catalog, key.sprite_id, key.has_pivot, key.pivot);
                }
            }
            return;
        }

        // Slow path: multiple layers and/or masks -> group samples per
        // (target, property) so numeric tracks can blend by weight.
        property_samples.clear();
        sprite_samples.clear();

        for (PlayerLayer& layer : player.layers) {
            clips.clear();
            if (layer.animation && !layer.done) {
                collect_active_clips(layer.animation->root, layer.cursor, clips);
            }
            for (const ActiveClip& active : clips) {
                for (std::size_t i = 0; i < active.clip->properties.size(); ++i) {
                    const PropertyTrack& track = active.clip->properties[i];
                    if (!layer.mask.allows(track.property)) {
                        continue;
                    }
                    flecs::entity target_entity = resolve_animation_target_raw(entity, player, track.target);
                    if (!target_entity) {
                        continue;
                    }
                    property_samples[sample_key(track.target, track.property)].samples.push_back({
                        .track = &track,
                        .cursor = active.cursor,
                        .index = i,
                        .layer = &layer,
                        .target_entity = target_entity,
                    });
                }
                if (layer.mask.empty()) {
                    for (const SpriteTrack& track : active.clip->sprites) {
                        if (active_sprite_key(track, active.cursor->time) >= 0) {
                            flecs::entity target_entity = resolve_animation_target_raw(entity, player, track.target);
                            if (!target_entity) {
                                continue;
                            }
                            sprite_samples[std::string{track.target}] = {
                                .track = &track,
                                .cursor = active.cursor,
                                .layer = &layer,
                                .target_entity = target_entity,
                            };
                        }
                    }
                }
            }
        }

        for (const auto& [property, stack] : property_samples) {
            (void)property;
            if (!player.properties) {
                break;
            }
            if (stack.samples.empty()) {
                continue;
            }
            const PropertySample& topmost = stack.samples.back();
            const PropertyAccessor* accessor = player.properties->find(topmost.track->property);
            if (!accessor) {
                continue;
            }
            void* component = component_ptr(topmost.target_entity, accessor->component);
            if (!component) {
                continue;
            }
            values.clear();
            weights.clear();
            for (const PropertySample& sample_entry : stack.samples) {
                AnimValue value = sampled_value(*sample_entry.track, *sample_entry.cursor, sample_entry.index);
                if (!is_blendable(value)) {
                    values.clear();
                    break;
                }
                values.push_back(std::move(value));
                weights.push_back(effective_weight(*sample_entry.layer));
            }
            AnimValue blended;
            if (!values.empty() && blend_values(values, weights, blended)) {
                accessor->set(component, blended);
            } else if (values.empty()) {
                accessor->set(component, sampled_value(*topmost.track, *topmost.cursor, topmost.index));
            }
        }

        for (const auto& [target, sprite_sample] : sprite_samples) {
            (void)target;
            const i32 index = active_sprite_key(*sprite_sample.track, sprite_sample.cursor->time);
            const SpriteKey& key = sprite_sample.track->keys[static_cast<std::size_t>(index)];
            set_sprite_renderer_sprite(EcsEntity{sprite_sample.target_entity}, player.sprite_catalog, key.sprite_id, key.has_pivot, key.pivot);
        }
    });
}

} // namespace kin
