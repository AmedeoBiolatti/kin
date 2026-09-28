#include <kin/anim/registry.hpp>

#include <kin/anim/anim_format.hpp>
#include <kin/platform/log.hpp>

#include <utility>
#include <variant>

namespace kin {
namespace {

bool substitute_in_place(std::string& text, const Bindings& bindings, std::string& error) {
    std::string out;
    if (!substitute(text, bindings, out, error)) {
        return false;
    }
    text = std::move(out);
    return true;
}

bool instantiate_node(const AnimationNode& source, const Bindings& bindings, AnimationNode& out, std::string& error);
bool validate_sprites(const AnimationNode& node, const SpriteValidator& sprite_validator, std::string& error);

bool instantiate_event(AnimationEvent& event, const Bindings& bindings, std::string& error) {
    return substitute_in_place(event.channel, bindings, error)
        && substitute_in_place(event.name, bindings, error)
        && substitute_in_place(event.value, bindings, error)
        && substitute_in_place(event.target, bindings, error);
}

bool instantiate_clip(const Clip& source, const Bindings& bindings, Clip& out, std::string& error) {
    out = source;
    for (PropertyTrack& track : out.properties) {
        if (!substitute_in_place(track.property, bindings, error) || !substitute_in_place(track.target, bindings, error)) {
            return false;
        }
    }
    for (SpriteTrack& track : out.sprites) {
        if (!substitute_in_place(track.target, bindings, error)) {
            return false;
        }
        for (SpriteKey& key : track.keys) {
            if (!substitute_in_place(key.sprite_id, bindings, error)) {
                return false;
            }
        }
    }
    for (EventTrack& track : out.events) {
        if (!substitute_in_place(track.target, bindings, error)) {
            return false;
        }
        for (EventKey& key : track.keys) {
            if (!instantiate_event(key.event, bindings, error)) {
                return false;
            }
        }
    }
    return true;
}

bool instantiate_node(const AnimationNode& source, const Bindings& bindings, AnimationNode& out, std::string& error) {
    if (const auto* clip = std::get_if<Clip>(&source.value)) {
        Clip copied;
        if (!instantiate_clip(*clip, bindings, copied, error)) {
            return false;
        }
        out = clip_node(std::move(copied));
        return true;
    }
    if (const auto* ref = std::get_if<Ref>(&source.value)) {
        std::string name = ref->name;
        if (!substitute_in_place(name, bindings, error)) {
            return false;
        }
        out = ref_node(std::move(name));
        return true;
    }
    if (const auto* sequence = std::get_if<Sequence>(&source.value)) {
        std::vector<AnimationNode> children;
        children.reserve(sequence->children.size());
        for (const AnimationNode& child : sequence->children) {
            AnimationNode child_out;
            if (!instantiate_node(child, bindings, child_out, error)) {
                return false;
            }
            children.push_back(std::move(child_out));
        }
        out = sequence_node(std::move(children));
        return true;
    }
    if (const auto* parallel = std::get_if<Parallel>(&source.value)) {
        std::vector<AnimationNode> children;
        children.reserve(parallel->children.size());
        for (const AnimationNode& child : parallel->children) {
            AnimationNode child_out;
            if (!instantiate_node(child, bindings, child_out, error)) {
                return false;
            }
            children.push_back(std::move(child_out));
        }
        out = parallel_node(std::move(children), parallel->end);
        std::get<Parallel>(out.value).primary = parallel->primary;
        return true;
    }

    const Repeat& repeat = std::get<Repeat>(source.value);
    if (!repeat.child) {
        Repeat copied;
        copied.count = repeat.count;
        out = AnimationNode{.value = std::move(copied)};
        return true;
    }
    AnimationNode child_out;
    if (!instantiate_node(*repeat.child, bindings, child_out, error)) {
        return false;
    }
    out = repeat_node(std::move(child_out), repeat.count);
    return true;
}

bool validate_clip_sprites(const Clip& clip, const SpriteValidator& sprite_validator, std::string& error) {
    for (const SpriteTrack& track : clip.sprites) {
        for (const SpriteKey& key : track.keys) {
            if (!sprite_validator(key.sprite_id)) {
                error = "missing sprite '" + key.sprite_id + "'";
                return false;
            }
        }
    }
    return true;
}

bool validate_sprites(const AnimationNode& node, const SpriteValidator& sprite_validator, std::string& error) {
    if (!sprite_validator) {
        return true;
    }
    if (const auto* clip = std::get_if<Clip>(&node.value)) {
        return validate_clip_sprites(*clip, sprite_validator, error);
    }
    if (std::holds_alternative<Ref>(node.value)) {
        return true;
    }
    if (const auto* sequence = std::get_if<Sequence>(&node.value)) {
        for (const AnimationNode& child : sequence->children) {
            if (!validate_sprites(child, sprite_validator, error)) {
                return false;
            }
        }
        return true;
    }
    if (const auto* parallel = std::get_if<Parallel>(&node.value)) {
        for (const AnimationNode& child : parallel->children) {
            if (!validate_sprites(child, sprite_validator, error)) {
                return false;
            }
        }
        return true;
    }
    const Repeat& repeat = std::get<Repeat>(node.value);
    return !repeat.child || validate_sprites(*repeat.child, sprite_validator, error);
}

} // namespace

bool substitute(std::string_view text, const Bindings& bindings, std::string& out, std::string& error) {
    out.clear();
    std::size_t pos = 0;
    while (pos < text.size()) {
        const std::size_t open = text.find('{', pos);
        if (open == std::string_view::npos) {
            out.append(text.substr(pos));
            break;
        }

        out.append(text.substr(pos, open - pos));
        const std::size_t close = text.find('}', open + 1);
        if (close == std::string_view::npos) {
            error = "unresolved placeholder in string";
            return false;
        }

        const std::string key{text.substr(open + 1, close - open - 1)};
        const auto found = bindings.find(key);
        if (found == bindings.end()) {
            error = "unresolved placeholder {" + key + "}";
            return false;
        }

        out += found->second;
        pos = close + 1;
    }

    if (out.find('{') != std::string::npos || out.find('}') != std::string::npos) {
        error = "unresolved placeholder in string";
        return false;
    }
    return true;
}

bool instantiate(const Animation& tmpl, const Bindings& bindings, Animation& out, std::string& error) {
    error.clear();

    std::string name;
    if (!substitute(tmpl.name, bindings, name, error)) {
        return false;
    }

    AnimationNode root;
    if (!instantiate_node(tmpl.root, bindings, root, error)) {
        return false;
    }

    out = Animation{
        .name = std::move(name),
        .root = std::move(root),
        .is_template = false,
    };

    if (!validate(out, error)) {
        return false;
    }
    return true;
}

void AnimationRegistry::add(Animation concrete) {
    concrete.is_template = false;
    const std::string name = concrete.name;
    _concrete[name] = std::make_shared<Animation>(std::move(concrete));
    KIN_LOG_DEBUG_F("animation",
                    "animation registered",
                    (LogFields{{.name = "name", .value = name}, {.name = "template", .value = "false"}}));
    bump_generation();
}

void AnimationRegistry::add_template(Animation tmpl) {
    tmpl.is_template = true;
    const std::string name = tmpl.name;
    _templates[name] = std::make_shared<Animation>(std::move(tmpl));
    KIN_LOG_DEBUG_F("animation",
                    "animation template registered",
                    (LogFields{{.name = "name", .value = name}, {.name = "template", .value = "true"}}));
    bump_generation();
}

std::shared_ptr<const Animation> AnimationRegistry::resolve(std::string_view name, const Bindings& bindings) {
    const std::string key{name};
    if (const auto found = _concrete.find(key); found != _concrete.end()) {
        return found->second;
    }
    if (const auto found = _cache.find(key); found != _cache.end()) {
        return found->second;
    }

    for (const auto& [template_name, tmpl] : _templates) {
        std::string concrete_name;
        std::string error;
        if (!substitute(template_name, bindings, concrete_name, error)) {
            continue;
        }
        if (concrete_name != key) {
            continue;
        }

        Animation instantiated;
        if (!instantiate(*tmpl, bindings, instantiated, error)) {
            KIN_LOG_ERROR_F("animation",
                            "animation template resolve failed",
                            (LogFields{{.name = "name", .value = key},
                                       {.name = "template", .value = template_name},
                                       {.name = "error", .value = error}}));
            return nullptr;
        }
        auto stored = std::make_shared<Animation>(std::move(instantiated));
        _cache[key] = stored;
        KIN_LOG_DEBUG_F("animation",
                        "animation template resolved",
                        (LogFields{{.name = "name", .value = key}, {.name = "template", .value = template_name}}));
        return stored;
    }

    KIN_LOG_DEBUG_F("animation", "animation resolve miss", (LogFields{{.name = "name", .value = key}}));
    return nullptr;
}

bool AnimationRegistry::expand(std::string_view template_name,
                               const Bindings& bindings,
                               std::string& error,
                               SpriteValidator sprite_validator) {
    const auto found = _templates.find(std::string{template_name});
    if (found == _templates.end()) {
        error = "animation template not found";
        KIN_LOG_WARN_F("animation",
                       "animation template not found",
                       (LogFields{{.name = "template", .value = std::string{template_name}},
                                  {.name = "reason", .value = "missing_template"}}));
        return false;
    }

    Animation instantiated;
    if (!instantiate(*found->second, bindings, instantiated, error)) {
        KIN_LOG_ERROR_F("animation",
                        "animation template expand failed",
                        (LogFields{{.name = "template", .value = std::string{template_name}},
                                   {.name = "error", .value = error}}));
        return false;
    }
    if (!validate_sprites(instantiated.root, sprite_validator, error)) {
        KIN_LOG_WARN_F("animation",
                       "animation sprite validation failed",
                       (LogFields{{.name = "template", .value = std::string{template_name}},
                                  {.name = "name", .value = instantiated.name},
                                  {.name = "error", .value = error}}));
        return false;
    }

    const std::string concrete_name = instantiated.name;
    _concrete[concrete_name] = std::make_shared<Animation>(std::move(instantiated));
    KIN_LOG_INFO_F("animation",
                   "animation template expanded",
                   (LogFields{{.name = "template", .value = std::string{template_name}},
                              {.name = "name", .value = concrete_name}}));
    bump_generation();
    return true;
}

void AnimationRegistry::merge(const AnimationRegistryFragment& fragment) {
    const std::size_t concrete_count = fragment.animations.size();
    const std::size_t template_count = fragment.animation_templates.size();
    for (const Animation& animation : fragment.animations) {
        Animation copied = clone(animation);
        copied.is_template = false;
        const std::string name = copied.name;
        _concrete[name] = std::make_shared<Animation>(std::move(copied));
    }
    for (const Animation& animation : fragment.animation_templates) {
        Animation copied = clone(animation);
        copied.is_template = true;
        const std::string name = copied.name;
        _templates[name] = std::make_shared<Animation>(std::move(copied));
    }
    if (!fragment.animations.empty() || !fragment.animation_templates.empty()) {
        KIN_LOG_INFO_F("animation",
                       "animation registry fragment merged",
                       (LogFields{{.name = "path", .value = fragment.source_path.string()},
                                  {.name = "count", .value = std::to_string(concrete_count)},
                                  {.name = "templates", .value = std::to_string(template_count)}}));
        bump_generation();
    }
}

void AnimationRegistry::bump_generation() {
    ++_generation;
    _cache.clear();
    KIN_LOG_DEBUG_F("animation",
                    "animation registry generation bumped",
                    (LogFields{{.name = "generation", .value = std::to_string(_generation)},
                               {.name = "reason", .value = "registry_changed"}}));
}

} // namespace kin
