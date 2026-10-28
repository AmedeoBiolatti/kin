# UI2

`ui2` is the only supported UI layer. The legacy `kin/ui` module and its retained
snapshot path have been removed, so new and existing UI code should include
`kin/ui2/*` headers and use the `kin::ui2` namespace explicitly.

## Entry Points

- `kin/ui2/context.hpp` for immediate-mode UI context, drawing, clipping, input,
  and interaction state.
- `kin/ui2/widgets.hpp` for controls, lists, tables, text inputs, scroll views
  and overlays. It brings `kin/ui2/editor.hpp` (docks, splitters, breadcrumbs,
  an asset browser, a log console, property inspectors and grids, node graphs)
  and `kin/ui2/hud.hpp` (input prompts, dialogue, nameplates, a target reticle,
  resource rows, bars and meters, inventory slots, floating text); each of those
  can be included alone.
- `kin/ui2/charts.hpp` for plots, sparklines and heat maps (spectrograms), and
  `kin/ui2/scale.hpp` for linear and log scales and the round values to mark on
  them.
- `kin/ui2/theme.hpp` for themes and palette presets; `kin/ui2/theme_file.hpp` for
  themes in `.kintheme` data files.
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

## Composition (experimental)

Inside `begin_layout()`/`end_layout()`, a box is an open container: a layout
node with a surface, a hit region and any children, so rows, tabs and panes are
built from boxes and labels instead of picked from the widget list.

```cpp
ui.begin_layout(screen);
{
    auto files = ui.scroll(make_id("files"), vbox().fill());
    ui.list(make_id("rows"), int(entries.size()), 22.0f, [&](int i) { // only visible rows are built
        auto row = ui.box(make_id(make_id("row"), i),
                          hbox().fill().gap(6).items(UiAlign::Center)
                              .hover_bg(hover).selected_bg(accent).selected_if(i == selected));
        ui.label(entries[i].name, {.width = grow()});
        if (row.hovered) ui.label("x");
        if (row.clicked) selected = i;
    });
}
ui.end_layout();
```

- `box()` returns a scope (it ends the box when it goes) carrying `hot`,
  `clicked`, `double_clicked`, `secondary_clicked` (right button), ... and
  `hovered`, which unlike `hot` stays true over the box's children; the box's
  `surface.hovered` follows it. Boxes are hit-tested where they were drawn last
  frame, like `hot`, so their results are there while their children are
  declared. `bounds` is that rect, to anchor a popup or a drag.
- `clip` clips the children to the box; `clip_rounded` to its rounded corners
  (a path clip: two layers and a render pass each, so only where corners show).
- `scroll()` clips and scrolls its children, vertically and with
  `ScrollBoxOptions::horizontal` sideways too (they keep their width; the wheel
  sideways or Shift+wheel). The wheel moves the innermost scroll box under the
  pointer that can move that way, and the scrollbars' thumbs are dragged before
  the content is placed, so both move it in the same frame.
  `scroll_offset(id)` and `set_scroll_offset(id, offset)` read and set it.
- `list(id, count, row_height, row)` calls `row(i)` only for the rows visible in
  the enclosing scroll box, plus a few either side; `scroll_to_row(list, i)`
  brings a row into view this frame, built or not, and `scroll_into_view(id)`
  a box (or a widget, by the box around it). With
  `ScrollBoxOptions::stick_to_end` a scroll box at its end stays there as its
  content grows (a log); scrolled up, it stays put. PageUp, PageDown, Home and
  End scroll the box the focus is in, else the one under the pointer.
- `begin_split()` / `split_next()` / `end_split()` make two clipped panes and a
  handle that drags the ratio, kept by id.
- `label()`, `image()`, `spacer()` and `add(widget)` add leaves the layout
  keeps; `widget(w)` still writes a widget's results back.

### Styles in one line

`BoxStyle` and `LayoutStyle` chain: start from `hbox()` (children side by
side) or `vbox()` and add what differs. `w()`/`h()` take a `SizeAxis` or pixels,
`fill()` grows both ways, `pad()`/`margins()` take one, two (x, y) or four
values, `gap()`, `justify()` (along) and `items()` (across) align, and `center()`
does both. For the look, `bg()` fills every state that has no fill of its own,
whichever comes first, `hover_bg()`, `pressed_bg()` and `selected_bg()` give one,
and `rounded()`, `border()` and `focus_border()` the rest. `passive()`,
`clipped()`, `selected_if()`, `disabled_if()`, `tab_stop()`, `key_scope()` and
`cursor_shape()` set the flags. Designated initializers still work.

### State a component keeps

`ui.state<T>(id)` is a `T` kept for `id`, made the first time it is asked for
(`ui.state<T>(id, make)` makes it with `make()`). A function that builds part of
the UI can keep its own state with it, so the caller passes only what it shows:

```cpp
void section(Context& ui, Id id, std::string_view title, auto&& body) {
    struct Section { bool open = true; };
    Section& state = ui.state<Section>(id);
    if (ui.box(make_id(id, "header"), hbox().w(grow()).h(22).hover_bg(hover)).clicked) state.open = !state.open;
    if (state.open) body();
}
```

It goes once a whole frame passes without it being asked for, as what used it is
gone; `keep_state(id)` keeps it anyway (a panel that is hidden for a while)
until `drop_state(id)`.

### Pointer shapes and drawing less

`BoxStyle::cursor` sets the pointer's shape over a box and its children; text
fields show the I-beam and splitters the resize arrows. `cursor()` is the
frame's choice. Something still moving, such as a hover transition or a
blinking caret, asks for another frame (`wants_redraw()`).

A tool can draw only when something changes: set `wait_for_events` in its
`GameWindowInfo` (or `AppConfig`), and the loop sleeps until input or a window
event comes (drawing two frames more, as hover resolves a frame late), a
`request_redraw()` falls due, or `max_idle_wait` passes. Call
`ui.apply(ctx.app, ctx.window)` after `end()` each frame: it hands over the text
input, the pointer shape and the redraw requests. Each frame drawn runs one
fixed step, so game time stops while the window idles.

### Menus and popovers

A box with `floating()` leaves its parent's flow and hangs next to it instead:
`Below` (a dropdown), `Above`, `Right` (a submenu), `Left`, or at a `Point` (a
context menu at the pointer), moved by an offset. It is sized against the
layout's root, not its parent, and kept inside the root: on the other side when
it fits better there, then moved along. It draws above everything else and
outside its ancestors' clips, and it and its contents are hit before what is
under them. Build it where it belongs, while it is open:

```cpp
auto title = ui.box(id, ui::hbox().pad(8, 2).hover_bg(hover).selected_if(open));
ui.label("File", text);
if (open) {
    auto menu = ui.box(ui::make_id(id, "menu"), ui::vbox().w(280).pad(4).bg(panel).floating(ui::UiFloat::Below));
    // rows; a row's submenu is a box floating(ui::UiFloat::Right) built inside the row
    if (menu.pressed_outside) { open = false; }
}
```

`pressed_outside` is true when a mouse button went down this frame away from the
box (as it was last frame): what closes a popup.

`popup(id, style, place)` does the rest: open (`open_popup(id)`, or
`open_popup_at(id, point)` for a context menu), it is a box under
`PopupPlace::anchor` (or at its point), closed by a press outside it and its
`opener`, or by Esc while it is the one on top (`popup_on_top(id)`):

```cpp
auto button = ui.box(ui::make_id("add"), ui::hbox().pad(8, 2).hover_bg(hover));
ui.label("Add", text);
if (button.clicked) ui.open_popup(ui::make_id("add_menu"));
if (auto menu = ui.popup(ui::make_id("add_menu"), ui::vbox().w(200).pad(4).bg(panel),
                         {.anchor = button.bounds, .opener = ui::make_id("add")})) {
    // rows
}
```

A box's `tooltip(text)` shows after the pointer rests on it (`TooltipOptions`:
the delay, and a shorter one while another tooltip was just up;
`set_tooltip_style()` for the look). `draggable(type, text, value)` makes a
press that moves a drag carrying `drag_payload()`, its text by the pointer;
a box that `accepts_drop(type)` is `drop_hovered` under it and `dropped` when
it is let go there. Tooltips and the drag's text are drawn after the layout,
over everything; `overlay(draw)` adds an app's own.

`kin/ui2/composed.hpp` has widgets made this way, in the theme's colours or a
`ComposedLook`: `menu_bar()` (a window's menus: Alt and a title's letter, the
arrows, submenus, their keys shown), `pick_list()` (a searchable list under a
button) and `breadcrumbs()`.

### Values typed in

`value_field(id, shown, options, valid)` is a text field for a value kept
elsewhere: it shows `shown` until clicked or tabbed into, then what is typed;
Enter, Tab or a press elsewhere commits it (`committed`, once), Esc goes back
to `shown`. What `valid` turns down is outlined and not committed. `live`
commits as it is typed, and Esc commits the value from before the typing. `number_field(id, value, {.min, .max, .whole})` does
this for numbers, and `edit_value(id)` starts typing in one (a rename from a
menu). `NumberInput` and `PropertyInspector` rows behave the same way.

```cpp
if (auto name = ui.value_field(ui::make_id("name"), entity.name, {.placeholder = "Name"},
                               [](std::string_view text) { return !text.empty(); }).committed) {
    rename(entity, *name);
}
if (const auto speed = ui.number_field(ui::make_id("speed"), mover.speed, {.min = 0})) {
    mover.speed = static_cast<f32>(*speed);
}
```

### Widgets drawn by hand

A widget of your own is a struct with `bounds`, a `measure()` and a `run()`
that `ui.widget()` finds (by argument-dependent lookup), as the built-in ones
are. Its run draws with the Context: rectangles and text, and anti-aliased
shapes (`line(a, b, color, width)`, `polyline`, `fill_polygon`, `fill_circle`,
`outline_circle`, `arc`, `fill_pie`, `fill_path`, `stroke_path`, and `shape()`
for a `ShapeMesh` made once). They are mirrored, clipped, checked against the
draw scope and recorded as everything else the Context draws.

`drag_value(id, bounds, value, options)` is the behaviour of a dial, a fader or
a number scrubbed: a drag up (or right) changes `value` across `options.scale`
over `travel` units, finely with Shift, past the ends and straight back; the
wheel nudges it; focused, the arrows step it; a double click sets `reset`.
The widget draws itself round it:

```cpp
struct Dial {
    ui::Id id{};
    kin::Rectf bounds{};
    double* value = nullptr;
    ui::DragValueOptions options{.scale = {.min = -24.0, .max = 12.0}, .reset = 0.0};
};
kin::Vec2f measure(const Dial&) { return {64.0f, 64.0f}; }
void run(ui::Context& ctx, Dial& dial) {
    ctx.drag_value(dial.id, dial.bounds, *dial.value, dial.options);
    const kin::Vec2f center{dial.bounds.x + 32.0f, dial.bounds.y + 32.0f};
    const float along = static_cast<float>(dial.options.scale.fraction(*dial.value));
    ctx.arc(center, 26.0f, 135.0f, 405.0f, ctx.theme().palette.surface_alt, {.width = 4.0f, .cap = kin::LineCap::Round});
    ctx.arc(center, 26.0f, 135.0f, 135.0f + 270.0f * along, ctx.theme().palette.accent, {.width = 4.0f, .cap = kin::LineCap::Round});
}
```

A widget that reads the wheel itself calls `take_wheel(id)` each frame, so the
scroll box it sits in does not scroll while the pointer is on it.
`double_click(id)`, called with each click of a region, says whether it was
the second of a double click.

### Text that runs out of room

A label given `TextOverflow::Ellipsis` is as wide as its text while its line has
room and narrower, its text cut with "…", when it has not:

```cpp
ui.label(file_name, text, {}, ui::TextOverflow::Ellipsis);
```

It shrinks (`LayoutStyle::shrink`, CSS's flex-shrink): when a line is too long
for its parent, the children that shrink give up the excess in proportion to
their size, none below its `min`. A box may shrink too, a tab say, and the label
in it is then cut to the tab.

Text in runs of their own colour, such as code coloured by its syntax, is
`AdvancedText` with `TextRun`s; a run starts where its text would in the whole
line:

```cpp
ui::AdvancedText line;
line.content.runs = {keyword_run, name_run};
line.layout_options.wrap = false;
ui.add(std::move(line));
```

### Icons

`ui.svg(document, size, tint)` draws an SVG icon. The document is its text,
embedded in the program or read from a file; it is drawn into a texture once per
document, size and options, and kept as long as the Context. With `.mask` only
its shape counts, in the tint: one-colour glyphs. `.oversample` is texels per
layout unit; left 0, it is the renderer's content scale, so icons are as sharp
as the text on a display with more pixels than the layout has units.
`rasterize_svg()` (`kin/assets/image.hpp`) draws one into an `Image`.

### A window's own title bar

A borderless window (`GameWindowInfo::borderless`) has no system title bar, so
the app draws one. A box with `drags_window()` moves the window when pressed,
except over the interactive boxes inside it (the menu titles, the buttons), and
a double click maximizes it where the system does that. `ui.apply()` hands these
areas to the window each frame, with edges to resize it by. The buttons call
`Window::minimize()`, `toggle_maximized()` and `request_close()`.

### Keyboard

Boxes with `focusable` join the focus order (by `tab_index`, then as built),
and so do `TextInput`s and `TextEdit`s that leave on Tab. Once any box is
focusable, or focus is already in the order, Tab and Shift+Tab move along it
(at `end()`) and the box focused is scrolled into view; Enter or Space clicks the
focused box. A text field hands Tab on itself, and stops editing when Tab takes
focus elsewhere; Enter or Esc leave it. A box with `focus_trap()` (a dialog)
keeps Tab and the focus inside it while it is built. The focus ring shows for
focus the keys gave, not a click's. Widgets that move a selection with the
arrows (`ListView`, `TreeView`, `IconGrid`, `TabBar`, `MenuBar`, `NodeGraph`)
take the keys only while focused; `keys_without_focus` lets one have them while
nothing else is focused too, as `MenuList` and `DialogueView` do.

Key bindings fire from where focus is. A box's `key_context` names a scope;
a binding with that context applies while focus is inside the box, and the
innermost context wins over outer ones and over global bindings (no context):

```cpp
ui.bind({.action = "save", .key = Key::S, .modifiers = KeyModifiers::Ctrl, .context = "editor"});
ui.bind({.action = "down", .key = Key::Down, .context = "explorer"});
// each frame, after ui.begin():
if (ui.action_triggered("save")) save();
```

Modifiers must match exactly. Bindings without Ctrl or Alt are skipped while a
text field has the keys, so typing is not taken for commands.

`games/studio_demo` is an editor-style window built this way, and
`games/nextcode_look` rebuilds next_code's window (menus, icons, title bar).

## Charts

`kin/ui2/charts.hpp` plots numbers the app keeps: a widget holds spans of them
and draws them as they are each frame.

```cpp
std::array<ui::Series, 2> series{{
    {.name = "update", .y = update_ms},
    {.name = "render", .y = render_ms},
}};
ui::Plot plot{.id = ui::make_id("times"), .series = series, .y = {.unit = "ms"}};
ui.widget(plot, ui::LayoutStyle{}.w(ui::grow()).h(200));
```

- `Plot`: series as lines, areas, bars (grouped at each x) or points, with x at
  `x0 + i * dx` or given. Axes fit the data (y rounded out to round values)
  unless given a `Scale`, linear or log. The wheel zooms x around the pointer, a
  drag pans, a double click shows all of it (`zoom`); the readout lists every
  series at the sample nearest the pointer, or a bar's value on the bar. Lines
  of more samples than pixels are drawn as each pixel column's lowest and
  highest. Results: `area`, `view_x` and `view_y` (where the data is, for the
  app's own marks), `pointer_x` and `hovered`.
- `Sparkline`: recent values as a small line, no axes.
- `HeatmapImage` and `Heatmap`: a grid of values as colours (`ColorMap`: Magma,
  Viridis, Gray), kept in a texture updated where it changed. `push_column()`
  scrolls it from the right, a column a frame for a spectrogram:

```cpp
ui::HeatmapImage spectrum{{360, 128}, ui::ColorMap::Magma, {.min = -90.0, .max = 0.0}};
ui::Heatmap view{.id = ui::make_id("spectrum"), .image = &spectrum,
                 .x = {.scale = {.min = -6.0, .max = 0.0}, .unit = "s"},
                 .y = {.scale = {.min = 0.0, .max = 22050.0}, .unit = "Hz"}, .unit = "dB"};
spectrum.push_column(levels_db); // each frame, before the widget draws
ui.widget(view, ui::LayoutStyle{}.fill());
```

Their look follows a data-visualisation method: series take `chart_color(i)`,
eight colours in an order checked for colour-blind readers (stepped for dark
and light surfaces; past eight, one grey: fold the rest into "other"); lines
2 wide with round joins; an area a faint wash of its colour; bars at most 24
wide with a rounded end and a gap between them; hairline grids; text in the
theme's text colours, never a series'; a legend for two or more named series.
`scale.hpp`'s `ticks()` and `tick_label()` give the values axes mark ("0 10k
20k", "100 Hz 1 kHz 10 kHz"). `games/charts_demo` shows them all.

## Text That Scales

TTF fonts are made one of two ways (`kin::ui2::TextRendering`):

- `Bitmap` (the default): glyphs rasterised and hinted at each size drawn. The
  crispest small UI text, but blurred when a transform or camera scales it,
  and an atlas per size (the most recent few are kept, so a scale that
  changes every frame no longer piles them up).
- `Sdf`: one atlas of signed distance fields (48-pixel glyphs, 8 pixels of
  spread), made once per renderer (about 15 ms, on the job system). Sharp at any
  size, scale, zoom or turn, and outlines come nearly free. Metrics scale
  linearly (unhinted), so measuring at twice the scale gives exactly twice the
  size; below about 12 pixels it is softer than `Bitmap`.

```cpp
auto title = kin::ui2::load_ttf_font("fonts/Title.ttf", 24, 1.0f, kin::ui2::TextRendering::Sdf);
auto label = kin::ui2::system_ui_font(16, kin::ui2::TextRendering::Sdf);

// In the world, under the camera: zooms and turns with it, still sharp.
const auto view = renderer.scoped_transform(camera.view_transform());
kin::ui2::draw_text(renderer, label, "Aqua", planet_pos, 1.0f, white);

// An outline 2 units wide, in one pass from the distance field.
kin::ui2::draw_text_outlined(renderer, title, "GAME OVER", pos, 2.0f, gold, 2.0f, ink);
```

`Sdf` text needs `capabilities().distance_fields` (SDL_GPU). Elsewhere it is
drawn as `Bitmap`, and `draw_text_outlined` stamps the text four times under
itself, as widgets' `TextStyle::outline_width` always did. `games/shapes_demo`
names its planets in the world with an `Sdf` font (`--zoom=3 --turn=20` to
see them zoomed and turned).

`Renderer2D::draw_distance_field(texture, quads, style)` draws any
distance-field texture this way: alpha 0.5 on the outline, rising inside and
falling outside over `DistanceFieldStyle::spread` texels.

## Text Beyond ASCII

TTF fonts draw any script: characters that do not join (Latin, Greek,
Cyrillic, CJK) from an atlas that grows as they appear, right-to-left and
shaped text (Arabic, Hebrew, Indic scripts) run by run through HarfBuzz. A
font can borrow the characters it lacks from fallback fonts:

```cpp
auto font = kin::ui2::load_ttf_font("fonts/Lato.ttf", 16,
    kin::ui2::TtfFontOptions{.fallbacks = {"fonts/NotoSansJP.ttf", "fonts/NotoSansArabic.ttf"}});
```

`system_ui_font` falls back to the system's own (`system_fallback_fonts()`,
and per language `system_language_fonts()`). Text fields take input methods
(`ui.apply_text_input(window)` after `ui.end()`) and edit right-to-left text;
`TextOverflow::Ellipsis` and `Shrink` fit labels and buttons.
`wrap_text` breaks CJK text between characters. `set_ui_direction(RightToLeft)`
mirrors layouts for right-to-left languages. See
[localization](localization.md) for translations, text by key and the details.

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

## Themes From Data

A game's look can live in a `.kintheme` file instead of code
(`kin/ui2/theme_file.hpp`), so it can be tuned, and reloaded, without a rebuild.

```ini
# The game's look.
[theme]                 # all optional
base = game             # game | editor | compact
palette = slate         # default | slate | ember | verdant | parchment | high_contrast
system_font = 15        # the system UI font at this size (pt)
transition = 0.08       # seconds widgets take to change state

[colors]                # the game's own colours: for its UI and for whatever else it draws
accent = 5cdebe         # rrggbb or rrggbbaa: no '#', which starts a comment
glow = accent@40        # a colour by name, with alpha 40 (hex); names may come in any order
panel_dark = 0a1015

[tokens]                # kin's colour tokens; text, widgets and surfaces all follow them
surface_panel = panel_dark
solid_accent = accent
text = dcf0e8

[style]                 # sizes: padding_x, padding_y, spacing, row_height, radius, text_scale, ...
radius = 6

[surface.panel]         # one surface: panel, card, header, tooltip, popup, input, button, ...
gradient = 121d22 090e12 vertical
border = 2c464a
radius = 8
shadow = 000000aa 0 8 24   # colour, offset x, offset y, spread [, radius]; or none

[surface.tooltip]
like = panel            # start from another surface
shadow = none

[skin]                  # nine-slice frames for kin's widgets: border fill radius border_width [size]
button = 213135 0f161b 5 1
input = 1b272c 0f161b 4 1
```

```cpp
kin::ui2::ThemeFile look;
std::vector<std::string> errors;
if (!kin::ui2::load_theme_file(root / "data/look.kintheme", look, errors)) {
    report(errors); // "line 12: no token 'txt' (app_background, ...)"
}
look.build_skin(renderer);                    // the [skin] frames, once a renderer exists
ui.set_theme(look.theme);
const kin::Color water = look.color("water"); // the game's own colours
```

- The theme starts from a built-in one (`[theme]`), takes `[style]` and
  `[tokens]`, and is then rebuilt from the tokens (`make_theme_from_tokens`), so a
  token reaches every text style, widget and surface derived from it. Surface
  sections come last and set one surface exactly.
- `[colors]` is for the game as much as for the theme: `ThemeFile::color(name)`
  reads any of them back, so one file can hold a game's whole palette.
- Anything unknown (a section, a key, a colour name) is an error naming its line,
  and a file with errors changes nothing. `parse_theme_file` fits
  `FileWatcher::load_and_watch`, so a theme can reload as it is edited; rebuild
  the skin and set the theme on the contexts after each reload.
- `[skin]` frames are GPU textures: let the `ThemeFile` (and themes copied from
  it) go before the renderer does.

A theme can also use the game's own art and fonts:

```ini
[theme]
font = fonts/game.ttf        # the text's font (a .ttf/.otf of the content)
bold_font = fonts/game-bold.ttf
font_size = 18

[skin]
button = image ui/button.png 12              # an image cut 12 px in from each edge
button_hovered = image ui/button.png 12 0 48 96 48   # [x y w h]: part of it
```

`ThemeFile::build(renderer, content, problems, oversample, image)` reads them
(its images through `image` when given, an `Assets2D`'s say), and applies the
skin. Fonts are drawn `oversample` times as fine (the renderer's content scale).
What can't be read is listed in `problems`, and that part keeps what it had.
`theme_token_names()`, `theme_surface_names()`, `theme_skin_slot_names()` and
the like list what a file can name, for tools.

## Screens From Data

A UI screen (a HUD, a menu, a title screen) can be a `.kinui` file
(`kin/ui2/screen.hpp`): a tree of elements in the look of a `.kintheme`.

```json
{"schema": "kin.ui/1", "theme": "ui/game.kintheme", "size": {"x": 1280, "y": 720},
 "root": {"kind": "box", "width": "grow", "height": "grow", "children": [
   {"id": "score", "kind": "label", "text": "Score: 0", "style": "title", "anchor": "top_left", "offset": {"x": 24, "y": 16}},
   {"id": "menu", "kind": "panel", "anchor": "center", "padding": 20, "gap": 10, "children": [
     {"id": "play", "kind": "button", "text": "Play", "width": 200},
     {"id": "volume", "kind": "slider", "value": 0.8, "width": 200}]}]}}
```

- **Containers:** a `box` holds elements in a column (or a `row`), with
  `padding`, a `gap`, and alignment along it (`justify`) and across it
  (`items`). It stands on a theme `surface` when it names one. A `panel` is a
  box on the panel surface.
- **Leaves:** `label`, `button`, `image` (a path in the content, with a
  `tint`), `progress` (0..1), `toggle`, `slider` (`min`, `max`, `step`),
  `text_input` (with a `hint`) and `spacer`.
- **Dialogue:** a `dialogue` element shows the dialogue the game runs, while
  one runs. See [dialogue](dialogue.md).
- **Sizes:** `width` and `height` are `"fit"`, `"grow"` (or `"grow 2"`),
  points, or `"50%"`.
- **Anchors:** an element with an `anchor` (`top_left` ... `bottom_right`, or
  `center`) is placed over its parent at that point, moved by `offset`, out of
  the flow of its siblings.
- **Text:** text takes a theme text style (`title`, `small`, `muted`,
  `emphasis` or `danger`). A `key` takes the language's text (`kin::tr`).

`parse_screen` and `serialize_screen` read and write the files.
`ui2::UiScreen` draws one with the composition API, inside a context's
`begin()`/`end()`:

```cpp
kin::ui2::UiScreen hud{document};
hud.set_text("score", "Score: " + std::to_string(score)); // kept until set again
hud.draw(ui, {0, 0, width, height}, {.texture = load_image});
if (hud.clicked("play")) start();
const f32 volume = hud.value("volume");
hud.clear_events();
```

- **Results:** `clicked`, `changed`, `value`, `checked` and `text` read what
  the player did at the last draw.
- **Changes:** `set_text`, `set_value`, `set_checked`, `set_visible`,
  `set_image` and `set_enabled` change elements by id.
- **For editors:** `placed()` gives where each element was laid out.

In a scene, an entity with a `UiScreenComponent` (`asset`, `visible`, `order`)
shows a screen, through `kin::SceneScreens` (`kin/ecs/ui_screens.hpp`):

- **Drawing:** it reads the screens and their themes (fonts and art too),
  reads them again when they change, and draws those shown in order over the
  world.
- **Access by id:** the game reaches any shown screen's elements by id:
  `screens.clicked("pause")`, `screens.set_text("score", ...)`.
- **Lua:** entity scripts reach them through `ui`
  (`EntityScripts::set_screens`).

Kin Studio edits both screens and themes.

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

