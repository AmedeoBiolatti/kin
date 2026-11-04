#include <kin/scripting/script_reload.hpp>
#include <kin/scripting/workflows2d.hpp>
namespace kin {
std::unique_ptr<DevelopmentChange> prepare_script_reload(std::unique_ptr<ScriptEngine>& active,
                                                         const DevelopmentRevision& revision,
                                                         const std::filesystem::path& script, std::string& error) {
    if (!active) {
        error = "there is no active script";
        return {};
    }
    struct State {
        std::unique_ptr<ScriptEngine> next;
        bool suspended = false, swapped = false;
    };
    auto state = std::make_shared<State>();
    state->next = active->prepare_reload(revision.content->root(), script, error);
    if (!state->next)
        return {};
    return std::make_unique<DevelopmentChange>(DevelopmentChange{[&active, state](std::string& failure) {
                                                                     active->suspend_systems();
                                                                     state->suspended = true;
                                                                     if (!state->next->activate_systems()) {
                                                                         failure = state->next->last_error();
                                                                         return false;
                                                                     }
                                                                     std::swap(active, state->next);
                                                                     state->swapped = true;
                                                                     return true;
                                                                 },
                                                                 [&active, state] {
                                                                     if (!state->suspended)
                                                                         return;
                                                                     if (state->swapped) {
                                                                         active->suspend_systems();
                                                                         std::swap(active, state->next);
                                                                     } else
                                                                         state->next->suspend_systems();
                                                                     if (!active->activate_systems())
                                                                         std::terminate();
                                                                     state->suspended = false;
                                                                 },
                                                                 [&active, revision] {
                                                                     if (auto services = active->workflows())
                                                                         services->use_content(revision.content);
                                                                 }});
}
} // namespace kin
