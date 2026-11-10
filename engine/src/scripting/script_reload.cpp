#include <kin/scripting/script_reload.hpp>

#include <kin/scripting/workflows2d.hpp>

namespace kin {

bool activate_script(std::unique_ptr<ScriptEngine>& active, std::unique_ptr<ScriptEngine>& next, std::string& error) {
    active->suspend_systems();
    if (!next->activate_systems()) {
        error = next->last_error();
        return false;
    }
    std::swap(active, next);
    return true;
}

void rollback_script(std::unique_ptr<ScriptEngine>& active, std::unique_ptr<ScriptEngine>& next, bool swapped) {
    if (swapped) {
        std::swap(active, next);
    }
    next->suspend_systems();
    if (!active->activate_systems()) {
        active->halt("reload rollback could not restore the script's systems: " + active->last_error());
    }
}

std::unique_ptr<DevelopmentChange> prepare_script_reload(std::unique_ptr<ScriptEngine>& active,
                                                         const DevelopmentRevision& revision,
                                                         const std::filesystem::path& script, std::string& error) {
    if (!active) {
        error = "there is no active script";
        return {};
    }
    struct State {
        std::unique_ptr<ScriptEngine> next;
        bool attempted = false, swapped = false;
    };
    auto state = std::make_shared<State>();
    state->next = active->prepare_reload(revision.content->root(), script, error);
    if (!state->next) {
        return {};
    }
    auto activate = [&active, state](std::string& failure) {
        state->attempted = true;
        state->swapped = activate_script(active, state->next, failure);
        return state->swapped;
    };
    auto rollback = [&active, state] {
        if (state->attempted) {
            rollback_script(active, state->next, state->swapped);
            state->attempted = state->swapped = false;
        }
    };
    auto finalize = [&active, revision] {
        if (auto services = active->workflows()) {
            services->use_content(revision.content);
        }
    };
    return std::make_unique<DevelopmentChange>(
        DevelopmentChange{std::move(activate), std::move(rollback), std::move(finalize)});
}

} // namespace kin
