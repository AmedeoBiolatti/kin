#include <emscripten.h>
#include <exception>

int run_summit_tests(int argc, char** argv);
int run_summit_expedition_tests();

namespace {
int suite = 0;

void run(void*) {
    try {
        char name[] = "summit_tests", verbose[] = "-v";
        char* args[] = {name, verbose};
        const int result = suite == 0 ? run_summit_tests(2, args) : run_summit_expedition_tests();
        EM_ASM({ Module.kinSimulation = ({completed : $0, passed : !$1}); }, suite + 1, result);
        if (result != 0)
            return;
        if (++suite < 2)
            emscripten_async_call(run, nullptr, 0);
        else
            EM_ASM({ Module.onGameReady ?.(); });
    } catch (const std::exception& error) {
        EM_ASM(
            {
                const error = UTF8ToString($0);
                Module.kinSimulation = ({completed : $1, passed : false, error});
                Module.onAbort ?.(error);
            },
            error.what(), suite);
    }
}
} // namespace

int main() {
    // Yield once so the loading UI is visible before the CPU-only suites start.
    emscripten_async_call(run, nullptr, 0);
    return 0;
}
