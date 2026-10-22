#pragma once

// One push button as events (px/input/input.hpp): debounced, a click, a double click and a hold that
// fires once or repeats. Pure C++, host tested (host/tests/input_test.cpp).

#include <cstdint>

namespace px::input::detail {

// a level has to stay this long to count
constexpr uint32_t kDebounceMs = 30;
// a press this long is a hold
constexpr uint32_t kHoldMs = 600;
// a hold that repeats (the brightness) fires again this often
constexpr uint32_t kRepeatMs = 100;
// a second press within this time after a release makes a double click
constexpr uint32_t kDoubleMs = 300;

enum class Event : uint8_t {
    None,
    Click,
    DoubleClick,
    // after the hold time, and again every kRepeatMs with `repeat`
    Hold,
};

// what the button's actions need: whether a click waits for a second one, when the hold fires and
// whether it repeats
struct Behavior {
    bool double_click = false;
    uint32_t hold_ms = kHoldMs;
    bool repeat = false;
};

class Button {
public:
    // A button held when it starts (the hotspot at boot) gives no event until it was let go.
    explicit Button(bool pressed_at_start) : _raw(pressed_at_start), _stable(pressed_at_start), _armed(!pressed_at_start)
    {
    }

    // `pressed` sampled at `now_ms` (every few milliseconds); at most one event per call
    Event update(bool pressed, uint32_t now_ms, const Behavior &behavior);

    bool pressed() const
    {
        return _stable;
    }

private:
    enum class State : uint8_t {
        Idle,
        Pressed,
        // released after a click, waiting for a second press
        WaitDouble,
        SecondPress,
    };

    bool _raw;
    bool _stable;
    bool _armed;
    State _state = State::Idle;
    bool _held = false;
    uint32_t _raw_since = 0;
    uint32_t _since = 0;
    uint32_t _next_repeat = 0;
};

// The encoder's detents in one sample period as steps: one each while turned slowly, more when
// turned fast (`gap_ms` since the detent before).
int accelerate(int detents, uint32_t gap_ms);

} // namespace px::input::detail
