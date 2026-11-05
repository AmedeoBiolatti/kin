// PocketPy's amalgamation, compiled with kin's sandbox (pocketpy_sandbox.h):
// its allocations counted, its watchdog counting instructions, and a way to
// take modules out of its VM. Built as C, without kin's warnings (it is
// third-party code but for the few functions at the end).

#include "pocketpy_sandbox.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// The budget's state: instructions counted since it began, and why the call stopped.
static uint64_t kin_steps = 0;
static uint64_t kin_step_limit = 0;
static size_t kin_memory_limit = 0;
static size_t kin_memory_held = 0;
static bool kin_budget_running = false;
static enum KinPocketPyStop kin_stop = KIN_POCKETPY_RUNNING;

static void* kin_pocketpy_malloc(size_t size);
static void* kin_pocketpy_realloc(void* memory, size_t size);
static void kin_pocketpy_free(void* memory);
static void kin_pocketpy_run_out(enum KinPocketPyStop why);

// PocketPy's watchdog compares clock() with its deadline at every instruction
// while one is set: here it gets the instructions counted instead.
static inline clock_t kin_pocketpy_step(void) {
    ++kin_steps;
    if (kin_step_limit > 0 && kin_steps > kin_step_limit && kin_stop == KIN_POCKETPY_RUNNING) {
        kin_pocketpy_run_out(KIN_POCKETPY_OUT_OF_INSTRUCTIONS);
    }
    return (clock_t)kin_steps;
}

#define PK_MALLOC(size) kin_pocketpy_malloc(size)
#define PK_REALLOC(memory, size) kin_pocketpy_realloc(memory, size)
#define PK_FREE(memory) kin_pocketpy_free(memory)
#define clock() kin_pocketpy_step()

#include "pocketpy.c"

#undef clock

// Each allocation carries its size before it, in 16 bytes (malloc's own alignment).
enum { kin_header_size = 16 };

// Makes the watchdog fire at the next instruction.
static void kin_pocketpy_stop_now(void) {
    if (kin_steps < 2) {
        kin_steps = 2;
    }
    pk_current_vm->watchdog_info.max_reset_time = 1;
}

// Once a call ran out, every new line, call and return stops it again.
static void kin_pocketpy_keep_stopping(py_Frame* frame, enum py_TraceEvent event) {
    (void)frame;
    (void)event;
    kin_pocketpy_stop_now();
}

static void kin_pocketpy_run_out(enum KinPocketPyStop why) {
    if (kin_stop == KIN_POCKETPY_RUNNING) {
        kin_stop = why;
        py_sys_settrace(kin_pocketpy_keep_stopping, false);
    }
    kin_pocketpy_stop_now();
}

static void kin_pocketpy_count(size_t before, size_t after) {
    kin_memory_held = kin_memory_held - before + after;
    if (kin_budget_running && kin_memory_limit > 0 && kin_memory_held > kin_memory_limit) {
        kin_pocketpy_run_out(KIN_POCKETPY_OUT_OF_MEMORY);
    }
}

static void* kin_pocketpy_malloc(size_t size) {
    unsigned char* block = malloc(kin_header_size + size);
    if (block == NULL) {
        return NULL;
    }
    memcpy(block, &size, sizeof(size));
    kin_pocketpy_count(0, size);
    return block + kin_header_size;
}

static void* kin_pocketpy_realloc(void* memory, size_t size) {
    if (memory == NULL) {
        return kin_pocketpy_malloc(size);
    }
    unsigned char* block = (unsigned char*)memory - kin_header_size;
    size_t before = 0;
    memcpy(&before, block, sizeof(before));
    unsigned char* moved = realloc(block, kin_header_size + size);
    if (moved == NULL) {
        return NULL;
    }
    memcpy(moved, &size, sizeof(size));
    kin_pocketpy_count(before, size);
    return moved + kin_header_size;
}

static void kin_pocketpy_free(void* memory) {
    if (memory == NULL) {
        return;
    }
    unsigned char* block = (unsigned char*)memory - kin_header_size;
    size_t size = 0;
    memcpy(&size, block, sizeof(size));
    kin_memory_held -= size;
    free(block);
}

void kin_pocketpy_budget_begin(uint64_t instructions, size_t memory_limit) {
    // clock_t is 32 bits on Windows: a deadline past it would never come.
    const uint64_t most = sizeof(clock_t) >= 8 ? (uint64_t)INT64_MAX : (uint64_t)INT32_MAX;
    kin_steps = 0;
    kin_step_limit = instructions < most ? instructions : most;
    kin_memory_limit = memory_limit;
    kin_stop = KIN_POCKETPY_RUNNING;
    kin_budget_running = true;
    pk_current_vm->watchdog_info.max_reset_time = (clock_t)kin_step_limit;
}

void kin_pocketpy_budget_end(void) {
    if (kin_stop != KIN_POCKETPY_RUNNING) {
        py_sys_settrace(NULL, true);
    }
    kin_budget_running = false;
    kin_step_limit = 0;
    pk_current_vm->watchdog_info.max_reset_time = 0;
}

enum KinPocketPyStop kin_pocketpy_budget_stop(void) {
    return kin_stop;
}

uint64_t kin_pocketpy_budget_used(void) {
    return kin_steps;
}

size_t kin_pocketpy_memory_used(void) {
    return kin_memory_held;
}

bool kin_pocketpy_forget_module(const char* path) {
    // The VM's modules are a binary tree by path; its root ("") is no module.
    BinTree* parent = NULL;
    BinTree* node = &pk_current_vm->modules;
    while (node != NULL) {
        const int order = strcmp(path, (const char*)node->key);
        if (order == 0) {
            break;
        }
        parent = node;
        node = order < 0 ? node->left : node->right;
    }
    if (node == NULL || parent == NULL) {
        return false;
    }
    BinTree* replacement = NULL;
    if (node->left == NULL) {
        replacement = node->right;
    } else if (node->right == NULL) {
        replacement = node->left;
    } else {
        // The least of its right side takes its place.
        BinTree* least_parent = node;
        BinTree* least = node->right;
        while (least->left != NULL) {
            least_parent = least;
            least = least->left;
        }
        if (least_parent != node) {
            least_parent->left = least->right;
            least->right = node->right;
        }
        least->left = node->left;
        replacement = least;
    }
    if (parent->left == node) {
        parent->left = replacement;
    } else {
        parent->right = replacement;
    }
    PK_FREE(node); // its key is the module's own path, which goes with the module
    return true;
}

static void kin_pocketpy_collect_paths(const BinTree* node, char (*paths)[PK_MAX_MODULE_PATH_LEN + 1], int* count,
                                       int capacity) {
    if (node == NULL) {
        return;
    }
    if (*count < capacity) {
        strncpy(paths[*count], (const char*)node->key, PK_MAX_MODULE_PATH_LEN);
        paths[*count][PK_MAX_MODULE_PATH_LEN] = '\0';
        ++*count;
    }
    kin_pocketpy_collect_paths(node->left, paths, count, capacity);
    kin_pocketpy_collect_paths(node->right, paths, count, capacity);
}

int kin_pocketpy_keep_modules(const char* const* kept, int count) {
    enum { most = 256 };
    // Their paths copied first: the tree changes as modules go.
    static char paths[most][PK_MAX_MODULE_PATH_LEN + 1];
    int found = 0;
    kin_pocketpy_collect_paths(pk_current_vm->modules.left, paths, &found, most);
    kin_pocketpy_collect_paths(pk_current_vm->modules.right, paths, &found, most);
    int taken = 0;
    for (int i = 0; i < found; ++i) {
        bool keep = strcmp(paths[i], "builtins") == 0 || strcmp(paths[i], "__main__") == 0;
        for (int k = 0; k < count && !keep; ++k) {
            keep = strcmp(paths[i], kept[k]) == 0;
        }
        if (!keep && kin_pocketpy_forget_module(paths[i])) {
            ++taken;
        }
    }
    return taken;
}

struct py_TValue* kin_pocketpy_function_module(struct py_TValue* function) {
    if(!py_istype(function, tp_function)) return NULL;
    const Function* made = py_touserdata(function);
    return made->module;
}
