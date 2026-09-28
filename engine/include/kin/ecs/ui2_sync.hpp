#pragma once

#include <kin/ecs/ui2.hpp>

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace kin {

struct Ui2SyncKey {
    u64 key = 0;
    u64 last_visit = 0;
};

struct Ui2SyncIndex {
    std::unordered_map<u64, EcsId> by_key;
    u64 frame = 0;
    bool keep_unvisited = false;
};

class Ui2Sync {
public:
    Ui2Sync() = default;

    Ui2Sync& key(std::string_view value);
    Ui2Sync& keep_unvisited(bool enabled = true);
    // Layout style for the next widget call (one-shot, like key()). Without it,
    // leaf widgets get the default fit-sized LayoutStyle.
    Ui2Sync& layout(ui2::LayoutStyle style);

    void begin_column(ui2::LayoutStyle style = ui2::column(ui2::grow(), ui2::fit(), ui2::UiAlign::Start, ui2::UiAlign::Stretch));
    void begin_row(ui2::LayoutStyle style = ui2::row(ui2::fit(), ui2::fit(), ui2::UiAlign::Start, ui2::UiAlign::Center));
    void begin_scroll(ui2::LayoutStyle style, Ui2ScrollContainer scroll = {});
    void end_container();
    void end_column() { end_container(); }
    void end_row() { end_container(); }
    void end_scroll() { end_container(); }

    ui2::Panel panel(ui2::Panel widget = {});
    ui2::Label label(ui2::Label widget);
    ui2::WrappedText wrapped_text(ui2::WrappedText widget);
    ui2::Separator separator(ui2::Separator widget = {});
    ui2::Button button(ui2::Button widget);
    ui2::Toggle toggle(ui2::Toggle widget);
    ui2::Slider slider(ui2::Slider widget);
    ui2::ProgressBar progress(ui2::ProgressBar widget);
    ui2::Image image(ui2::Image widget);
    ui2::Spacer spacer(ui2::Spacer widget = {});
    ui2::IconButton icon_button(ui2::IconButton widget);
    ui2::Meter meter(ui2::Meter widget);
    ui2::TextInput text_input(ui2::TextInput widget = {});
    ui2::ListView list_view(ui2::ListView widget);
    ui2::TabBar tab_bar(ui2::TabBar widget);
    ui2::MenuList menu_list(ui2::MenuList widget);

    void end();

    EcsEntity root() const { return _root; }

private:
    friend Ui2Sync begin_ui2_sync(EcsWorld& world, std::string_view name, Rectf bounds);
    friend Ui2Sync begin_ui2_sync(flecs::world& world, std::string_view name, Rectf bounds);

    struct Frame {
        EcsEntity parent;
        u64 key = 0;
        std::unordered_map<u64, u32> occurrences;
        u32 next_sequence = 0; // document order within this container
    };

    explicit Ui2Sync(flecs::world& world, std::string_view name, Rectf bounds);

    u64 next_key(std::string_view kind, std::string_view fallback);
    EcsEntity upsert(std::string_view kind, std::string_view fallback, const ui2::LayoutStyle* style);
    void visit(EcsEntity entity, u64 key);
    void mark_dirty();

    template <typename T>
    T sync_widget(std::string_view kind, std::string_view fallback, T widget);

    flecs::world* _world = nullptr;
    EcsEntity _root;
    Ui2SyncIndex* _index = nullptr;
    std::vector<Frame> _stack;
    std::string _next_key;
    ui2::LayoutStyle _next_layout{};
    bool _has_next_layout = false;
    u64 _frame = 0;
    bool _ended = false;
};

Ui2Sync begin_ui2_sync(EcsWorld& world, std::string_view name, Rectf bounds);
Ui2Sync begin_ui2_sync(flecs::world& world, std::string_view name, Rectf bounds);

} // namespace kin
