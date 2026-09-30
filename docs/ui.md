# UI2

`ui2` is the only supported UI layer. The legacy `kin/ui` module and its retained
snapshot path have been removed, so new and existing UI code should include
`kin/ui2/*` headers and use the `kin::ui2` namespace explicitly.

## Entry Points

- `kin/ui2/context.hpp` for immediate-mode UI context, drawing, clipping, input,
  and interaction state.
- `kin/ui2/widgets.hpp` for controls, lists, tables, editor widgets, text inputs,
  scroll views, overlays, and game HUD widgets.
- `kin/ui2/theme.hpp` for themes and palette presets.
- `kin/ui2/geometry.hpp` for layout geometry helpers.
- `kin/ui2/text.hpp` for `kin::ui2::Font`, text measuring, wrapping, and drawing.
- `kin/ui2/menu_scene.hpp` for reusable menu scenes.
- `kin/ecs/ui2.hpp` for ECS-authored UI trees rendered through a `ui2::Context`.
- `kin/ecs/ui2_sync.hpp` for immediate-style code that reconciles into a retained
  ECS UI tree.

## Immediate UI

Keep a persistent `kin::ui2::Context` on the scene, overlay, or tool that owns the
UI. Begin it once per frame after input has been updated and before issuing UI
draw calls:

```cpp
kin::ui2::Context ui;

void render(kin::Input& input, kin::Renderer2D& renderer) {
    ui.begin(input, renderer);
    ui.surface({.x = 16, .y = 16, .w = 240, .h = 120}, panel_style);
    ui.text(font, "Score", {.x = 28, .y = 28}, text_style);
    ui.end();
}
```

Use `ui2` text helpers directly:

```cpp
auto font = kin::ui2::bitmap_font();
auto size = kin::ui2::measure_text(*font, "START", 2.0f);
kin::ui2::draw_text_centered(renderer, *font, "START", center, 2.0f, color);
```

## Text Editing

`TextInput` is one line; `TextEdit` is a multi-line editor for chat boxes and notes
(a few KB of text; it is not a code editor). Like `TextInput` it is a struct you run
each frame: keep it (or its `UiTextEditState`) across frames and read `result`
afterwards.

```cpp
kin::ui2::TextEdit chat{
    .id = kin::ui2::make_id("chat"),
    .placeholder = "Message",
    .submit = kin::ui2::UiSubmitKey::Enter, // Enter sends, Shift+Enter is a new line
    .auto_grow = true,                      // bounds.h follows the text...
    .max_height = 120.0f,                   // ...up to here, then it scrolls
};

chat.bounds = {x, bottom - chat.bounds.h, width, chat.bounds.h};
kin::ui2::run(ui, chat);
if (chat.result.submitted) {
    send(chat.state.text);
    chat.state.text.clear();
}
```

To keep the state in the `Context` instead, pass
`ui.text_edit_state(id, initial_text)` as the third argument of `run`.

- **Editing:** typed text (including composed characters), Backspace and Delete
  (with Ctrl, a word), Enter, and Tab (`UiTabKey::LeaveField` stops editing and
  reports `result.tab_direction`; `InsertSpaces` indents). Held keys repeat.
- **Moving:** arrows (Ctrl+Left/Right by word), Up/Down by displayed line keeping
  the column, Home/End (with Ctrl, the whole text), Page Up/Down. Shift selects.
- **Mouse:** click places the caret, drag selects and scrolls past an edge,
  double-click selects a word, triple-click a line, Shift+click extends; the wheel
  and the scrollbar scroll.
- **Clipboard and undo:** Ctrl+C/X/V (pasted line endings become `\n`), Ctrl+Z,
  and Ctrl+Y or Ctrl+Shift+Z. Consecutive typing or deleting is one undo step; the
  history keeps `TextEdit::undo_limit` steps.
- **Read-only:** `read_only = true` keeps selecting and copying but refuses edits,
  for selectable chat history or logs.
- **Layout:** `wrap` soft-wraps at the width (otherwise lines scroll sideways);
  `max_bytes` caps the text, cutting only between characters. Lines are cached
  and re-wrapped only when the text, width or font changes.

Carets in both text widgets move by character, never into the middle of a
multi-byte UTF-8 character; `kin/core/utf8.hpp` has the stepping and word helpers,
and `wrap_text_ranges` in `kin/ui2/text.hpp` wraps text into byte ranges.

## Menu Scenes

Games that need a simple navigable menu should use `kin::ui2::MenuScene`. It keeps
menu input, focus, and button rendering on the same `ui2` path as the rest of the
engine.

## ECS Sync And Lazy Layout

Retained UI can be authored directly with `ui2_entity(...)` or reconciled each
frame from immediate-style code:

```cpp
kin::Ui2Sync sync = kin::begin_ui2_sync(world, "hud", screen_bounds);
sync.begin_column(kin::ui2::column(kin::ui2::grow(), kin::ui2::fit()));
sync.key("score").label({.text = "Score: " + std::to_string(score)});
if (sync.button({.label = "Restart"}).clicked) {
    restart();
}
sync.end_column();
sync.end();
```

Each sync call upserts a stable ECS entity under the named root. Without
`key(...)`, identity is derived from parent, widget kind, label/text when present,
and sibling occurrence. Use explicit keys for looped or reorderable content.
`layout(style)` sets the next widget's `LayoutStyle` (one-shot, like `key`);
leaf widgets default to a fit-sized layout otherwise. Sibling order follows call
order via `Ui2Layout.sequence`; builder-made siblings should set distinct
`.layer(n)` values when document order matters, because entity ids get recycled.
`end()` sweeps synced entities under that root that were not visited this frame;
`keep_unvisited()` disables that sweep for toggled menus.

Widget config is overwritten by sync, while interaction state written by
`update_ui2_world` is preserved and returned on the next sync call. This is the
same one-frame result model as immediate `ui2::Context`.

ECS roots cache their solved layout. `update_ui2_world` still draws and runs
interaction every frame, but it skips gather/measure/solve when the root bounds
and tree version are unchanged. Sync, builder helpers, layout/root/scroll
component changes, widget add/remove observers, scroll feedback, and
`ui2_mark_dirty(entity)` bump the tree version. Code that mutates layout-affecting
widget fields through raw component pointers should call `ui2_mark_dirty(entity)`
afterward.

## Runtime Debug Overlay

The F1 runtime debug overlay is implemented with `kin::ui2::Context`. It renders
above the active scene in native render-output/window pixel space by using
`Renderer2D::scoped_native_coordinates()`, so panel bounds, hit testing, resizing,
scrolling, and copy selection are independent of a game's configured logical size.

## Automation

There is no `ui.snapshot` server method. For automation, expose assertable scene
state through `Scene::write_report`, inspect ECS scenes through `world.snapshot`,
and drive UI with `input.mouse`, `input.action`, and `input.text`.

