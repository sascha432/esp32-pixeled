#include "px/memory/large.hpp"

#include <cstdlib>

namespace px::memory {

namespace {

// written once by main before the tasks start, read-only afterwards
AllocateFunction allocate_function = std::malloc;
ReleaseFunction release_function = std::free;

} // namespace

void set_large_heap(AllocateFunction allocate, ReleaseFunction release)
{
    allocate_function = allocate;
    release_function = release;
}

void *allocate_large(size_t size)
{
    return allocate_function(size == 0 ? 1 : size);
}

void release_large(void *pointer)
{
    if (pointer != nullptr) {
        release_function(pointer);
    }
}

} // namespace px::memory
