#include <kin/anim/tween.hpp>

#include <algorithm>
#include <cmath>
#include <optional>
#include <stdexcept>
#include <vector>

namespace kin {

struct Tween::State {
    struct Step {
        f32 duration = 0;
        Easing easing = Easing::Linear;
        std::function<AnimValue()> get;
        std::function<void(const AnimValue&)> set;
        AnimValue target = 0.0f;
        std::optional<AnimValue> initial;
        std::function<void()> callback;
        bool started = false;
        bool done = false;
    };
    std::vector<std::vector<Step>> groups;
    std::function<bool()> alive;
    std::size_t group = 0;
    f32 elapsed = 0;
    u64 revision = 0;
    bool next_parallel = false;
    bool started = false;
    bool updating = false;
    bool stopped = false;
    bool complete = false;
    bool suspended = false;

    void append(Step step) {
        if (started) throw std::logic_error("Cannot append to a started tween; build a new tween");
        if (!std::isfinite(step.duration) || step.duration < 0)
            throw std::invalid_argument("Tween duration must be finite and non-negative");
        if (!next_parallel || groups.empty()) groups.emplace_back();
        groups.back().push_back(std::move(step));
        next_parallel = false;
    }
};

Tween::Tween() : _state(std::make_shared<State>()) {}
Tween::~Tween() = default;
Tween::Tween(Tween&&) noexcept = default;
Tween& Tween::operator=(Tween&&) noexcept = default;

Tween& Tween::property(std::function<AnimValue()> get, std::function<void(const AnimValue&)> set,
                       AnimValue destination, f32 seconds, Easing easing) {
    if (!get || !set) throw std::invalid_argument("Tween property needs a getter and setter");
    State::Step step;
    step.duration = seconds;
    step.easing = easing;
    step.get = std::move(get);
    step.set = std::move(set);
    step.target = std::move(destination);
    _state->append(std::move(step));
    return *this;
}

Tween& Tween::wait(f32 seconds) {
    State::Step step;
    step.duration = seconds;
    _state->append(std::move(step));
    return *this;
}

Tween& Tween::call(std::function<void()> callback) {
    if (!callback) throw std::invalid_argument("Tween callback is empty");
    State::Step step;
    step.callback = std::move(callback);
    _state->append(std::move(step));
    return *this;
}

Tween& Tween::parallel() {
    if (_state->started) throw std::logic_error("Cannot append to a started tween");
    _state->next_parallel = true;
    return *this;
}

Tween& Tween::while_alive(std::function<bool()> predicate) {
    _state->alive = std::move(predicate);
    return *this;
}

void Tween::advance(f32 dt) {
    if (!std::isfinite(dt) || dt < 0) throw std::invalid_argument("Tween dt must be finite and non-negative");
    // Keep state alive even if a callback releases the owning Tween.
    const auto state = _state;
    auto& s = *state;
    if (s.updating) throw std::logic_error("Cannot recursively advance a tween");
    if (s.stopped || s.complete || s.suspended) return;
    s.updating = true;
    struct Reset { bool& flag; ~Reset() { flag = false; } } reset{s.updating};
    s.started = true;
    const u64 revision = s.revision;
    const auto running = [&] {
        if (s.revision != revision || s.stopped || s.suspended) return false;
        // A predicate may replace itself or destroy its Tween.
        const auto alive = s.alive;
        if (alive && !alive()) { s.stopped = true; return false; }
        return s.revision == revision && !s.stopped && !s.suspended;
    };
    while (s.group < s.groups.size() && running()) {
        auto& group = s.groups[s.group];
        f32 duration = 0;
        for (const auto& step : group) duration = std::max(duration, step.duration);
        const f32 consumed = std::min(dt, std::max(0.0f, duration - s.elapsed));
        s.elapsed += consumed;
        dt -= consumed;
        for (auto& step : group) {
            if (step.done) continue;
            if (!step.started) {
                if (step.get) {
                    AnimValue initial = step.get();
                    if (!running()) return;
                    if (initial.index() != step.target.index())
                        throw std::invalid_argument("Tween property and destination have different types");
                    step.initial = std::move(initial);
                }
                step.started = true;
                if (step.callback) {
                    step.done = true;
                    step.callback();
                    if (!running()) return;
                }
            }
            if (step.set) {
                const f32 progress = step.duration == 0 ? 1.0f : std::min(1.0f, s.elapsed / step.duration);
                step.set(interpolate(*step.initial, step.target, progress, step.easing));
                if (!running()) return;
            }
            step.done = s.elapsed >= step.duration;
        }
        if (s.elapsed < duration) return;
        ++s.group;
        s.elapsed = 0;
    }
    if (running() && s.group == s.groups.size()) s.complete = true;
}

void Tween::pause() { _state->suspended = true; }
void Tween::resume() { _state->suspended = false; }
void Tween::cancel() { _state->stopped = true; }
void Tween::restart() {
    auto& s = *_state;
    ++s.revision;
    s.group = 0;
    s.elapsed = 0;
    s.stopped = s.complete = s.suspended = false;
    for (auto& group : s.groups) for (auto& step : group) {
        step.started = step.done = false;
        step.initial.reset();
    }
}
bool Tween::finished() const { return _state->complete; }
bool Tween::cancelled() const { return _state->stopped; }
bool Tween::paused() const { return _state->suspended; }

} // namespace kin
