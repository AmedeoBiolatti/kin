#include <kin/scene/scene_manager.hpp>

namespace kin {
namespace {

i32 active_start_index(const std::vector<std::unique_ptr<Scene>>& stack) {
    if (stack.empty()) {
        return 0;
    }

    i32 start = static_cast<i32>(stack.size()) - 1;
    while (start > 0 && stack[static_cast<std::size_t>(start)]->updates_below()) {
        --start;
    }
    return start;
}

} // namespace

void SceneManager::push(std::unique_ptr<Scene> scene) {
    if (scene) {
        _pending.push_back({.type = CommandType::Push, .scene = std::move(scene)});
    }
}

void SceneManager::pop() {
    _pending.push_back({.type = CommandType::Pop});
}

void SceneManager::replace(std::unique_ptr<Scene> scene) {
    if (scene) {
        _pending.push_back({.type = CommandType::Replace, .scene = std::move(scene)});
    }
}

void SceneManager::clear() {
    _pending.push_back({.type = CommandType::Clear});
}

void SceneManager::update(SceneContext& ctx) {
    if (_stack.empty()) {
        apply_pending(ctx);
        return;
    }

    const i32 start = active_start_index(_stack);
    const i32 top_index = static_cast<i32>(_stack.size()) - 1;
    for (i32 i = start; i <= top_index; ++i) {
        ctx.is_top = (i == top_index);
        _stack[static_cast<std::size_t>(i)]->update(ctx);
    }

    apply_pending(ctx);
}

void SceneManager::render(SceneContext& ctx) {
    if (_stack.empty()) {
        return;
    }

    i32 start = static_cast<i32>(_stack.size()) - 1;
    while (start > 0 && _stack[static_cast<std::size_t>(start)]->is_overlay()) {
        --start;
    }

    const i32 top_index = static_cast<i32>(_stack.size()) - 1;
    for (i32 i = start; i <= top_index; ++i) {
        ctx.is_top = (i == top_index);
        _stack[static_cast<std::size_t>(i)]->render(ctx);
    }
}

const Scene* SceneManager::top() const {
    return _stack.empty() ? nullptr : _stack.back().get();
}

const Scene* SceneManager::at(i32 index) const {
    if (index < 0 || index >= depth()) {
        return nullptr;
    }
    return _stack[static_cast<std::size_t>(index)].get();
}

Scene* SceneManager::top_mut() {
    return _stack.empty() ? nullptr : _stack.back().get();
}

Scene* SceneManager::at_mut(i32 index) {
    if (index < 0 || index >= depth()) {
        return nullptr;
    }
    return _stack[static_cast<std::size_t>(index)].get();
}

InputActionContext SceneManager::action_context() const {
    InputActionContext result{"scenes"};
    if (_stack.empty()) {
        return result;
    }

    const i32 start = active_start_index(_stack);
    const i32 top_index = static_cast<i32>(_stack.size()) - 1;
    for (i32 i = start; i <= top_index; ++i) {
        const Scene& scene = *_stack[static_cast<std::size_t>(i)];
        InputActionContext scene_context{scene.name()};
        scene.collect_actions(scene_context);
        if (!scene_context.empty()) {
            result.include(scene_context);
        }
    }
    return result;
}

std::vector<AvailableInputAction> SceneManager::available_actions(const InputMap& map) const {
    return action_context().resolve(map);
}

void SceneManager::flush_pending(SceneContext& ctx) {
    apply_pending(ctx);
}

void SceneManager::shutdown(SceneContext& ctx) {
    _pending.clear();
    while (!_stack.empty()) {
        _stack.back()->on_exit(ctx);
        _pending.clear();
        _stack.pop_back();
    }
}

void SceneManager::apply_pending(SceneContext& ctx) {
    // Lifecycle hooks (on_enter/on_exit/on_resume/on_suspend) receive the
    // SceneContext and may enqueue further transitions. Drain into a local
    // batch each pass so those hooks can push to _pending without invalidating
    // the range we are iterating, and process the newly enqueued commands on
    // the next loop iteration instead of dropping them.
    while (!_pending.empty()) {
        std::vector<Command> batch = std::move(_pending);
        _pending.clear();
        for (auto& command : batch) {
            switch (command.type) {
            case CommandType::Push:
                if (!_stack.empty()) {
                    _stack.back()->on_suspend(ctx);
                }
                _stack.push_back(std::move(command.scene));
                _stack.back()->on_enter(ctx);
                break;

            case CommandType::Pop:
                if (!_stack.empty()) {
                    _stack.back()->on_exit(ctx);
                    _stack.pop_back();
                    if (!_stack.empty()) {
                        _stack.back()->on_resume(ctx);
                    }
                }
                break;

            case CommandType::Replace:
                if (!_stack.empty()) {
                    _stack.back()->on_exit(ctx);
                    _stack.pop_back();
                }
                _stack.push_back(std::move(command.scene));
                _stack.back()->on_enter(ctx);
                break;

            case CommandType::Clear:
                while (!_stack.empty()) {
                    _stack.back()->on_exit(ctx);
                    _stack.pop_back();
                }
                break;
            }
        }
    }
}

} // namespace kin
