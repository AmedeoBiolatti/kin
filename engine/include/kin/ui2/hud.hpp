#pragma once

// Widgets for a game's screen: input prompts, dialogue, nameplates, a target
// reticle and a selection rectangle, resource rows, labelled bars and meters,
// inventory slots and floating text. widgets.hpp includes this; on its own it
// brings widgets.hpp.

#include <kin/dialogue/dialogue.hpp>
#include <kin/ui2/widgets.hpp>

#include <string>
#include <vector>

namespace kin::ui2 {

struct IconSlot {
    Id id{};
    Rectf bounds{};
    Sprite icon{};
    Vec2f icon_size{};
    Vec2f size{};
    std::string text;
    i32 count = 0;
    std::string tooltip;
    TextStyle text_style{};
    WidgetStyle style{};
    Color icon_tint = colors::white;
    bool enabled = true;
    bool selected = false;
    bool locked = false;
    i32 z = 0;
    bool drag_enabled = false;
    bool drop_enabled = false;
    std::string drag_payload_type = "icon_slot";
    std::string drag_payload_text;
    u64 drag_payload_value = 0;
    std::string drop_accept_type = "icon_slot";
    Interaction interaction{};
    bool clicked = false;
    bool drag_started = false;
    bool dragging = false;
    bool dropped = false;
    std::string dropped_text;
    u64 dropped_value = 0;
};

struct Meter {
    Rectf bounds{};
    f32 value = 0.0f;
    f32 max = 1.0f;
    i32 segments = 5;
    f32 gap = 2.0f;
    Color fill = Color::rgb(128, 226, 160);
    Color empty = Color::rgb(24, 28, 38);
    Color border = colors::transparent;
};

enum class PromptBindingMode {
    First,
    All,
};

struct PromptOptions {
    PromptBindingMode mode = PromptBindingMode::First;
    std::string separator = " / ";
    bool compact = true;
    bool brackets = false;
    std::string fallback;
};

struct PromptLabel {
    Id id{};
    Rectf bounds{};
    std::string action;
    std::string prompt;
    std::string text;
    PromptOptions prompt_options{};
    Sprite icon{};
    Vec2f icon_size{};
    TextStyle prompt_style{};
    TextStyle text_style{};
    UiPadding padding{8.0f, 4.0f, 8.0f, 4.0f};
    UiPadding chip_padding{8.0f, 4.0f, 8.0f, 4.0f};
    f32 gap = 8.0f;
    std::string separator = " ";
    Color chip_fill = Color::rgba(24, 28, 38, 235);
    Color chip_border = Color::rgb(78, 88, 110);
    bool show_chip = true;
    UiAlign horizontal = UiAlign::Start;
    UiAlign vertical = UiAlign::Center;
};

struct PromptRowItem {
    std::string action;
    std::string prompt;
    std::string text;
    Sprite icon{};
    bool enabled = true;
};

struct PromptRow {
    Id id{};
    Rectf bounds{};
    std::vector<PromptRowItem> items;
    PromptOptions prompt_options{};
    UiLayoutAxis axis = UiLayoutAxis::Horizontal;
    f32 spacing = 12.0f;
    PromptLabel item_style{};
    TextStyle prompt_style{};
    TextStyle text_style{};
    UiPadding padding{8.0f, 4.0f, 8.0f, 4.0f};
    UiAlign horizontal = UiAlign::Start;
    UiAlign vertical = UiAlign::Center;
};

struct DialogBox {
    Rectf bounds{};
    UiNineSlice skin{};
    Color fill = Color::rgba(7, 9, 14, 230);
    Color border = Color::rgb(78, 88, 110);
    std::string title;
    std::string body;
    AdvancedTextContent title_content{};
    AdvancedTextContent body_content{};
    std::string title_markup;
    std::string body_markup;
    AdvancedTextMarkupOptions markup_options{};
    AdvancedTextLayoutOptions title_layout{};
    AdvancedTextLayoutOptions body_layout{};
    TextStyle title_style{.scale = 3.0f};
    TextStyle body_style{};
    UiPadding padding{12.0f, 8.0f, 12.0f, 8.0f};
    f32 gap = 8.0f;
};

struct DialogueView {
    Id id{};
    Rectf bounds{};
    DialogueViewModel model{};
    bool show_disabled_choices = false;
    bool show_history_button = true;
    bool show_close_button = true;
    f32 speaker_height = 26.0f;
    f32 choice_height = 28.0f;
    f32 choice_spacing = 4.0f;
    Vec2f portrait_size{64.0f, 64.0f};
    Sprite portrait{};
    PromptRow prompts{};
    AdvancedTextMarkupOptions markup_options{};
    TextStyle speaker_style{.scale = 2.0f};
    TextStyle body_style{};
    TextStyle choice_style{};
    WidgetStyle style{};
    UiPadding padding{12.0f, 8.0f, 12.0f, 8.0f};
    i32 z = 0;
    bool keys_without_focus = true; // advanced by Enter while nothing else has focus too
    bool advance_requested = false;
    bool skip_requested = false;
    bool history_requested = false;
    bool closed_requested = false;
    bool choice_requested = false;
    i32 choice_index = -1;
    std::string choice_id;
};

struct Nameplate {
    Rectf bounds{};
    std::string label;
    f32 value = 1.0f;
    bool show_bar = true;
    TextStyle text_style{};
    Color fill = Color::rgba(7, 9, 14, 208);
    Color border = Color::rgb(78, 88, 110);
    Color bar_fill = Color::rgb(128, 226, 160);
    Color bar_back = Color::rgb(24, 28, 38);
};

struct SelectionRect {
    Vec2f start{};
    Vec2f end{};
    Color fill = Color::rgba(128, 226, 160, 48);
    Color border = Color::rgb(128, 226, 160);
    Rectf bounds{};
    bool active = false;
};

struct TargetReticle {
    Rectf bounds{};
    bool valid = true;
    bool selected = false;
    bool hovered = false;
    Color valid_color = Color::rgb(128, 226, 160);
    Color invalid_color = Color::rgb(230, 88, 80);
    f32 corner = 8.0f;
};

struct ResourceItem {
    Sprite icon{};
    std::string label;
    std::string value;
    Color color = colors::white;
};

struct ResourceRow {
    Rectf bounds{};
    std::vector<ResourceItem> items;
    Vec2f icon_size{16.0f, 16.0f};
    f32 gap = 6.0f;
    f32 item_gap = 14.0f;
    TextStyle text_style{};
    WidgetStyle style{};
};

struct LabeledBar {
    Rectf bounds{};
    std::string label;
    std::string value_text;
    f32 value = 0.0f;
    TextStyle text_style{};
    Color fill = Color::rgb(128, 226, 160);
    Color background = Color::rgb(24, 28, 38);
    Color border = Color::rgb(78, 88, 110);
};

struct IconMeter {
    Rectf bounds{};
    Sprite icon{};
    Sprite empty_icon{};
    i32 value = 0;
    i32 max = 3;
    Vec2f icon_size{16.0f, 16.0f};
    f32 gap = 4.0f;
    Color fill_tint = colors::white;
    Color empty_tint = Color::rgba(255, 255, 255, 96);
};

struct FloatingText {
    Rectf bounds{};
    Vec2f position{};
    Vec2f velocity{0.0f, -24.0f};
    std::string text;
    f32 age = 0.0f;
    f32 lifetime = 1.0f;
    bool active = true;
    bool fade = true;
    TextStyle text_style{};
    Vec2f rendered_position{};
};

Vec2f measure(const IconSlot& widget);
Vec2f measure(const Meter& widget);
Vec2f measure(const PromptLabel& widget);
Vec2f measure(const PromptRow& widget);
Vec2f measure(const DialogBox& widget);
Vec2f measure(const DialogueView& widget);
Vec2f measure(const Nameplate& widget);
Vec2f measure(const SelectionRect& widget);
Vec2f measure(const TargetReticle& widget);
Vec2f measure(const ResourceRow& widget);
Vec2f measure(const LabeledBar& widget);
Vec2f measure(const IconMeter& widget);
Vec2f measure(const FloatingText& widget);

void run(Context& ctx, IconSlot& widget);
void run(Context& ctx, Meter& widget);
void run(Context& ctx, PromptLabel& widget);
void run(Context& ctx, PromptRow& widget);
void run(Context& ctx, DialogBox& widget);
void run(Context& ctx, DialogueView& widget);
void run(Context& ctx, Nameplate& widget);
void run(Context& ctx, SelectionRect& widget);
void run(Context& ctx, TargetReticle& widget);
void run(Context& ctx, ResourceRow& widget);
void run(Context& ctx, LabeledBar& widget);
void run(Context& ctx, IconMeter& widget);
void run(Context& ctx, FloatingText& widget);

// As the theme makes them look (see theme_styles in widgets.hpp).
void theme_styles(const Theme& theme, IconSlot& widget);
void theme_styles(const Theme& theme, DialogueView& widget);
void theme_styles(const Theme& theme, ResourceRow& widget);
void theme_styles(const Theme& theme, LabeledBar& widget);
void theme_styles(const Theme& theme, FloatingText& widget);

} // namespace kin::ui2
