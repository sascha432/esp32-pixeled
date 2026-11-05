#include <cstdlib>

#include "check.hpp"
#include "px/memory/large.hpp"

using namespace px::memory;

namespace {

// a heap with room for `g_room` bytes at a time
size_t g_room = 0;

void *small_heap(size_t size)
{
    return size <= g_room ? std::malloc(size) : nullptr;
}

} // namespace

// a full heap answers null instead of stopping (the render task says "no memory for the frame")
TEST(memory_full_heap_null)
{
    g_room = 64;
    set_large_heap(small_heap, std::free);
    void *fits = allocate_large(64);
    void *too_large = allocate_large(65);
    set_large_heap(std::malloc, std::free);
    CHECK(fits != nullptr);
    CHECK(too_large == nullptr);
    release_large(fits);
    release_large(too_large);
}

TEST(memory_array_says_when_it_does_not_fit)
{
    g_room = 8;
    set_large_heap(small_heap, std::free);
    Array<uint16_t> values;
    const bool four = values.assign(4, 7);
    const size_t four_size = values.size();
    const bool five = values.assign(5, 7);
    const size_t five_size = values.size();
    const bool listed = values.assign({1, 2, 3});
    set_large_heap(std::malloc, std::free);
    CHECK(four && four_size == 4);
    CHECK(!five && five_size == 0);
    REQUIRE(listed && values.size() == 3);
    CHECK(values[0] == 1 && values[2] == 3);
    Array<uint16_t> moved = std::move(values);
    CHECK(moved.size() == 3 && values.size() == 0);
}
