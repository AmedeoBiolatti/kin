#pragma once

#include <kin/anim/animation.hpp>
#include <kin/anim/blend_space.hpp>

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

namespace kin {

using Bindings = std::unordered_map<std::string, std::string>;
using SpriteValidator = std::function<bool(std::string_view sprite_id)>;

struct AnimationRegistryFragment;

bool substitute(std::string_view text, const Bindings& bindings, std::string& out, std::string& error);
bool instantiate(const Animation& tmpl, const Bindings& bindings, Animation& out, std::string& error);

class AnimationRegistry {
public:
    void add(Animation concrete);
    void add_template(Animation tmpl);

    std::shared_ptr<const Animation> resolve(std::string_view name, const Bindings& bindings = {});
    bool expand(std::string_view template_name,
                const Bindings& bindings,
                std::string& error,
                SpriteValidator sprite_validator = {});
    void merge(const AnimationRegistryFragment& fragment);
    // Blend spaces (blend_space.hpp), played by name as animations are.
    void add_blend_space(std::string name, AnimationBlendSpace space);
    std::shared_ptr<const AnimationBlendSpace> blend_space(std::string_view name) const;

    u64 generation() const { return _generation; }
    void bump_generation();

private:
    std::unordered_map<std::string, std::shared_ptr<const Animation>> _concrete;
    std::unordered_map<std::string, std::shared_ptr<const Animation>> _templates;
    std::unordered_map<std::string, std::shared_ptr<const Animation>> _cache;
    std::unordered_map<std::string, std::shared_ptr<const AnimationBlendSpace>> _blend_spaces;
    u64 _generation = 0;
};

} // namespace kin
