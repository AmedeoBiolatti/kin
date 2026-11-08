#pragma once

// Work that recurses as deep as a user's file nests (a material graph's chains of nodes, compiled or imported), run
// on a stack of its own: a main thread's is 1 MB on Windows (and in a browser, KIN_WEB_STACK_SIZE), which such work
// overflows in a few dozen levels where its frames are large (MSVC's).

#include <cstddef>
#include <functional>

namespace kin::detail {

// The stack run_on_large_stack gives: reserved, its pages committed as they are used.
inline constexpr std::size_t large_stack_bytes = std::size_t(256) << 20;

// How many levels such work may recurse: on run_on_large_stack's stack (`large`), else on a caller's (1 MB).
constexpr int recursion_limit(bool large) {
    return large ? 1024 : 128;
}

// Runs `work` on a thread of its own with a stack of large_stack_bytes and returns once it has run (its exception, if
// it throws one, thrown again here). On that thread already, without threads (KIN_ENABLE_THREADS off) or in a browser
// (whose threads take their stacks from the heap), or where no thread can be made, `work` runs on the calling thread.
// `work` is told which stack it has: true, the large one.
void run_on_large_stack(const std::function<void(bool large)>& work);
// Whether this thread is run_on_large_stack's (recursion_limit(on_large_stack()): how deep work here may go).
bool on_large_stack();

} // namespace kin::detail
