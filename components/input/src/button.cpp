// One push button as events (button.hpp).

#include "button.hpp"

namespace px::input::detail {

Event Button::update(bool pressed, uint32_t now_ms, const Behavior &behavior)
{
    if (pressed != _raw) {
        _raw = pressed;
        _raw_since = now_ms;
    }
    // a level that held for the debounce time is the button's state
    const bool edge = _raw != _stable && now_ms - _raw_since >= kDebounceMs;
    if (edge) {
        _stable = _raw;
    }
    if (!_armed) {
        _armed = edge && !_stable;
        return Event::None;
    }
    if (edge && _stable) {
        _state = _state == State::WaitDouble ? State::SecondPress : State::Pressed;
        _since = now_ms;
        _held = false;
        return Event::None;
    }
    switch (_state) {
    case State::Pressed:
        if (edge) {
            // let go: a hold is over, a click waits for a second press when that has an action
            const bool held = _held;
            _state = !held && behavior.double_click ? State::WaitDouble : State::Idle;
            _since = now_ms;
            return held || behavior.double_click ? Event::None : Event::Click;
        }
        if (!_held && now_ms - _since >= behavior.hold_ms) {
            _held = true;
            _next_repeat = now_ms + kRepeatMs;
            return Event::Hold;
        }
        if (_held && behavior.repeat && static_cast<int32_t>(now_ms - _next_repeat) >= 0) {
            _next_repeat += kRepeatMs;
            return Event::Hold;
        }
        return Event::None;
    case State::WaitDouble:
        if (now_ms - _since >= kDoubleMs) {
            _state = State::Idle;
            return Event::Click;
        }
        return Event::None;
    case State::SecondPress:
        if (edge) {
            _state = State::Idle;
            return Event::DoubleClick;
        }
        return Event::None;
    case State::Idle:
        return Event::None;
    }
    return Event::None;
}

int accelerate(int detents, uint32_t gap_ms)
{
    // a detent every 40 ms or faster is a quick spin: four times the steps; up to 100 ms twice
    const int factor = gap_ms < 40 ? 4 : gap_ms < 100 ? 2 : 1;
    return detents * factor;
}

} // namespace px::input::detail
