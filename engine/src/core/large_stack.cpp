#include "large_stack.hpp"

#include <exception>

#if KIN_ENABLE_THREADS && !defined(__EMSCRIPTEN__) // (a browser thread's stack is taken from the heap)
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <process.h>
#include <windows.h>
#else
#include <pthread.h>
#endif
#endif

namespace kin::detail {
namespace {

thread_local bool t_on_large_stack = false;

#if KIN_ENABLE_THREADS && !defined(__EMSCRIPTEN__)
struct Run {
    const std::function<void(bool)>* work = nullptr;
    std::exception_ptr error;
};

void run(Run& r) {
    t_on_large_stack = true;
    try {
        (*r.work)(true);
    } catch (...) {
        r.error = std::current_exception();
    }
}

#if defined(_WIN32)
unsigned __stdcall entry(void* r) {
    run(*static_cast<Run*>(r));
    return 0;
}
#else
void* entry(void* r) {
    run(*static_cast<Run*>(r));
    return nullptr;
}
#endif
#endif

} // namespace

bool on_large_stack() {
    return t_on_large_stack;
}

void run_on_large_stack(const std::function<void(bool large)>& work) {
    if (t_on_large_stack) {
        work(true);
        return;
    }
#if KIN_ENABLE_THREADS && !defined(__EMSCRIPTEN__)
    Run r{&work, nullptr};
#if defined(_WIN32)
    // (A reservation: Windows commits the stack's pages as they are touched.)
    const auto thread = reinterpret_cast<HANDLE>(
        _beginthreadex(nullptr, unsigned(large_stack_bytes), entry, &r, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr));
    if (!thread) {
        work(false);
        return;
    }
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
#else
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_t thread;
    const bool made = pthread_attr_setstacksize(&attributes, large_stack_bytes) == 0 &&
                      pthread_create(&thread, &attributes, entry, &r) == 0;
    pthread_attr_destroy(&attributes);
    if (!made) {
        work(false);
        return;
    }
    pthread_join(thread, nullptr);
#endif
    if (r.error)
        std::rethrow_exception(r.error);
#else
    work(false);
#endif
}

} // namespace kin::detail
