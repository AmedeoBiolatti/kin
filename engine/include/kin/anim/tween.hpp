#pragma once

#include <kin/anim/value.hpp>

#include <functional>
#include <memory>
#include <utility>

namespace kin {

// A small, explicitly stepped animation. Steps run in sequence; parallel() puts
// the next step alongside the previous one. Initial values are captured when a
// step starts, so a sequence can use destinations chosen at runtime.
class Tween {
public:
    Tween();
    ~Tween();
    Tween(Tween&&) noexcept;
    Tween& operator=(Tween&&) noexcept;
    Tween(const Tween&) = delete;
    Tween& operator=(const Tween&) = delete;

    Tween& property(std::function<AnimValue()> get, std::function<void(const AnimValue&)> set,
                    AnimValue destination, f32 seconds, Easing easing = Easing::Linear);
    // References must outlive the tween; use property() with checked accessors
    // or while_alive() when an entity or UI object can disappear.
    template <typename T>
    Tween& to(T& value, T destination, f32 seconds, Easing easing = Easing::Linear) {
        return property([&value]() -> AnimValue { return value; },
                        [&value](const AnimValue& v) { value = std::get<T>(v); },
                        AnimValue{std::move(destination)}, seconds, easing);
    }
    Tween& wait(f32 seconds);
    Tween& call(std::function<void()> callback);
    Tween& parallel();
    Tween& while_alive(std::function<bool()> predicate);

    void advance(f32 dt);
    void pause();
    void resume();
    void cancel();
    void restart();
    bool finished() const;
    bool cancelled() const;
    bool paused() const;

private:
    struct State;
    std::shared_ptr<State> _state;
};

} // namespace kin
