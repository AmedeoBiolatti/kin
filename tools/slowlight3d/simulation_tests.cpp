#include "simulation.hpp"

#include <iostream>
#include <stdexcept>
#include <utility>
using namespace slowlight;
namespace {
void check(bool ok, const char* why) {
    if (!ok)
        throw std::runtime_error(why);
}
void near(double a, double b, double tolerance, const char* why) {
    check(std::abs(a - b) < tolerance, why);
}
} // namespace
int main() {
    try {
        near(10 / default_speed, .36, 1e-12, "100 km/h: ten metres must take .36 seconds");
        History h;
        const auto steady = [](double) { return Sample{{0, 10, 0, 65}, {100, 5, 0, 0}}; };
        h.seek(2, steady);
        const auto light = evaluate(h, {}, {0, 10, 0}, default_speed);
        check(light.known && light.energy > 0, "steady light should arrive");
        near(light.delay, .72, 2e-6, "both ten-metre transport legs must be delayed");
        const auto life = [](double t) { return Sample{{0, 10, 0, t >= 1 - 1e-8 && t < 2 - 1e-8 ? 65.f : 0.f}, {}}; };
        for (const auto& [time, lit] : {std::pair{1.70, false}, {1.74, true}, {2.70, true}, {2.74, false}}) {
            h.seek(time, life);
            const auto result = evaluate(h, {}, {0, 10, 0}, default_speed);
            check(result.known && (result.energy > 0) == lit, "birth/death events must arrive after .72 seconds");
        }
        History moving;
        moving.seek(2, [](double t) { return Sample{{float(10 + 2 * t), 0, 0, 65}, {}}; });
        check(moving.subluminal(10) && !moving.subluminal(1), "source speed admission");
        near(evaluate(moving, {}, {}, 10).delay, 2 - (2 - 1.0) / 1.2, 2e-6, "moving emitter retarded root");
        for (double crossing : {1.5, 2.0}) {
            History shadow;
            shadow.seek(2, [=](double t) { return Sample{{0, 10, 0, 65}, {float(6 * (t - crossing)), 5, 0, .35f}}; });
            const auto delayed = evaluate(shadow, {}, {}, 10);
            const auto instant = evaluate(shadow, {}, {}, 10, true);
            check(delayed.known && delayed.blocked == (crossing == 1.5), "photon-time moving occlusion");
            check(instant.blocked == (crossing == 2), "instantaneous shadow control");
        }
        History lifecycle;
        lifecycle.seek(2, [](double t) { return Sample{{0, 10, 0, 65}, {0, 5, 0, t >= 1.4 && t < 1.6 ? .35f : 0.f}}; });
        check(evaluate(lifecycle, {}, {}, 10).blocked && !evaluate(lifecycle, {}, {}, 10, true).blocked,
              "an occluder that has despawned now must still block its past photon event");
        History rolling;
        const auto indexed = [](double t) { return Sample{{float(t), 10, 0, 65}, {}}; };
        for (int tick = 0; tick <= 300; ++tick)
            rolling.seek(tick * step, indexed);
        check(rolling.count == capacity, "sequential history exceeded its cap");
        near(rolling.samples.front().light[0], 6, 1e-6, "rollover oldest sample");
        near(rolling.samples[rolling.count - 1].light[0], 10, 1e-6, "rollover latest sample");
        History bounded;
        int calls = 0;
        bounded.seek(10000, [&](double) {
            ++calls;
            return steady(0);
        });
        check(calls == int(capacity) && bounded.count == capacity, "large jumps must have bounded catch-up");
        near(bounded.oldest(), -4, 1e-12, "history window");
        check(!evaluate(bounded, {}, {0, 100, 0}, 10).known, "old history must be unknown, not clamped");
        const auto previous = bounded.samples;
        bounded.seek(10000, [&](double) {
            throw std::runtime_error("paused history regenerated");
            return Sample{};
        });
        check(previous[0].light == bounded.samples[0].light, "paused history changed");
        bounded.seek(1e9, steady);
        near(evaluate(bounded, {}, {0, 10, 0}, default_speed).delay, .72, 2e-6,
             "relative history time must retain precision after long-running simulation");
        bounded.seek(0, steady);
        check(bounded.count == 1 && !evaluate(bounded, {}, {}, 10).known, "reset must drop future/old history");
        check(evaluate(bounded, {}, {}, 10, true).known, "instantaneous mode needs no past history");
        bool rejected = false;
        try {
            bounded.seek(-1, steady);
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        check(rejected, "negative time must fail");
        std::cout << "Slow-light CPU: analytic delays, emission lifetimes, moving source/occluder, bounded history, "
                     "reset/pause\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
