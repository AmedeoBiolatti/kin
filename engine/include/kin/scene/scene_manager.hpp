#pragma once

#include <kin/scene/scene.hpp>

#include <memory>
#include <vector>

namespace kin {

class SceneManager {
public:
    void push(std::unique_ptr<Scene> scene);
    void pop();
    void replace(std::unique_ptr<Scene> scene);
    void clear();

    void update(SceneContext& ctx);
    void render(SceneContext& ctx);

    // Apply queued push/pop/replace/clear commands immediately, running the
    // associated on_enter/on_exit hooks without updating active scenes. Used by
    // the server to realize the initial stack and to rebuild on reset.
    void flush_pending(SceneContext& ctx);

    // Tear down all scenes synchronously. Intended for application shutdown,
    // before renderer/device destruction, so scene-owned resources release in
    // a valid backend lifetime.
    void shutdown(SceneContext& ctx);

    bool empty() const { return _stack.empty(); }
    i32 depth() const { return static_cast<i32>(_stack.size()); }
    const Scene* top() const;
    const Scene* at(i32 index) const;
    Scene* top_mut();
    Scene* at_mut(i32 index);
    InputActionContext action_context() const;
    std::vector<AvailableInputAction> available_actions(const InputMap& map) const;

private:
    enum class CommandType {
        Push,
        Pop,
        Replace,
        Clear,
    };

    struct Command {
        CommandType type = CommandType::Pop;
        std::unique_ptr<Scene> scene;
    };

    void apply_pending(SceneContext& ctx);

    std::vector<std::unique_ptr<Scene>> _stack;
    std::vector<Command> _pending;
};

} // namespace kin
