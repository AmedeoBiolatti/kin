#pragma once

// What kin's Python entity scripts (python_scripts.cpp) need of PocketPy
// beyond its public API (pocketpy.h). pocketpy_sandbox.c compiles PocketPy's
// amalgamation with these, reaching into its VM where pocketpy.h has no
// call for it:
//
// - an instruction budget: PocketPy's watchdog counts bytecode instructions
//   instead of reading the CPU clock (its own watchdog asks the clock at every
//   instruction: some 45 times slower), so a call stops after the same
//   instructions on every run and machine;
// - a memory count and limit: PocketPy allocates through kin, which counts
//   what it holds and stops the running call once that passes the limit;
// - built-in modules taken out, so that importing one fails as if it never
//   existed (PocketPy registers them all when its VM starts).
//
// A call that runs out stops with a TimeoutError at its next instruction.
// From then on until kin_pocketpy_budget_end(), every new line, call and
// return raises it again, so `except` in the script cannot keep it going.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum KinPocketPyStop {
    KIN_POCKETPY_RUNNING = 0,
    KIN_POCKETPY_OUT_OF_INSTRUCTIONS = 1,
    KIN_POCKETPY_OUT_OF_MEMORY = 2,
};

// Starts one call's budget: at most `instructions` bytecode instructions (0:
// no limit), while PocketPy holds at most `memory_limit` bytes (0: no limit).
void kin_pocketpy_budget_begin(uint64_t instructions, size_t memory_limit);
// Ends it: no limit until the next begin.
void kin_pocketpy_budget_end(void);
// Why the call of the last budget stopped, if it ran out.
enum KinPocketPyStop kin_pocketpy_budget_stop(void);
// Instructions the call of the last budget ran (counted only with a limit).
uint64_t kin_pocketpy_budget_used(void);

// Bytes PocketPy holds now (garbage not yet collected included).
size_t kin_pocketpy_memory_used(void);

// Takes the module `path` out of the current VM, so that importing it fails;
// the module itself goes with the garbage once nothing refers to it. False
// when there is none by that name.
bool kin_pocketpy_forget_module(const char* path);
// Takes out every module of the current VM but `kept` (`count` names),
// "builtins" and "__main__". Returns how many it took out.
int kin_pocketpy_keep_modules(const char* const* kept, int count);

// The module a Python function (not a native one) was defined in: what its
// globals are. NULL for anything else. PocketPy's functions have no
// __module__ to ask from Python.
struct py_TValue* kin_pocketpy_function_module(struct py_TValue* function); // py_GlobalRef (py_Ref)

#ifdef __cplusplus
}
#endif
