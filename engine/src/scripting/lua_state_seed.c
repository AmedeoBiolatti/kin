/* Lua's lstate.c, built with kin's string-hash seed in place of one made from the clock and addresses: a table with
 * string keys then walks in the same order (pairs) in every run and process, and on every peer that sets the same
 * seed. kin::set_lua_hash_seed (kin/scripting/lua_runtime.hpp) sets it; states made afterwards use it. */

static unsigned int kin_lua_hash_seed = 0x6b696e31u; /* "kin1" */

void kin_lua_set_hash_seed(unsigned int seed) {
    kin_lua_hash_seed = seed;
}

#define luai_makeseed(L) ((void)(L), kin_lua_hash_seed)
#include "lstate.c"
