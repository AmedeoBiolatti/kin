# Platform: files and processes

What a game or tool built on kin needs from the desktop beyond windows and
input, without calling SDL itself: files dropped on a window, the system's file
dialogs, child processes, and where to keep the user's data.

## Dropped files

```cpp
for (const kin::DroppedFile& file : ctx.input.take_dropped_files()) {
    import(file.path, ctx.renderer.window_to_logical(file.pos));
}
if (const auto at = ctx.input.drop_position()) {
    draw_drop_target(*at); // a drag from the desktop is over the window
}
```

- `Input::take_dropped_files()` returns the files dropped since the last call,
  oldest first, with where each landed (window pixels, like `mouse_pos()`) and
  on which window. Taking clears them, so each drop is handled exactly once
  however frames and fixed updates interleave.
- `Input::drop_position()` is where a drag is while it hovers over a window, so
  the game can highlight a drop target; it is empty otherwise.
- `Input::add_dropped_file()` makes a drop as if the desktop did, for tests and
  agents.

## File dialogs

```cpp
kin::FileDialogs dialogs{ctx.app};
const kin::u32 images = dialogs.open_file(&ctx.window, {
    .filters = {{"Images", "png;jpg;jpeg"}},
    .many = true,
});
// Later, once a frame:
for (const kin::FileDialogResult& result : dialogs.take_results()) {
    if (result.request == images && !result.cancelled()) {
        add_images(result.paths);
    }
}
```

- `open_file`, `save_file` and `open_folder` show the system's dialog over a
  window (or none) and return a request number. The dialog answers later, from
  whatever thread the system uses; answers wait until `take_results()`, so the
  game handles them on its own thread.
- A result holds the chosen paths, nothing if cancelled, or an error if the
  dialog could not be shown.
- Headless runs never show a dialog: one answers at once with an error.
  `answer_next(paths)` queues the answer the next dialog gives without showing,
  in any run, for tests and agents.
- Destroying `FileDialogs` while a dialog is open is safe; its answer is
  dropped.

## Child processes

```cpp
kin::Process helper;
if (!helper.start({.args = {"python3", "-m", "studiod"}, .working_directory = tools})) {
    log(helper.error());
}
helper.write("{\"id\": 1, \"method\": \"ping\"}\n");
// Once a frame:
while (auto line = helper.read_line()) {
    handle(*line);
}
if (!helper.running()) {
    report(helper.exit_code());
}
```

- `start(options)` runs `args[0]` (looked up on PATH when it has no path) with
  the rest as arguments, in `working_directory`. By default its standard input
  and output are pipes and its standard error is the app's;
  `errors_to_output` merges errors into the output instead.
- Nothing blocks the frame. `write()` queues input and sends what the pipe
  takes; `read_line()` returns a complete line (without `\n` or `\r\n`) when one
  has arrived, and once the child has exited, also a last unfinished line;
  `read()` returns everything read so far. `pump()` does the sending and
  reading, and the other calls pump as needed.
- `close_input()` gives the child end of file once queued input is sent.
- `running()` checks without waiting and keeps the exit code
  (`exit_code()`); `wait(timeout)` pumps until the child exits; `kill(force)`
  asks it to end, or ends it at once.
- A `Process` destroyed while its child runs ends the child, so a tool never
  leaves helpers behind.

Child processes talk to the world outside the game, so they are not
deterministic: keep them out of a headless simulation, or out of what its
report checks.

## The user's data

`kin::user_data_dir(app_id)` (`kin/platform/user_data.hpp`) is the folder for
settings and saves, per user and per app; `SaveStore` keeps its files there.
Tests point it elsewhere with `set_user_data_dir_override()`.
