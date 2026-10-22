// components/input/src/button.cpp: a button's clicks, double clicks and holds, the encoder's
// acceleration.

#include <vector>

#include "button.hpp"
#include "check.hpp"

using namespace px::input::detail;

namespace {

// Samples `pressed` every 10 ms from `from` for `ms`; the events that came, in order.
std::vector<Event> run(Button &button, bool pressed, uint32_t &now, uint32_t ms, const Behavior &behavior)
{
    std::vector<Event> events;
    for (const uint32_t end = now + ms; now < end; now += 10) {
        const Event event = button.update(pressed, now, behavior);
        if (event != Event::None) {
            events.push_back(event);
        }
    }
    return events;
}

} // namespace

TEST(input_click_after_the_release)
{
    Button button(false);
    uint32_t now = 1000;
    const Behavior behavior{};
    CHECK(run(button, true, now, 200, behavior).empty());
    CHECK(button.pressed());
    const auto events = run(button, false, now, 100, behavior);
    REQUIRE(events.size() == 1);
    CHECK(events[0] == Event::Click);
}

TEST(input_bounce_is_no_press)
{
    Button button(false);
    uint32_t now = 0;
    const Behavior behavior{};
    // 20 ms pressed is shorter than the debounce time
    CHECK(run(button, true, now, 20, behavior).empty());
    CHECK(run(button, false, now, 200, behavior).empty());
    CHECK(!button.pressed());
}

TEST(input_click_waits_for_a_double_click)
{
    const Behavior behavior{.double_click = true};
    {
        Button button(false);
        uint32_t now = 0;
        run(button, true, now, 100, behavior);
        // no click yet: a second press may come
        CHECK(run(button, false, now, 100, behavior).empty());
        run(button, true, now, 100, behavior);
        const auto events = run(button, false, now, 500, behavior);
        REQUIRE(events.size() == 1);
        CHECK(events[0] == Event::DoubleClick);
    }
    {
        Button button(false);
        uint32_t now = 0;
        run(button, true, now, 100, behavior);
        // no second press: the click after the double click time
        const auto events = run(button, false, now, 500, behavior);
        REQUIRE(events.size() == 1);
        CHECK(events[0] == Event::Click);
    }
}

TEST(input_hold_once_and_repeating)
{
    {
        Button button(false);
        uint32_t now = 0;
        const auto events = run(button, true, now, 2000, Behavior{});
        REQUIRE(events.size() == 1);
        CHECK(events[0] == Event::Hold);
        // the release after a hold is no click
        CHECK(run(button, false, now, 500, Behavior{}).empty());
    }
    {
        Button button(false);
        uint32_t now = 0;
        const Behavior repeat{.repeat = true};
        // the hold after 0.6 s (and the debounce), then every 100 ms
        const auto events = run(button, true, now, 1000, repeat);
        CHECK(events.size() >= 4 && events.size() <= 5);
    }
    {
        Button button(false);
        uint32_t now = 0;
        // the restart: after 3 s, not before
        const Behavior restart{.hold_ms = 3000};
        CHECK(run(button, true, now, 2900, restart).empty());
        CHECK(run(button, true, now, 300, restart).size() == 1);
    }
    {
        // no hold action: a long press is a click
        Button button(false);
        uint32_t now = 0;
        const Behavior none{.hold_ms = UINT32_MAX};
        CHECK(run(button, true, now, 2000, none).empty());
        const auto events = run(button, false, now, 100, none);
        REQUIRE(events.size() == 1);
        CHECK(events[0] == Event::Click);
    }
}

TEST(input_held_at_start_counts_after_the_release)
{
    Button button(true);
    uint32_t now = 0;
    // held from the boot (the hotspot): no hold, no click
    CHECK(run(button, true, now, 5000, Behavior{}).empty());
    CHECK(run(button, false, now, 200, Behavior{}).empty());
    run(button, true, now, 100, Behavior{});
    const auto events = run(button, false, now, 100, Behavior{});
    REQUIRE(events.size() == 1);
    CHECK(events[0] == Event::Click);
}

TEST(input_encoder_accelerates)
{
    CHECK(accelerate(1, 500) == 1);
    CHECK(accelerate(-1, 500) == -1);
    CHECK(accelerate(2, 60) == 4);
    CHECK(accelerate(3, 10) == 12);
}
