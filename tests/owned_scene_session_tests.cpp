#include <SDL3/SDL.h>
#include <array>
#include <cassert>
#include <kin/assets/asset_server.hpp>
#include <kin/l10n/localization.hpp>
#include <kin/runtime/scene_app.hpp>
#include <memory>
#include <sstream>
#include <stdexcept>

namespace {
struct Counts {
    int enter = 0, exit = 0, update = 0, render = 0, destroyed = 0, shutdown = 0;
};
struct Resources {
    std::shared_ptr<Counts> counts = std::make_shared<Counts>();
    kin::Localization language;
    std::ostringstream report;
};
class OwnedScene final : public kin::Scene {
public:
    OwnedScene(Counts& counts, bool fail = false) : _counts(counts), _fail(fail) {}
    ~OwnedScene() override {
        assert(SDL_WasInit(SDL_INIT_VIDEO));
        ++_counts.destroyed;
    }
    std::string_view name() const override { return "owned"; }
    void on_enter(kin::SceneContext& ctx) override {
        ++_counts.enter;
        constexpr std::array<kin::u8, 4> pixel{255, 255, 255, 255};
        _texture = ctx.renderer.create_texture_from_rgba(pixel.data(), {1, 1});
        assert(_texture);
    }
    void update(kin::SceneContext&) override { ++_counts.update; }
    void render(kin::SceneContext& ctx) override {
        ++_counts.render;
        ctx.renderer.clear();
        ctx.renderer.draw_texture(_texture, {0, 0, 8, 8});
    }
    void on_exit(kin::SceneContext& ctx) override {
        assert(SDL_WasInit(SDL_INIT_VIDEO));
        ctx.renderer.draw_texture(_texture, {0, 0, 1, 1});
        ++_counts.exit;
        if (_fail)
            throw std::runtime_error("expected scene exit failure");
    }

private:
    Counts& _counts;
    bool _fail;
    kin::Texture _texture;
};
std::unique_ptr<kin::SceneAppSession> make_session(const std::shared_ptr<Resources>& resources,
                                                   bool exit_failure = false, bool shutdown_failure = false) {
    // These original config/title/game variables all die before the first frame.
    std::string title = "owned scene runtime";
    kin::GameInfo game{.id = "owned-session", .title = title};
    auto scenes = std::make_unique<kin::SceneManager>();
    scenes->push(std::make_unique<OwnedScene>(*resources->counts, exit_failure));
    kin::SceneAppConfig config{.window = {.title = title,
                                          .width = 32,
                                          .height = 32,
                                          .mode = kin::AppMode::Headless,
                                          .fixed_dt = 0.125f,
                                          .shutdown =
                                              [counts = resources->counts, shutdown_failure](auto&) {
                                                  ++counts->shutdown;
                                                  if (shutdown_failure)
                                                      throw std::runtime_error("expected user shutdown failure");
                                              }},
                               .headless = {.seed = 42},
                               .game = &game,
                               .report_output = &resources->report,
                               .render_headless = true,
                               .localization = &resources->language,
                               .pipeline_record_path = std::filesystem::path{}};
    return std::make_unique<kin::SceneAppSession>(std::move(config), std::move(scenes), resources);
}
void owned_lifetime() {
    kin::Localization prior;
    kin::set_active_localization(&prior);
    auto resources = std::make_shared<Resources>();
    auto counts = resources->counts;
    std::weak_ptr<Resources> weak = resources;
    {
        auto session = make_session(resources);
        resources.reset();
        assert(!weak.expired());
        assert(session->step(0.125f) && session->step(0.125f));
        assert(counts->enter == 1 && counts->update == 1 && counts->render == 2);
        session->scenes().replace(std::make_unique<OwnedScene>(*counts));
        assert(session->step(0.125f));
        assert(counts->enter == 2 && counts->exit == 1 && counts->destroyed == 1);
        assert(session->finish() == 0 && session->finish() == 0 && !session->step(0.125f));
        assert(counts->exit == 2 && counts->destroyed == 2 && counts->shutdown == 1);
        auto retained = weak.lock();
        assert(retained->report.str().find("\"seed\": 42") != std::string::npos);
        assert(retained->report.str().find("\"frames\": 3") != std::string::npos);
    }
    assert(weak.expired() && kin::active_localization() == &prior);
    kin::set_active_localization(nullptr);
}
void failure_cleanup(bool user_failure) {
    auto resources = std::make_shared<Resources>();
    auto counts = resources->counts;
    auto session = make_session(resources, !user_failure, user_failure);
    session->step(0.125f);
    bool threw = false;
    try {
        session->finish();
    } catch (const std::runtime_error&) {
        threw = true;
    }
    assert(threw && session->exit_code() == 1 && session->scenes().empty());
    assert(counts->exit == 1 && counts->destroyed == 1 && counts->shutdown == 1);
    session.reset();
    assert(counts->exit == 1);
}
void cooperative_assets() {
    kin::JobSystem jobs{{.single_threaded = true}};
    kin::AssetManager manager;
    kin::AssetServer assets(manager, {.jobs = &jobs});
    assets.register_async_loader<int>([](const auto&, auto&) { return 42; });
    auto value = assets.load_async<int>("answer");
    assert(!value);
    assets.pump(kin::AssetServer::PumpMode::Budgeted);
    assert(value && *value == 42);
}
} // namespace
int main() {
    owned_lifetime();
    failure_cleanup(false);
    failure_cleanup(true);
    cooperative_assets();
}
