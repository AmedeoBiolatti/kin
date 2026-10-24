// Included inside the Emscripten runtime, so FS/IDBFS need no public exports.
Module["preRun"] = Module["preRun"] || [];
Module["preRun"].push(function () {
  const state = (Module["kinStorage"] = {
    ready: false,
    available: false,
    pending: false,
    flushes: 0,
    error: null,
  });
  let busy = false,
    again = false;
  state.flush = function () {
    if (!state.available) return;
    if (busy) {
      again = true;
      return;
    }
    busy = state.pending = true;
    FS.syncfs(false, function (error) {
      busy = false;
      if (error) {
        state.available = false;
        state.error = String(error);
      } else ++state.flushes;
      if (again && state.available) {
        again = false;
        state.flush();
      } else {
        again = false;
        state.pending = false;
      }
    });
  };
  addRunDependency("kin-user-storage");
  let mounted = false;
  function complete(error) {
    state.ready = true;
    state.available = !error;
    if (error) {
      state.error = String(error);
      if (mounted) {
        try {
          FS.unmount("/persistent");
        } catch (_) {}
      }
    }
    removeRunDependency("kin-user-storage");
  }
  try {
    FS.mkdirTree("/persistent");
    FS.mount(IDBFS, {}, "/persistent");
    mounted = true;
    FS.syncfs(true, complete);
  } catch (error) {
    complete(error);
  }
});
