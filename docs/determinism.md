# Determinism

A run of a kin game is meant to play out the same way every time it gets the same seed and the same inputs. Replays,
the [determinism check](testing.md#determinism-check) and a game that keeps several machines in step all rely on it.
On one machine and one build, kin keeps that promise. Across machines it holds for what the table below lists. A
simulation that peers on different systems must agree on keeps to those parts.

| | What kin does | What is left to the game |
|---|---|---|
| Time | a fixed timestep; `--frames` and `--seed` make a headless run repeat exactly | nothing: don't read clocks in the simulation |
| Randomness | `SceneContext::rng` is the run's root key, from `--seed`. `SceneContext::step` numbers the updates (a paused inspector holds the count), and `fold_in(ctx.rng, ctx.step)` gives each update its own key | a simulation that is rolled back keeps its RNG key in its own state, so a rollback restores it |
| Fused multiply-adds | kin's own code whose bits matter is compiled without them: `kin::dmath`, the 3D physics glue, the scene session, PocketPy (`KIN_STRICT_FP` / `KIN_NO_FP_CONTRACT` in `engine/CMakeLists.txt`, including clang-cl and AppleClang) | a fused multiply-add rounds once where two operations round twice, so a target with FMA (aarch64, x86 with `-mfma` or `/arch:AVX2`) gives other bits: compile the game's simulation with `-ffp-contract=off` too (MSVC's `/fp:precise` does not contract) |
| `std::sin`, `std::atan2`, ... | `kin::dmath` (`kin/core/dmath.hpp`): sin, cos, sincos, tan, asin, acos, atan, atan2 in f32, the same bits on every compiler, C library and CPU. A golden hash checks it in CI on x64 (GCC, Clang, MSVC); arm64 and the web are not checked yet | C libraries differ in the last bit: use `kin::dmath`. There is no f64 version and no exp, log or pow yet. `std::sqrt`, `std::fmod` and the four operations are exact already |
| Floating-point mode | a 32-bit x86 build with x87 arithmetic does not compile `kin::dmath` | flush-to-zero (set by some audio or middleware code on its threads) changes results for subnormal numbers: keep it off on the simulation's thread |
| ECS scheduler | rate and interval counters, and which systems are enabled, live in the registry: `save_schedule_state()` / `restore_schedule_state()` beside the world. The bytes do not depend on the order systems were registered in | save and restore them with the world |
| flecs | | no snapshot, and query order follows table history: an ECS simulation can run in lockstep but cannot roll back yet |
| Lua | a fixed string-hash seed: `pairs` walks string and number keys in the same order everywhere. `kin::set_lua_hash_seed(key)` sets it from a session's key (every peer has the key, outsiders don't, so nobody can send strings made to collide). Scene scripts' `math.random` follows `--seed` (3D scenes: `SceneScriptOptions3D::rng`) | tables keyed by tables, functions or userdata walk by address. `math.sin`, `math.cos`, `math.exp`, `math.log` and `^` call the C library on doubles, so they can differ in the last bit between systems: keep them out of what peers on different systems must agree on. A Lua state cannot be saved, so scripts suit lockstep, not rollback, unless they keep their state in the simulation |
| Python (PocketPy) | instruction budgets count the same on every machine; built without fused multiply-adds | its `math` calls the C library, as Lua's does |
| Jolt (3D physics) | built with `CROSS_PLATFORM_DETERMINISTIC`; `World3D::save_state()`, `restore_state()` and `state_hash()` | Jolt's claim; kin has run it on one machine only |
| Box2D (2D physics) | v3 (`KIN_PHYSICS_BOX2D_V3`, experimental) is deterministic across platforms | 2.4, the default, is not; neither has a snapshot through `PhysicsWorld`, so 2D physics can run in lockstep on v3 but cannot roll back |
