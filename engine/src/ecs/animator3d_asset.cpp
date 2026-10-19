#include <algorithm>
#include <cmath>
#include <kin/anim/anim_format.hpp>
#include <kin/assets/content.hpp>
#include <kin/ecs/scene3d.hpp>
#include <stdexcept>
#include <unordered_set>

namespace kin {
namespace {

void require(bool condition, const std::string& message) {
    if (!condition)
        throw std::runtime_error(message);
}
void fields(const JsonValue& value, std::initializer_list<std::string_view> names) {
    require(value.is_object(), "expected object");
    for (const auto& [key, ignored] : value.members()) {
        (void)ignored;
        require(std::find(names.begin(), names.end(), key) != names.end(), "unknown field '" + key + "'");
    }
}
std::string string(const JsonValue& value, std::string_view key, std::string_view fallback = {}) {
    const auto* member = value.find(key);
    require(!member || member->is_string(), "expected string '" + std::string(key) + "'");
    return member ? member->as_string() : std::string(fallback);
}
f32 number(const JsonValue& value, std::string_view key, f32 fallback, f32 min, f32 max) {
    const auto* member = value.find(key);
    require(!member || (member->is_number() && std::isfinite(member->as_number()) && member->as_number() >= min &&
                        member->as_number() <= max),
            "invalid number '" + std::string(key) + "'");
    return member ? f32(member->as_number()) : fallback;
}
bool boolean(const JsonValue& value, std::string_view key, bool fallback) {
    const auto* member = value.find(key);
    require(!member || member->is_bool(), "expected bool '" + std::string(key) + "'");
    return member ? member->as_bool() : fallback;
}
const JsonValue::Array& array(const JsonValue& value, std::string_view key, size_t maximum) {
    const auto* member = value.find(key);
    static const JsonValue::Array empty;
    require(!member || (member->is_array() && member->items().size() <= maximum),
            "invalid/oversized array '" + std::string(key) + "'");
    return member ? member->items() : empty;
}
std::vector<std::string> names(const JsonValue& value, std::string_view key) {
    std::vector<std::string> result;
    for (const auto& name : array(value, key, 256)) {
        require(name.is_string() && !name.as_string().empty(), "expected joint name in '" + std::string(key) + "'");
        result.push_back(name.as_string());
    }
    return result;
}

std::shared_ptr<const AnimatorConfiguration3D> load(const std::filesystem::path& path,
                                                    const std::filesystem::path& root) {
    const auto bytes = content_file_size(path);
    require(bytes && *bytes <= 1024 * 1024, "configuration missing or exceeds 1 MiB");
    auto text = read_content_text(path);
    require(text.has_value(), "cannot read configuration");
    require(text->size() <= 1024 * 1024, "configuration exceeds 1 MiB");
    auto parsed = parse_json(*text);
    require(parsed.ok(), "invalid JSON: " + parsed.error);
    const auto& json = *parsed.value;
    fields(json, {"schema", "machine", "blend_spaces", "layers", "fades", "ik", "events", "actions"});
    require(string(json, "schema") == "kin.animator3d/1", "unsupported animator schema");
    auto config = std::make_shared<AnimatorConfiguration3D>();
    config->source = std::move(*text);
    if (const auto* machine = json.find("machine")) {
        fields(*machine, {"asset", "name"});
        const auto asset = string(*machine, "asset");
        require(!asset.empty(), "machine requires an asset");
        const auto machine_bytes = content_file_size(root / asset);
        require(machine_bytes && *machine_bytes <= 1024 * 1024, "machine missing or exceeds 1 MiB: '" + asset + "'");
        auto machine_text = read_content_text(root / asset);
        config->machine_asset = (root / asset).lexically_normal();
        require(machine_text.has_value(), "cannot read machine '" + asset + "'");
        require(machine_text->size() <= 1024 * 1024, "machine exceeds 1 MiB");
        AnimationRegistryFragment fragment;
        std::string error;
        const bool parsed_machine = parse_animation_registry_fragment(*machine_text, fragment, error);
        require(parsed_machine, "machine '" + asset + "': " + error);
        auto name = string(*machine, "name");
        if (name.empty() && fragment.state_machines.size() == 1)
            name = fragment.state_machines.begin()->first;
        require(fragment.state_machines.contains(name), "machine name '" + name + "' not found in '" + asset + "'");
        config->machine = std::make_shared<AnimationStateMachine>(std::move(fragment.state_machines.at(name)));
        config->machine_source = std::move(*machine_text);
        const auto& graph = *config->machine;
        require(!graph.states.empty() && graph.states.contains(graph.initial), "machine initial state is undefined");
        for (const auto& transition : graph.transitions) {
            require((transition.from.empty() || graph.states.contains(transition.from)) &&
                        graph.states.contains(transition.to),
                    "machine transition has undefined state");
            require(transition.mode == TransitionMode::ReplaceBase, "3D machines support replace transitions only");
            for (const auto& condition : transition.when)
                require(!condition.param.empty() && std::isfinite(condition.threshold), "invalid machine condition");
        }
    }
    if (const auto* spaces = json.find("blend_spaces")) {
        require(spaces->is_object() && spaces->members().size() <= 64, "invalid/oversized blend_spaces");
        for (const auto& [state, value] : spaces->members()) {
            fields(value, {"parameter", "entries"});
            require(!config->machine || config->machine->states.contains(state),
                    "blend space has undefined state '" + state + "'");
            Animator3D::BlendSpace space{.parameter = string(value, "parameter")};
            require(!space.parameter.empty(), "blend space requires a parameter");
            for (const auto& entry : array(value, "entries", 256)) {
                fields(entry, {"clip", "position"});
                space.entries.push_back({string(entry, "clip"), number(entry, "position", 0, -1e6f, 1e6f)});
                require(!space.entries.back().clip.empty(), "blend entry requires a clip");
            }
            require(!space.entries.empty(), "blend space requires entries");
            std::ranges::sort(space.entries, {}, &Animator3D::BlendEntry::position);
            for (size_t i = 1; i < space.entries.size(); ++i)
                require(space.entries[i - 1].position < space.entries[i].position, "duplicate blend positions");
            config->blend_spaces.emplace(state, std::move(space));
        }
    }
    for (const auto& value : array(json, "layers", 64)) {
        fields(value, {"clip", "mask_joint", "include_root", "weight", "speed", "loop", "additive"});
        config->layers.push_back({.clip = string(value, "clip"),
                                  .mask_joint = string(value, "mask_joint"),
                                  .include_root = boolean(value, "include_root", true),
                                  .weight = number(value, "weight", 1, 0, 1),
                                  .speed = number(value, "speed", 1, -1000, 1000),
                                  .loop = boolean(value, "loop", true),
                                  .additive = boolean(value, "additive", false)});
        require(!config->layers.back().clip.empty(), "layer requires a clip");
    }
    for (const auto& value : array(json, "fades", 256)) {
        fields(value, {"from", "to", "seconds"});
        Animator3D::Fade fade{string(value, "from"), string(value, "to"), number(value, "seconds", .2f, 0, 3600)};
        require(!config->machine || ((fade.from.empty() || config->machine->states.contains(fade.from)) &&
                                     (fade.to.empty() || config->machine->states.contains(fade.to))),
                "fade has undefined state");
        config->fades.push_back(std::move(fade));
    }
    auto event_tracks = std::make_shared<AnimationEventTracks3D>();
    size_t marker_count = 0;
    std::unordered_set<std::string> event_clips;
    for (const auto& value : array(json, "events", 64)) {
        fields(value, {"clip", "markers", "source"});
        ClipEventTrack3D track{.clip = string(value, "clip")};
        const auto source = string(value, "source", "base");
        if (source == "action")
            track.source = MarkerSource3D::Action;
        else if (source == "both")
            track.source = MarkerSource3D::Both;
        else
            require(source == "base", "unknown event source");
        require(!track.clip.empty() && event_clips.insert(track.clip + "|" + source).second,
                "event clip/source missing or duplicated");
        for (const auto& marker : array(value, "markers", 256)) {
            fields(marker, {"phase", "channel", "name", "value"});
            AnimationMarker3D key{.phase = number(marker, "phase", -1, 0, 1),
                                  .event = {.channel = string(marker, "channel"),
                                            .name = string(marker, "name"),
                                            .value = string(marker, "value")}};
            require(marker.find("phase") && key.phase >= 0 && key.phase < 1, "event phase must be in [0,1)");
            require(!key.event.channel.empty() && !key.event.name.empty(), "event requires channel and name");
            require(key.event.channel.size() <= 95 && key.event.name.size() <= 95 && key.event.value.size() <= 255,
                    "event string exceeds limit");
            require(++marker_count <= 256, "animator exceeds 256 markers");
            track.markers.push_back(std::move(key));
        }
        std::stable_sort(track.markers.begin(), track.markers.end(),
                         [](const auto& a, const auto& b) { return a.phase < b.phase; });
        event_tracks->push_back(std::move(track));
    }
    if (!event_tracks->empty())
        config->events = std::move(event_tracks);
    auto actions = std::make_shared<CharacterActions3D>();
    std::unordered_set<std::string> action_names;
    for (const auto& value : array(json, "actions", 32)) {
        fields(value, {"name", "clip", "mask_joint", "include_root", "speed", "weight", "fade_in", "fade_out",
                       "additive", "base_events"});
        CharacterAction3D action{
            .name = string(value, "name"), .clip = string(value, "clip"), .mask_joint = string(value, "mask_joint")};
        require(!action.name.empty() && action.name.size() <= 95 && action_names.insert(action.name).second,
                "action name missing, oversized or duplicated");
        require(!action.clip.empty() && action.clip.size() <= 95 && action.mask_joint.size() <= 95,
                "action clip/joint missing or oversized");
        auto& o = action.playback;
        o.speed = number(value, "speed", 1, .001f, 1000);
        o.weight = number(value, "weight", 1, .001f, 1);
        o.fade_in = number(value, "fade_in", .08f, 0, 3600);
        o.fade_out = number(value, "fade_out", .08f, 0, 3600);
        o.include_root = boolean(value, "include_root", true);
        o.additive = boolean(value, "additive", false);
        o.base_events = boolean(value, "base_events", true);
        actions->push_back(std::move(action));
    }
    if (!actions->empty())
        config->actions = std::move(actions);
    std::unordered_set<std::string> constraints;
    for (const auto& value : array(json, "ik", 64)) {
        fields(value, {"name", "kind", "joints", "weight", "fade_seconds", "pole", "max_stretch", "foot_heights",
                       "toe_joints", "toe_heights", "foot_smooth_seconds", "toe_max_tilt_degrees"});
        IKConstraint3D ik;
        ik.name = string(value, "name");
        require(!ik.name.empty() && constraints.insert(ik.name).second, "IK name missing or duplicated");
        const auto kind = string(value, "kind");
        if (kind == "two_bone")
            ik.kind = IKKind3D::TwoBone;
        else if (kind == "foot_placer")
            ik.kind = IKKind3D::FootPlacer;
        else if (kind == "fabrik")
            ik.kind = IKKind3D::Fabrik;
        else if (kind == "look_at")
            ik.kind = IKKind3D::LookAt;
        else
            require(false, "unknown IK kind '" + kind + "'");
        ik.joints = names(value, "joints");
        require((ik.kind == IKKind3D::TwoBone && ik.joints.size() == 3) ||
                    (ik.kind == IKKind3D::FootPlacer && ik.joints.size() >= 4 && (ik.joints.size() - 1) % 3 == 0) ||
                    (ik.kind == IKKind3D::Fabrik && ik.joints.size() >= 2) ||
                    (ik.kind == IKKind3D::LookAt && !ik.joints.empty()),
                "invalid IK chain size");
        ik.weight = number(value, "weight", 1, 0, 1);
        ik.fade_seconds = number(value, "fade_seconds", 0, 0, 3600);
        ik.two_bone.max_stretch = number(value, "max_stretch", 1, 1, 10);
        if (const auto* pole = value.find("pole")) {
            std::string error;
            auto vector = component_field_value_from_json(ComponentFieldKind::Vec3f, *pole, error);
            require(vector.has_value(), "invalid IK pole: " + error);
            ik.pole = IKConstraint3D::Pole::ModelDirection;
            ik.pole_vector = std::get<Vec3f>(*vector);
        }
        ik.toe_joints = names(value, "toe_joints");
        const auto heights = [&](std::string_view key, std::vector<f32>& out) {
            for (const auto& height : array(value, key, 256)) {
                require(height.is_number() && std::isfinite(height.as_number()) && height.as_number() >= 0 &&
                            height.as_number() <= 10,
                        "invalid IK height");
                out.push_back(f32(height.as_number()));
            }
        };
        heights("foot_heights", ik.foot_heights);
        heights("toe_heights", ik.toe_heights);
        ik.foot.smooth_seconds = number(value, "foot_smooth_seconds", ik.foot.smooth_seconds, 0, 3600);
        ik.foot.toe_max_tilt_degrees = number(value, "toe_max_tilt_degrees", ik.foot.toe_max_tilt_degrees, 0, 180);
        config->ik.push_back(std::move(ik));
    }
    return config;
}

void validate_model(const AnimatorConfiguration3D& config, const Model3D& model) {
    const auto clip = [&](const std::string& name) {
        require(name.empty() || model.find_clip(name), "missing model clip '" + name + "'");
    };
    if (config.machine)
        for (const auto& [state, name] : config.machine->states)
            if (!config.blend_spaces.contains(state))
                clip(name);
    for (const auto& [state, space] : config.blend_spaces) {
        (void)state;
        for (const auto& entry : space.entries)
            clip(entry.clip);
    }
    const auto joint = [&](const std::string& name) {
        require(name.empty() || (!model.skeletons.empty() && model.skeletons[0].find(name) >= 0),
                "missing model joint '" + name + "'");
    };
    if (config.events)
        for (const auto& track : *config.events)
            clip(track.clip);
    if (config.actions)
        for (const auto& action : *config.actions) {
            clip(action.clip);
            joint(action.mask_joint);
            require(model.find_clip(action.clip)->duration > 0, "action clip has no duration");
        }
    for (const auto& layer : config.layers) {
        clip(layer.clip);
        joint(layer.mask_joint);
    }
    for (const auto& ik : config.ik) {
        for (const auto& name : ik.joints)
            joint(name);
        for (const auto& name : ik.toe_joints)
            joint(name);
    }
}

} // namespace

std::vector<std::string> resolve_animator3d_assets(EcsWorld& world, const std::filesystem::path& content_root,
                                                   std::span<const EcsId> selected) {
    std::vector<EcsEntity> entities;
    if (selected.empty())
        world.raw().each([&](flecs::entity entity, const AnimatorAsset3D&) { entities.emplace_back(entity); });
    else
        for (auto id : selected)
            if (auto e = world.entity_by_id(id); e && e.has<AnimatorAsset3D>())
                entities.push_back(e);
    std::ranges::sort(entities, {}, &EcsEntity::id);
    std::unordered_map<std::string, std::shared_ptr<const AnimatorConfiguration3D>> loaded;
    std::vector<std::string> errors;
    for (auto entity : entities) {
        const auto asset = entity.get<AnimatorAsset3D>()->asset.id;
        if (asset.empty())
            continue;
        try {
            const auto path = (content_root / asset).lexically_normal();
            auto& config = loaded[path.string()];
            if (!config)
                config = load(path, content_root);
            if (const auto* skin = entity.get<SkinnedMeshRenderer3D>(); skin && skin->model)
                validate_model(*config, *skin->model);
            const auto previous = entity.get<AnimatorAsset3D>()->configuration;
            if (previous && previous->source == config->source && previous->machine_source == config->machine_source) {
                config = previous;
                continue;
            }
            Animator3D animator;
            if (const auto* old = entity.get<Animator3D>())
                animator = *old;
            animator.machine = config->machine;
            animator.blend_spaces = config->blend_spaces;
            animator.layers = config->layers;
            animator.fades = config->fades;
            animator.event_tracks = config->events;
            animator.events.clear();
            animator.actions = config->actions;
            auto constraints = config->ik;
            for (auto& constraint : constraints)
                for (const auto& old : animator.ik_constraints)
                    if (constraint.name == old.name)
                        constraint.ground = old.ground;
            animator.ik_constraints = std::move(constraints);
            animator.player = AnimationPlayer3D{};
            animator.bound = nullptr;
            animator.applied.clear();
            animator.applied_layers.clear();
            animator.ik_pose = {};
            animator.ik_world.clear();
            animator.ik_dt = 0;
            animator.pose_override = {};
            animator.state.clear();
            entity.set(static_cast<const Animator3D&>(animator));
            entity.get_mut<AnimatorAsset3D>()->configuration = config;
        } catch (const std::exception& error) {
            errors.push_back("AnimatorAsset3D entity '" + entity.name() + "' (" + std::to_string(entity.id()) +
                             ") asset '" + asset + "': " + error.what());
        }
    }
    return errors;
}

} // namespace kin
