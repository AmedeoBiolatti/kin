#pragma once

#include <kin/ecs/world.hpp>
#include <kin/ui2/context.hpp>
#include <kin/ui2/theme.hpp>
#include <kin/ui2/widgets.hpp>

#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace kin {

class Localization;

struct Ui2Static {
    i32 marker = 1;
};

struct Ui2Layer {
    i32 order = 0;
};

struct Ui2Root {
    Rectf bounds{};
};

struct Ui2LayoutCache {
    u64 solved_version = 0;
    u64 tree_version = 1;
    Rectf root_bounds{};
    TextDirection solved_direction = TextDirection::LeftToRight; // ui2::ui_direction() when solved
    std::vector<ui2::LayoutNode> nodes;
    std::vector<EcsId> order;
    u64 solve_count = 0;
};

struct Ui2Layout {
    ui2::LayoutStyle style{};
    Rectf solved{};
    // Draw layer after propagating max(own layer, parent's) down the tree, set each frame
    // by solve_ui2_layout. Keeps a child from drawing behind its ancestors.
    i32 effective_order = 0;
    // Sibling sort key between layer and entity id (document order). Entity ids are
    // recycled by flecs, so creation order alone is not stable across destroys —
    // Ui2Sync writes the call index here; builders may set it explicitly.
    u32 sequence = 0;
};

struct Ui2ScrollContainer {
    f32 offset = 0.0f;
    f32 offset_x = 0.0f;
    f32 content_height = 0.0f;
    f32 content_width = 0.0f;
    f32 wheel_step = 48.0f;
    f32 wheel_step_x = 48.0f;
    f32 scrollbar_thickness = 10.0f;
    f32 min_thumb = 18.0f;
    bool show_scrollbar = true;
    bool horizontal = false;
    Color scrollbar_track = Color::rgba(32, 36, 48, 220);
    Color scrollbar_thumb = Color::rgba(150, 166, 196, 230);
    bool changed = false;
    bool dragging = false;
    f32 v_grab_offset = 0.0f;
    f32 h_grab_offset = 0.0f;
    Rectf viewport{};
    Rectf content{};
    Rectf v_track{};
    Rectf v_thumb{};
    Rectf h_track{};
    Rectf h_thumb{};
};

// A value put into a Ui2Text's message ("{gold}").
struct Ui2TextArg {
    std::string name;
    std::variant<f64, std::string> value;
};

// A widget's text by translation key. update_ui2_world puts the active
// localization's text (kin::tr) into the entity's widget - Label::text,
// WrappedText::text, Button::label, Toggle::label, IconButton::label,
// PromptLabel::text, Nameplate::label, LabeledBar::label or
// AdvancedText::markup - and lays the tree out again, when the language
// changes or the component is set anew (a new key or new args).
struct Ui2Text {
    std::string key;
    std::vector<Ui2TextArg> args;
    // What was applied last: by which localization, at which generation.
    const Localization* applied_from = nullptr;
    u64 applied_generation = 0;
};

struct Ui2WorldRenderOptions {
    bool include_static = true;
    bool include_dynamic = true;
};

enum class Ui2WorldRecordKind {
    Surface,
    Panel,
    Label,
    WrappedText,
    Separator,
    Button,
    Toggle,
    Slider,
    ProgressBar,
    Image,
    NineSlicePanel,
    Spacer,
    IconButton,
    IconSlot,
    Meter,
    PromptLabel,
    PromptRow,
    MenuList,
    MenuBar,
    TabBar,
    Splitter,
    DockPanel,
    BreadcrumbBar,
    AssetBrowser,
    StatusBar,
    LogConsole,
    PropertyInspector,
    NodeGraph,
    DialogBox,
    DialogueView,
    Nameplate,
    SelectionRect,
    TargetReticle,
    ScrollView,
    ResourceRow,
    LabeledBar,
    IconMeter,
    RichTextLine,
    TextArea,
    AdvancedText,
    ToastStack,
    FloatingText,
    AnimatedValue,
    TextInput,
    NumberInput,
    ComboBox,
    ColorPicker,
    IconGrid,
    ListView,
    Table,
    TreeView,
    PropertyGrid,
};

struct Ui2WorldRecord {
    Ui2WorldRecordKind kind = Ui2WorldRecordKind::Panel;
    EcsId entity_id = 0;
    i32 order = 0;
    bool clipped = false;
    Rectf clip{};
    // Every record contains one drawable. Keep the named pointer views for
    // source compatibility, but store them in one union instead of reserving
    // a pointer-sized slot for every widget type. The view matching `kind` is
    // the only one that is meaningful for a given record.
    union {
        Ui2Layout* layout = nullptr;
        ui2::Panel* panel;
        ui2::Label* label;
        ui2::WrappedText* wrapped_text;
        ui2::Separator* separator;
        ui2::Button* button;
        ui2::Toggle* toggle;
        ui2::Slider* slider;
        ui2::ProgressBar* progress_bar;
        ui2::Image* image;
        ui2::NineSlicePanel* nine_slice;
        ui2::Spacer* spacer;
        ui2::IconButton* icon_button;
        ui2::IconSlot* icon_slot;
        ui2::Meter* meter;
        ui2::PromptLabel* prompt;
        ui2::PromptRow* prompt_row;
        ui2::MenuList* menu_list;
        ui2::MenuBar* menu_bar;
        ui2::TabBar* tab_bar;
        ui2::Splitter* splitter;
        ui2::DockPanel* dock_panel;
        ui2::BreadcrumbBar* breadcrumb_bar;
        ui2::AssetBrowser* asset_browser;
        ui2::StatusBar* status_bar;
        ui2::LogConsole* log_console;
        ui2::PropertyInspector* property_inspector;
        ui2::NodeGraph* node_graph;
        ui2::DialogBox* dialog;
        ui2::DialogueView* dialogue_view;
        ui2::Nameplate* nameplate;
        ui2::SelectionRect* selection_rect;
        ui2::TargetReticle* target_reticle;
        ui2::ScrollView* scroll_view;
        ui2::ResourceRow* resource_row;
        ui2::LabeledBar* labeled_bar;
        ui2::IconMeter* icon_meter;
        ui2::RichTextLine* rich_text_line;
        ui2::TextArea* text_area;
        ui2::AdvancedText* advanced_text;
        ui2::ToastStack* toast_stack;
        ui2::FloatingText* floating_text;
        ui2::AnimatedValue* animated_value;
        ui2::TextInput* text_input;
        ui2::NumberInput* number_input;
        ui2::ComboBox* combo_box;
        ui2::ColorPicker* color_picker;
        ui2::IconGrid* icon_grid;
        ui2::ListView* list_view;
        ui2::Table* table;
        ui2::TreeView* tree_view;
        ui2::PropertyGrid* property_grid;
    };
    // Type-erased draw callback keeps the public typed pointers available to
    // inspectors while the renderer uses one dispatch path for every widget.
    using DrawFn = void (*)(ui2::Context&, void*, EcsId);
    DrawFn draw = nullptr;
    void* widget = nullptr;
};

struct Ui2WorldRenderScratch {
    std::vector<Ui2WorldRecord> records;
    ui2::LayoutScratch layout_scratch; // reused by solve() across frames

    void clear() { records.clear(); }
    void reserve(std::size_t capacity) { records.reserve(capacity); }
    std::size_t size() const { return records.size(); }
    std::size_t capacity() const { return records.capacity(); }
};

class Ui2EntityBuilder {
public:
    Ui2EntityBuilder() = default;
    explicit Ui2EntityBuilder(EcsEntity entity);

    Ui2EntityBuilder& root(Rectf bounds);
    Ui2EntityBuilder& layout(ui2::LayoutStyle style = {});
    Ui2EntityBuilder& scroll_container(Ui2ScrollContainer scroll = {});
    Ui2EntityBuilder& panel(ui2::Panel widget);
    Ui2EntityBuilder& themed_panel(const ui2::Theme& theme);
    Ui2EntityBuilder& label(ui2::Label widget);
    Ui2EntityBuilder& themed_label(const ui2::Theme& theme, std::string text);
    Ui2EntityBuilder& wrapped_text(ui2::WrappedText widget);
    Ui2EntityBuilder& separator(ui2::Separator widget);
    Ui2EntityBuilder& button(ui2::Button widget);
    Ui2EntityBuilder& themed_button(const ui2::Theme& theme, std::string label);
    Ui2EntityBuilder& themed_danger_button(const ui2::Theme& theme, std::string label);
    Ui2EntityBuilder& toggle(ui2::Toggle widget);
    Ui2EntityBuilder& slider(ui2::Slider widget);
    Ui2EntityBuilder& progress(ui2::ProgressBar widget);
    Ui2EntityBuilder& themed_progress(const ui2::Theme& theme, f32 value);
    Ui2EntityBuilder& image(ui2::Image widget);
    Ui2EntityBuilder& nine_slice(ui2::NineSlicePanel widget);
    Ui2EntityBuilder& spacer(ui2::Spacer widget);
    Ui2EntityBuilder& icon_button(ui2::IconButton widget);
    Ui2EntityBuilder& icon_slot(ui2::IconSlot widget);
    Ui2EntityBuilder& meter(ui2::Meter widget);
    Ui2EntityBuilder& prompt(ui2::PromptLabel widget);
    Ui2EntityBuilder& prompt_row(ui2::PromptRow widget);
    Ui2EntityBuilder& menu_list(ui2::MenuList widget);
    Ui2EntityBuilder& menu_bar(ui2::MenuBar widget);
    Ui2EntityBuilder& tab_bar(ui2::TabBar widget);
    Ui2EntityBuilder& splitter(ui2::Splitter widget);
    Ui2EntityBuilder& dock_panel(ui2::DockPanel widget);
    Ui2EntityBuilder& breadcrumb_bar(ui2::BreadcrumbBar widget);
    Ui2EntityBuilder& asset_browser(ui2::AssetBrowser widget);
    Ui2EntityBuilder& status_bar(ui2::StatusBar widget);
    Ui2EntityBuilder& log_console(ui2::LogConsole widget);
    Ui2EntityBuilder& property_inspector(ui2::PropertyInspector widget);
    Ui2EntityBuilder& node_graph(ui2::NodeGraph widget);
    Ui2EntityBuilder& dialog(ui2::DialogBox widget);
    Ui2EntityBuilder& dialogue_view(ui2::DialogueView widget);
    Ui2EntityBuilder& nameplate(ui2::Nameplate widget);
    Ui2EntityBuilder& selection_rect(ui2::SelectionRect widget);
    Ui2EntityBuilder& target_reticle(ui2::TargetReticle widget);
    Ui2EntityBuilder& scroll_view(ui2::ScrollView widget);
    Ui2EntityBuilder& resource_row(ui2::ResourceRow widget);
    Ui2EntityBuilder& labeled_bar(ui2::LabeledBar widget);
    Ui2EntityBuilder& icon_meter(ui2::IconMeter widget);
    Ui2EntityBuilder& rich_text_line(ui2::RichTextLine widget);
    Ui2EntityBuilder& text_area(ui2::TextArea widget);
    Ui2EntityBuilder& advanced_text(ui2::AdvancedText widget);
    Ui2EntityBuilder& toast_stack(ui2::ToastStack widget);
    Ui2EntityBuilder& floating_text(ui2::FloatingText widget);
    Ui2EntityBuilder& animated_value(ui2::AnimatedValue widget);
    Ui2EntityBuilder& text_input(ui2::TextInput widget);
    Ui2EntityBuilder& number_input(ui2::NumberInput widget);
    Ui2EntityBuilder& combo_box(ui2::ComboBox widget);
    Ui2EntityBuilder& color_picker(ui2::ColorPicker widget);
    Ui2EntityBuilder& icon_grid(ui2::IconGrid widget);
    Ui2EntityBuilder& list_view(ui2::ListView widget);
    Ui2EntityBuilder& table(ui2::Table widget);
    Ui2EntityBuilder& tree_view(ui2::TreeView widget);
    Ui2EntityBuilder& property_grid(ui2::PropertyGrid widget);
    // The widget's text by translation key (see Ui2Text).
    Ui2EntityBuilder& text_key(std::string key, std::vector<Ui2TextArg> args = {});
    Ui2EntityBuilder& layer(i32 order);
    Ui2EntityBuilder& static_ui(bool enabled = true);
    Ui2EntityBuilder& child_of(EcsEntity parent);
    Ui2EntityBuilder& child_of(const Ui2EntityBuilder& parent);

    EcsEntity entity() const { return _entity; }

private:
    EcsEntity _entity;
};

Ui2EntityBuilder ui2_entity(EcsWorld& world, std::string_view name = {});
Ui2EntityBuilder ui2_entity(flecs::world& world, std::string_view name = {});

void register_ui2_components(EcsWorld& world);
void register_ui2_components(flecs::world& world);

void ui2_mark_dirty(EcsEntity entity);
void ui2_mark_dirty(flecs::entity entity);

// Puts translated text into the widgets of entities with Ui2Text where the
// language changed since; update_ui2_world calls it first. Returns how many
// widgets it changed.
std::size_t apply_ui2_text(flecs::world& world);
std::size_t apply_ui2_text(EcsWorld& world);

void update_ui2_world(EcsWorld& world,
                      ui2::Context& ui,
                      Ui2WorldRenderScratch& scratch,
                      Ui2WorldRenderOptions options = {});
void update_ui2_world(flecs::world& world,
                      ui2::Context& ui,
                      Ui2WorldRenderScratch& scratch,
                      Ui2WorldRenderOptions options = {});
void update_ui2_world(EcsWorld& world, ui2::Context& ui, Ui2WorldRenderOptions options = {});
void update_ui2_world(flecs::world& world, ui2::Context& ui, Ui2WorldRenderOptions options = {});

} // namespace kin

namespace kin::ui2 {

LayoutStyle fill();
LayoutStyle overlay(UiAnchor anchor = UiAnchor::TopLeft, Vec2f offset = {});
LayoutStyle fixed_box(f32 width, f32 height);
LayoutStyle column(SizeAxis width = fit(),
                   SizeAxis height = fit(),
                   UiAlign main = UiAlign::Start,
                   UiAlign cross = UiAlign::Start,
                   f32 spacing = 0.0f);
LayoutStyle row(SizeAxis width = fit(),
                SizeAxis height = fit(),
                UiAlign main = UiAlign::Start,
                UiAlign cross = UiAlign::Start,
                f32 spacing = 0.0f);

} // namespace kin::ui2
