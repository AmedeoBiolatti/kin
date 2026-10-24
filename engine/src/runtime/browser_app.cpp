#include <algorithm>
#include <emscripten.h>
#include <emscripten/html5.h>
#include <kin/runtime/scene_app.hpp>
#include <memory>
#include <stdexcept>
#include <string>

namespace kin {
namespace {
std::unique_ptr<SceneAppSession> active;
double last_frame = 0;

void browser_error(const char* message) {
    KIN_LOG_ERROR("web", message);
    EM_ASM({ Module.onAbort ?.(UTF8ToString($0)); }, message);
}

EM_BOOL frame(double time, void*) {
    if (!active)
        return EM_FALSE;
    try {
        auto* driver = active->windowed();
        double width = 0, height = 0;
        emscripten_get_element_css_size("#canvas", &width, &height);
        const Vec2i requested{std::max(1, int(width)), std::max(1, int(height))};
        if (requested != driver->window().size())
            driver->window().set_size(requested);
        const bool background = EM_ASM_INT({
            const skip = !!Module.kinSkipElapsed;
            Module.kinSkipElapsed = false;
            return skip;
        });
        const double elapsed = last_frame == 0 || background ? 0 : (time - last_frame) / 1000;
        last_frame = time;
        if (active->step(static_cast<f32>(elapsed))) {
            if (!active->scenes().empty())
                EM_ASM({
                    if (!Module.kinReady) {
                        Module.kinReady = true;
                        Module.onGameReady ?.();
                    }
                });
            return EM_TRUE;
        }
        if (active->exit_code() != 0)
            browser_error("The game reported a failure; see the browser console.");
        else
            EM_ASM({ Module.onGameFinished ?.(); });
    } catch (const std::exception& error) {
        browser_error(error.what());
    } catch (...) {
        browser_error("The browser game stopped unexpectedly.");
    }
    active.reset();
    return EM_FALSE;
}

} // namespace

// Called by the pagehide handler while the old canvas and SDL event targets live.
extern "C" EMSCRIPTEN_KEEPALIVE void kin_browser_shutdown() {
    active.reset();
}

int run_scene_app(SceneAppConfig config, std::unique_ptr<SceneManager> scenes, std::shared_ptr<void> keep_alive) {
    if (active)
        throw std::logic_error("Only one browser scene session may run at a time");
    try {
        const std::string title(config.window.title);
        auto session = std::make_unique<SceneAppSession>(std::move(config), std::move(scenes), std::move(keep_alive));
        if (session->finished())
            return session->exit_code();
        if (session->windowed()->app().headless())
            throw std::invalid_argument("Browser scene sessions require windowed mode");
        EM_ASM(
            {
                document.title = UTF8ToString($0);
                Module.canvas.setAttribute('aria-label', document.title);
                document.addEventListener(
                    'visibilitychange', function() {
                        if (document.hidden)
                            Module.kinSkipElapsed = true;
                    });
            },
            title.c_str());
        active = std::move(session);
        last_frame = 0;
        EM_ASM({
            globalThis.addEventListener('pagehide', function() { Module['_kin_browser_shutdown'](); }, {once : true});
            globalThis.addEventListener(
                'pageshow', function(event) {
                    if (event.persisted)
                        location.reload();
                });
        });
        emscripten_request_animation_frame_loop(frame, nullptr);
        return 0;
    } catch (const std::exception& error) {
        browser_error(error.what());
        return 1;
    }
}
} // namespace kin
