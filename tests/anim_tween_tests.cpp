#include <kin/anim/tween.hpp>

#include <cassert>
#include <cmath>
#include <limits>
#include <stdexcept>

using namespace kin;

int main() {
    f32 x = 0, y = 0;
    int calls = 0;
    Tween tween;
    tween.to(x, 10.0f, 1).parallel().to(y, 20.0f, 2).wait(0.5f)
         .call([&] { ++calls; }).to(x, 30.0f, 1);
    tween.advance(0.5f);
    assert(x == 5 && y == 5);
    tween.pause(); tween.advance(100); assert(x == 5);
    tween.resume(); tween.advance(2.5f);
    assert(calls == 1 && x == 20 && y == 20 && !tween.finished());
    tween.advance(100); tween.advance(100);
    assert(x == 30 && calls == 1 && tween.finished());

    // A following step reads the actual property at its start, not when built.
    x = 0;
    Tween late;
    late.wait(1).to(x, 20.0f, 1);
    x = 10;
    late.advance(1.5f); assert(x == 15);
    late.cancel(); late.advance(10); assert(x == 15 && late.cancelled());
    late.restart(); late.advance(2); assert(x == 20 && late.finished());

    bool alive = true;
    Tween guarded;
    guarded.while_alive([&] { return alive; }).to(x, 40.0f, 1);
    alive = false; guarded.advance(1); assert(x == 20 && guarded.cancelled());

    Tween cancelled;
    cancelled.call([&] { cancelled.cancel(); }).call([&] { ++calls; });
    cancelled.advance(0); assert(calls == 1 && cancelled.cancelled());
    Tween restarted;
    int restarts = 0;
    restarted.call([&] { if (++restarts == 1) restarted.restart(); });
    restarted.advance(0); assert(!restarted.finished());
    restarted.advance(0); assert(restarted.finished() && restarts == 2);

    Vec2f pos{};
    Color color{0, 0, 0, 0};
    Tween typed;
    typed.to(pos, Vec2f{8, 4}, 1).parallel().to(color, Color{100, 200, 50, 100}, 1);
    typed.advance(0.5f); assert(pos.x == 4 && pos.y == 2 && color.r == 50);
    bool rejected = false;
    try { typed.advance(std::numeric_limits<f32>::quiet_NaN()); }
    catch (const std::invalid_argument&) { rejected = true; }
    assert(rejected);
    // A failed getter/type check may be retried without dereferencing an empty initial value.
    Tween mismatch;
    mismatch.property([]() -> AnimValue { return Vec2f{}; }, [](const AnimValue&) {}, 1.0f, 1);
    for (int attempt = 0; attempt < 2; ++attempt) {
        rejected = false;
        try { mismatch.advance(0); } catch (const std::invalid_argument&) { rejected = true; }
        assert(rejected);
    }
    auto owned = std::make_unique<Tween>();
    owned->call([&] { owned.reset(); });
    owned->advance(0); // callback may destroy the owning object
    assert(!owned);
    rejected = false;
    try { Tween bad; bad.wait(-1); } catch (const std::invalid_argument&) { rejected = true; }
    assert(rejected);
}
