#include <kin/assets/asset_scope.hpp>

#include <cassert>

using namespace kin;
struct Data { int value = 0; };
struct Alternate { int value = 0; };

int main() {
    AssetManager manager;
    int loads = 0;
    manager.register_loader<Data>([&](const auto&) { ++loads; return Data{42}; });
    auto a = manager.load_handle<Data>("a");
    auto shared = a.shared();
    a = {};
    assert(manager.unload_unused() == 0 && shared->value == 42);
    shared.reset();
    assert(manager.unload_unused() == 1 && !manager.loaded<Data>("a"));
    {
        AssetScope level;
        level.load<Data>(manager, "a");
        assert(manager.unload_unused() == 0);
    }
    assert(manager.unload_unused() == 1);
    manager.register_loader<Alternate>([](const auto&) { return Alternate{9}; });
    auto original = manager.load_handle<Data>("same_path");
    { auto alternative = manager.load_handle<Alternate>("same_path"); }
    assert(manager.unload_unused() == 1);
    assert(manager.metadata("same_path")->status == AssetStatus::Loaded && original->value == 42);
    original = {};
    assert(manager.unload_unused() == 1);

    JobSystem jobs{{.single_threaded = true}};
    AssetServer server{manager, {.jobs = &jobs, .unload_unused = true}};
    int async_loads = 0;
    server.register_async_loader<Data>([&](const auto& path, LoadContext& ctx) {
        ++async_loads;
        if (path.filename() == "root") ctx.require<Data>("leaf");
        if (path.filename() == "leaf") ctx.require<Data>("root");
        return Data{7};
    });
    auto root = server.load_async<Data>("root");
    server.drain();
    assert(root && server.ready<Data>("root") && manager.loaded<Data>("leaf"));
    assert(manager.unload_unused() == 0);
    root = {};
    server.drain();
    assert(!manager.loaded<Data>("root") && !manager.loaded<Data>("leaf"));
    assert(server.load_state<Data>("root") == LoadState::NotLoaded);

    async_loads = 0;
    server.load_async<Data>("root"); // unreferenced, cyclic dependency batch
    server.pump(AssetServer::PumpMode::Budgeted);
    assert(manager.unload_unused() == 0); // completed parent survives its pending child
    for (int frame = 0; frame < 4; ++frame) server.pump(AssetServer::PumpMode::Budgeted);
    assert(async_loads == 2 && server.stats().pending == 0);
    assert(!manager.loaded<Data>("root") && !manager.loaded<Data>("leaf"));

    // Scope pins an asynchronous load even before a shared() view is available.
    AssetScope scope;
    scope.load_async<Data>(server, "pending");
    assert(manager.unload_unused() == 0);
    server.drain();
    assert(manager.loaded<Data>("pending"));
    scope.clear(); server.drain();
    assert(!manager.loaded<Data>("pending"));

    server.load_async<Data>("orphan"); // no caller retains this pending job
    assert(manager.unload_unused() == 0); // the job owns its storage until applied
    server.drain();
    assert(!manager.loaded<Data>("orphan"));
}
