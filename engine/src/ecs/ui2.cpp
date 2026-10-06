#include <kin/ecs/ui2.hpp>
#include <kin/ecs/ui2_sync.hpp>

#include "ui2_internal.hpp"

#include <kin/l10n/localization.hpp>

#include <algorithm>
#include <functional>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>

namespace kin {
namespace {

flecs::entity ui2_root_entity_for(flecs::entity entity) {
    for (flecs::entity current = entity; current; current = current.parent()) {
        if (current.has<Ui2Root>()) {
            return current;
        }
    }
    return {};
}

void bump_ui2_root_version(flecs::entity root) {
    if (!root || !root.is_alive()) {
        return;
    }
    Ui2LayoutCache& cache = root.ensure<Ui2LayoutCache>();
    ++cache.tree_version;
}

void mark_ui2_entity_dirty(flecs::entity entity) {
    bump_ui2_root_version(ui2_root_entity_for(entity));
}

template <typename T>
void register_ui2_dirty_observers(flecs::world& world, const char* name, bool observe_set = false) {
    const std::string base = std::string{"ui2_dirty_"} + name;
    world.observer<T>((base + "_add").c_str())
        .event(flecs::OnAdd)
        .each([](flecs::entity entity, T&) {
            mark_ui2_entity_dirty(entity);
        });
    world.observer<T>((base + "_remove").c_str())
        .event(flecs::OnRemove)
        .each([](flecs::entity entity, T&) {
            mark_ui2_entity_dirty(entity);
        });
    if (observe_set) {
        world.observer<T>((base + "_set").c_str())
            .event(flecs::OnSet)
            .each([](flecs::entity entity, T&) {
                mark_ui2_entity_dirty(entity);
            });
    }
}

i32 ui2_order(flecs::entity entity) {
    if (const auto* layer = entity.get<Ui2Layer>()) {
        return layer->order;
    }
    return 0;
}

bool include_ui2_entity(flecs::entity entity, Ui2WorldRenderOptions options) {
    const bool is_static = entity.has<Ui2Static>();
    return (is_static && options.include_static) || (!is_static && options.include_dynamic);
}

std::pair<bool, Rectf> ui2_clip_for(flecs::entity entity) {
    bool clipped = false;
    Rectf clip{};
    flecs::entity parent = entity.parent();
    while (parent) {
        if (parent.has<Ui2ScrollContainer>()) {
            if (const auto* layout = parent.get<Ui2Layout>()) {
                clip = clipped ? ui2::intersect_rect(clip, layout->solved) : layout->solved;
                clipped = true;
            } else if (const auto* root = parent.get<Ui2Root>()) {
                clip = clipped ? ui2::intersect_rect(clip, root->bounds) : root->bounds;
                clipped = true;
            }
        }
        parent = parent.parent();
    }
    return {clipped, clip};
}

ui2::Id ecs_ui2_id(EcsId entity_id) {
    return ui2::make_id(ui2::make_id("ecs.ui2"), entity_id);
}

template <typename T>
void ensure_id(T& widget, EcsId entity_id) {
    if constexpr (requires { widget.id; }) {
        if (!widget.id) {
            widget.id = ecs_ui2_id(entity_id);
        }
    }
}

template <typename Fn>
void for_each_ui2_widget_type(Fn&& fn) {
    fn.template operator()<ui2::Panel>();
    fn.template operator()<ui2::Label>();
    fn.template operator()<ui2::WrappedText>();
    fn.template operator()<ui2::Separator>();
    fn.template operator()<ui2::Button>();
    fn.template operator()<ui2::Toggle>();
    fn.template operator()<ui2::Slider>();
    fn.template operator()<ui2::ProgressBar>();
    fn.template operator()<ui2::Image>();
    fn.template operator()<ui2::NineSlicePanel>();
    fn.template operator()<ui2::Spacer>();
    fn.template operator()<ui2::IconButton>();
    fn.template operator()<ui2::IconSlot>();
    fn.template operator()<ui2::Meter>();
    fn.template operator()<ui2::PromptLabel>();
    fn.template operator()<ui2::PromptRow>();
    fn.template operator()<ui2::MenuList>();
    fn.template operator()<ui2::MenuBar>();
    fn.template operator()<ui2::TabBar>();
    fn.template operator()<ui2::Splitter>();
    fn.template operator()<ui2::DockPanel>();
    fn.template operator()<ui2::BreadcrumbBar>();
    fn.template operator()<ui2::AssetBrowser>();
    fn.template operator()<ui2::StatusBar>();
    fn.template operator()<ui2::LogConsole>();
    fn.template operator()<ui2::PropertyInspector>();
    fn.template operator()<ui2::NodeGraph>();
    fn.template operator()<ui2::DialogBox>();
    fn.template operator()<ui2::DialogueView>();
    fn.template operator()<ui2::Nameplate>();
    fn.template operator()<ui2::SelectionRect>();
    fn.template operator()<ui2::TargetReticle>();
    fn.template operator()<ui2::ScrollView>();
    fn.template operator()<ui2::ResourceRow>();
    fn.template operator()<ui2::LabeledBar>();
    fn.template operator()<ui2::IconMeter>();
    fn.template operator()<ui2::RichTextLine>();
    fn.template operator()<ui2::TextArea>();
    fn.template operator()<ui2::AdvancedText>();
    fn.template operator()<ui2::ToastStack>();
    fn.template operator()<ui2::FloatingText>();
    fn.template operator()<ui2::AnimatedValue>();
    fn.template operator()<ui2::TextInput>();
    fn.template operator()<ui2::NumberInput>();
    fn.template operator()<ui2::ComboBox>();
    fn.template operator()<ui2::ColorPicker>();
    fn.template operator()<ui2::IconGrid>();
    fn.template operator()<ui2::ListView>();
    fn.template operator()<ui2::Table>();
    fn.template operator()<ui2::TreeView>();
    fn.template operator()<ui2::PropertyGrid>();
}

template <typename T>
void draw_ui2_widget(ui2::Context& ui, void* value, EcsId entity_id) {
    T& widget = *static_cast<T*>(value);
    ensure_id(widget, entity_id);
    ui2::run(ui, widget);
}

void draw_ui2_surface(ui2::Context& ui, void* value, EcsId) {
    const Ui2Layout& layout = *static_cast<const Ui2Layout*>(value);
    ui.draw_layout_surface(layout.solved, layout.style);
}

template <typename T>
struct Ui2RecordTraits;

template <typename T, Ui2WorldRecordKind Kind, auto Member>
struct Ui2RecordTraitsBase {
    static constexpr Ui2WorldRecordKind kind = Kind;

    static void assign(Ui2WorldRecord& record, T& widget) {
        record.*Member = &widget;
        record.widget = &widget;
        record.draw = &draw_ui2_widget<T>;
    }
};

#define KIN_UI2_RECORD_TRAIT(TYPE, KIND, FIELD, COMPONENT_NAME, OBSERVER_NAME) \
    template <> struct Ui2RecordTraits<ui2::TYPE> \
        : Ui2RecordTraitsBase<ui2::TYPE, Ui2WorldRecordKind::KIND, &Ui2WorldRecord::FIELD> { \
        static constexpr const char* component_name = COMPONENT_NAME; \
        static constexpr const char* observer_name = OBSERVER_NAME; \
    };

KIN_UI2_RECORD_TRAIT(Panel, Panel, panel, "Ui2Panel", "panel")
KIN_UI2_RECORD_TRAIT(Label, Label, label, "Ui2Label", "label")
KIN_UI2_RECORD_TRAIT(WrappedText, WrappedText, wrapped_text, "Ui2WrappedText", "wrapped_text")
KIN_UI2_RECORD_TRAIT(Separator, Separator, separator, "Ui2Separator", "separator")
KIN_UI2_RECORD_TRAIT(Button, Button, button, "Ui2Button", "button")
KIN_UI2_RECORD_TRAIT(Toggle, Toggle, toggle, "Ui2Toggle", "toggle")
KIN_UI2_RECORD_TRAIT(Slider, Slider, slider, "Ui2Slider", "slider")
KIN_UI2_RECORD_TRAIT(ProgressBar, ProgressBar, progress_bar, "Ui2ProgressBar", "progress_bar")
KIN_UI2_RECORD_TRAIT(Image, Image, image, "Ui2Image", "image")
KIN_UI2_RECORD_TRAIT(NineSlicePanel, NineSlicePanel, nine_slice, "Ui2NineSlicePanel", "nine_slice")
KIN_UI2_RECORD_TRAIT(Spacer, Spacer, spacer, "Ui2Spacer", "spacer")
KIN_UI2_RECORD_TRAIT(IconButton, IconButton, icon_button, "Ui2IconButton", "icon_button")
KIN_UI2_RECORD_TRAIT(IconSlot, IconSlot, icon_slot, "Ui2IconSlot", "icon_slot")
KIN_UI2_RECORD_TRAIT(Meter, Meter, meter, "Ui2Meter", "meter")
KIN_UI2_RECORD_TRAIT(PromptLabel, PromptLabel, prompt, "Ui2PromptLabel", "prompt_label")
KIN_UI2_RECORD_TRAIT(PromptRow, PromptRow, prompt_row, "Ui2PromptRow", "prompt_row")
KIN_UI2_RECORD_TRAIT(MenuList, MenuList, menu_list, "Ui2MenuList", "menu_list")
KIN_UI2_RECORD_TRAIT(MenuBar, MenuBar, menu_bar, "Ui2MenuBar", "menu_bar")
KIN_UI2_RECORD_TRAIT(TabBar, TabBar, tab_bar, "Ui2TabBar", "tab_bar")
KIN_UI2_RECORD_TRAIT(Splitter, Splitter, splitter, "Ui2Splitter", "splitter")
KIN_UI2_RECORD_TRAIT(DockPanel, DockPanel, dock_panel, "Ui2DockPanel", "dock_panel")
KIN_UI2_RECORD_TRAIT(BreadcrumbBar, BreadcrumbBar, breadcrumb_bar, "Ui2BreadcrumbBar", "breadcrumb_bar")
KIN_UI2_RECORD_TRAIT(AssetBrowser, AssetBrowser, asset_browser, "Ui2AssetBrowser", "asset_browser")
KIN_UI2_RECORD_TRAIT(StatusBar, StatusBar, status_bar, "Ui2StatusBar", "status_bar")
KIN_UI2_RECORD_TRAIT(LogConsole, LogConsole, log_console, "Ui2LogConsole", "log_console")
KIN_UI2_RECORD_TRAIT(PropertyInspector, PropertyInspector, property_inspector, "Ui2PropertyInspector", "property_inspector")
KIN_UI2_RECORD_TRAIT(NodeGraph, NodeGraph, node_graph, "Ui2NodeGraph", "node_graph")
KIN_UI2_RECORD_TRAIT(DialogBox, DialogBox, dialog, "Ui2DialogBox", "dialog_box")
KIN_UI2_RECORD_TRAIT(DialogueView, DialogueView, dialogue_view, "Ui2DialogueView", "dialogue_view")
KIN_UI2_RECORD_TRAIT(Nameplate, Nameplate, nameplate, "Ui2Nameplate", "nameplate")
KIN_UI2_RECORD_TRAIT(SelectionRect, SelectionRect, selection_rect, "Ui2SelectionRect", "selection_rect")
KIN_UI2_RECORD_TRAIT(TargetReticle, TargetReticle, target_reticle, "Ui2TargetReticle", "target_reticle")
KIN_UI2_RECORD_TRAIT(ScrollView, ScrollView, scroll_view, "Ui2ScrollView", "scroll_view")
KIN_UI2_RECORD_TRAIT(ResourceRow, ResourceRow, resource_row, "Ui2ResourceRow", "resource_row")
KIN_UI2_RECORD_TRAIT(LabeledBar, LabeledBar, labeled_bar, "Ui2LabeledBar", "labeled_bar")
KIN_UI2_RECORD_TRAIT(IconMeter, IconMeter, icon_meter, "Ui2IconMeter", "icon_meter")
KIN_UI2_RECORD_TRAIT(RichTextLine, RichTextLine, rich_text_line, "Ui2RichTextLine", "rich_text_line")
KIN_UI2_RECORD_TRAIT(TextArea, TextArea, text_area, "Ui2TextArea", "text_area")
KIN_UI2_RECORD_TRAIT(AdvancedText, AdvancedText, advanced_text, "Ui2AdvancedText", "advanced_text")
KIN_UI2_RECORD_TRAIT(ToastStack, ToastStack, toast_stack, "Ui2ToastStack", "toast_stack")
KIN_UI2_RECORD_TRAIT(FloatingText, FloatingText, floating_text, "Ui2FloatingText", "floating_text")
KIN_UI2_RECORD_TRAIT(AnimatedValue, AnimatedValue, animated_value, "Ui2AnimatedValue", "animated_value")
KIN_UI2_RECORD_TRAIT(TextInput, TextInput, text_input, "Ui2TextInput", "text_input")
KIN_UI2_RECORD_TRAIT(NumberInput, NumberInput, number_input, "Ui2NumberInput", "number_input")
KIN_UI2_RECORD_TRAIT(ComboBox, ComboBox, combo_box, "Ui2ComboBox", "combo_box")
KIN_UI2_RECORD_TRAIT(ColorPicker, ColorPicker, color_picker, "Ui2ColorPicker", "color_picker")
KIN_UI2_RECORD_TRAIT(IconGrid, IconGrid, icon_grid, "Ui2IconGrid", "icon_grid")
KIN_UI2_RECORD_TRAIT(ListView, ListView, list_view, "Ui2ListView", "list_view")
KIN_UI2_RECORD_TRAIT(Table, Table, table, "Ui2Table", "table")
KIN_UI2_RECORD_TRAIT(TreeView, TreeView, tree_view, "Ui2TreeView", "tree_view")
KIN_UI2_RECORD_TRAIT(PropertyGrid, PropertyGrid, property_grid, "Ui2PropertyGrid", "property_grid")

#undef KIN_UI2_RECORD_TRAIT

template <typename T>
void append_ui2_widget_records(flecs::world& world,
                               std::vector<Ui2WorldRecord>& records,
                               Ui2WorldRenderOptions options) {
    world.each([&](flecs::entity entity, T& widget) {
        if (!include_ui2_entity(entity, options)) {
            return;
        }
        Ui2WorldRecord record{
            .kind = Ui2RecordTraits<T>::kind,
            .entity_id = static_cast<EcsId>(entity.id()),
            .order = ui2_order(entity),
        };
        Ui2RecordTraits<T>::assign(record, widget);
        records.push_back(record);
    });
}

template <typename Fn>
bool visit_first_ui2_widget(flecs::entity entity, Fn&& fn) {
    bool visited = false;
    for_each_ui2_widget_type([&]<typename T>() {
        if (visited) {
            return;
        }
        if (const auto* widget = entity.get<T>()) {
            std::invoke(fn, *widget);
            visited = true;
        }
    });
    return visited;
}

Vec2f measure_entity(flecs::entity entity) {
    Vec2f result{};
    visit_first_ui2_widget(entity, [&](const auto& widget) {
        result = ui2::measure(widget);
    });
    return result;
}

void set_entity_bounds(flecs::entity entity, Rectf bounds) {
    for_each_ui2_widget_type([&]<typename T>() {
        if (auto* widget = entity.get_mut<T>()) {
            widget->bounds = bounds;
            if constexpr (std::is_same_v<T, ui2::FloatingText>) {
                widget->position = {bounds.x, bounds.y};
            }
        }
    });
}

struct LayoutEntity {
    flecs::entity entity;
    Ui2Layout* layout = nullptr;
};

void solve_ui2_layout(flecs::world& world, ui2::Context& ui, Ui2WorldRenderScratch& scratch) {
    // Prompt widgets show the key bound to their `action`, but their measure reads
    // the stored prompt/items — resolve the binding into the component before layout
    // so the allocated bounds include the chip run() will draw. Change-detected so an
    // idle tree (stable keybinds) does not re-solve every frame.
    world.each([&](flecs::entity entity, ui2::PromptLabel& widget) {
        if (widget.action.empty()) {
            return;
        }
        std::string resolved = ui.prompt_for_action(widget.action, widget.prompt_options);
        if (resolved != widget.prompt) {
            widget.prompt = std::move(resolved);
            ui2_mark_dirty(entity);
        }
    });
    world.each([&](flecs::entity entity, ui2::PromptRow& widget) {
        bool changed = false;
        for (ui2::PromptRowItem& item : widget.items) {
            if (item.action.empty()) {
                continue;
            }
            std::string resolved = ui.prompt_for_action(item.action, widget.prompt_options);
            if (resolved != item.prompt) {
                item.prompt = std::move(resolved);
                changed = true;
            }
        }
        if (changed) {
            ui2_mark_dirty(entity);
        }
    });

    bool layout_dirty = false;
    world.each([&](flecs::entity root_entity, const Ui2Root& root) {
        const Ui2LayoutCache* cache = root_entity.get<Ui2LayoutCache>();
        if (!cache || cache->root_bounds != root.bounds || cache->solved_version != cache->tree_version) {
            layout_dirty = true;
        }
    });
    if (!layout_dirty) {
        return;
    }

    std::vector<LayoutEntity> layouts;
    layouts.reserve(32);
    std::unordered_map<EcsId, std::vector<std::size_t>> children_by_parent;

    world.each([&](flecs::entity entity, Ui2Layout& layout) {
        layouts.push_back({entity, &layout});
    });

    for (std::size_t i = 0; i < layouts.size(); ++i) {
        const flecs::entity parent = layouts[i].entity.parent();
        if (parent) {
            children_by_parent[static_cast<EcsId>(parent.id())].push_back(i);
        }
    }

    for (auto& [parent, children] : children_by_parent) {
        (void)parent;
        std::stable_sort(children.begin(), children.end(), [&](std::size_t lhs, std::size_t rhs) {
            const flecs::entity left = layouts[lhs].entity;
            const flecs::entity right = layouts[rhs].entity;
            const i32 left_order = ui2_order(left);
            const i32 right_order = ui2_order(right);
            if (left_order != right_order) {
                return left_order < right_order;
            }
            if (layouts[lhs].layout->sequence != layouts[rhs].layout->sequence) {
                return layouts[lhs].layout->sequence < layouts[rhs].layout->sequence;
            }
            return left.id() < right.id();
        });
    }

    world.each([&](flecs::entity root_entity, Ui2Root& root) {
        Ui2LayoutCache& cache = root_entity.ensure<Ui2LayoutCache>();
        const bool root_bounds_changed = cache.root_bounds != root.bounds;
        if (!root_bounds_changed && cache.solved_version == cache.tree_version) {
            return;
        }

        std::vector<ui2::LayoutNode>& nodes = cache.nodes;
        nodes.clear();
        cache.order.clear();
        std::vector<std::size_t> node_to_layout;
        std::vector<i32> node_effective_order; // parallel to nodes; parent-clamped draw layer
        nodes.reserve(16);
        cache.order.reserve(16);
        node_to_layout.reserve(16);
        node_effective_order.reserve(16);

        ui2::LayoutStyle root_style;
        root_style.width = ui2::fixed(root.bounds.w);
        root_style.height = ui2::fixed(root.bounds.h);
        root_style.axis = ui2::UiLayoutAxis::Vertical;
        root_style.cross = ui2::UiAlign::Stretch;
        nodes.push_back({root_style, {}, {}, {}, -1});
        cache.order.push_back(static_cast<EcsId>(root_entity.id()));
        node_to_layout.push_back(static_cast<std::size_t>(-1));
        node_effective_order.push_back(0);

        // --- append: iterative pre-order DFS to build the layout node tree ---
        {
            struct AppendFrame { std::size_t layout_index; i32 parent_node; };
            std::vector<AppendFrame> stk;
            const auto root_children = children_by_parent.find(static_cast<EcsId>(root_entity.id()));
            if (root_children == children_by_parent.end()) {
                ui2::solve(nodes, 0, root.bounds, scratch.layout_scratch);
                cache.root_bounds = root.bounds;
                cache.solved_version = cache.tree_version;
                ++cache.solve_count;
                return;
            }
            // Push in reverse so the first child is processed first (stack is LIFO).
            for (auto it = root_children->second.rbegin(); it != root_children->second.rend(); ++it) {
                stk.push_back({*it, 0});
            }
            while (!stk.empty()) {
                auto [layout_index, parent_node] = stk.back();
                stk.pop_back();

                Ui2Layout& layout = *layouts[layout_index].layout;
                const i32 node_index = static_cast<i32>(nodes.size());
                ui2::LayoutNode node;
                node.style = layout.style;
                node.intrinsic = measure_entity(layouts[layout_index].entity);
                node.parent = parent_node;
                nodes.push_back(std::move(node));
                cache.order.push_back(static_cast<EcsId>(layouts[layout_index].entity.id()));
                node_to_layout.push_back(layout_index);

                const i32 own_order = ui2_order(layouts[layout_index].entity);
                const i32 effective_order = std::max(own_order, node_effective_order[static_cast<std::size_t>(parent_node)]);
                node_effective_order.push_back(effective_order);
                layout.effective_order = effective_order;
                nodes[static_cast<std::size_t>(parent_node)].children.push_back(node_index);

                const auto found = children_by_parent.find(static_cast<EcsId>(layouts[layout_index].entity.id()));
                if (found != children_by_parent.end()) {
                    for (auto it = found->second.rbegin(); it != found->second.rend(); ++it) {
                        stk.push_back({*it, node_index});
                    }
                }
            }
        }

        ui2::solve(nodes, 0, root.bounds, scratch.layout_scratch);

        // --- scatter: iterative pre-order DFS to write solved bounds back to entities ---
        // descendant_scroll_hovered is inlined as an iterative sub-search.
        bool runtime_dirty = false;
        {
            struct ScatterFrame { i32 node_index; Vec2f offset; bool clipped; Rectf clip; };
            std::vector<ScatterFrame> stk;
            for (auto it = nodes[0].children.rbegin(); it != nodes[0].children.rend(); ++it) {
                stk.push_back({*it, {}, false, {}});
            }
            while (!stk.empty()) {
                auto [node_index, offset, clipped, clip] = stk.back();
                stk.pop_back();

                const std::size_t layout_index = node_to_layout[static_cast<std::size_t>(node_index)];
                Ui2Layout& layout = *layouts[layout_index].layout;
                flecs::entity entity = layouts[layout_index].entity;
                Rectf solved = nodes[static_cast<std::size_t>(node_index)].solved;
                solved.x -= offset.x;
                solved.y -= offset.y;
                layout.solved = solved;
                set_entity_bounds(entity, layout.solved);

                Vec2f child_offset = offset;
                bool child_clipped = clipped;
                Rectf child_clip = clip;
                if (auto* scroll = entity.get_mut<Ui2ScrollContainer>()) {
                    f32 content_height = 0.0f;
                    f32 content_width = 0.0f;
                    for (i32 child_node : nodes[static_cast<std::size_t>(node_index)].children) {
                        const Rectf child = nodes[static_cast<std::size_t>(child_node)].solved;
                        content_height = std::max(content_height, child.y + child.h - nodes[static_cast<std::size_t>(node_index)].solved.y);
                        content_width = std::max(content_width, child.x + child.w - nodes[static_cast<std::size_t>(node_index)].solved.x);
                    }
                    scroll->content_height = content_height;
                    scroll->content_width = content_width;
                    const f32 bar = scroll->show_scrollbar ? scroll->scrollbar_thickness : 0.0f;
                    scroll->viewport = {
                        solved.x,
                        solved.y,
                        std::max(0.0f, solved.w - bar),
                        std::max(0.0f, solved.h - (scroll->horizontal ? bar : 0.0f)),
                    };

                    // descendant_scroll_hovered: iterative DFS over this node's subtree.
                    // A descendant's on-screen rect is its solved rect minus the scroll offset
                    // accumulated from this container down to it — not just the ancestor `offset`.
                    // Seed direct children with this container's own scroll, and add each nested
                    // scroll container's offset as we descend, so the hit-test matches what is
                    // actually rendered. (Uses the previous frame's offsets, like other hot/hit
                    // resolution; scroll_region updates them just below.)
                    bool any_descendant_scroll_hovered = false;
                    {
                        const Vec2f content_offset{offset.x + scroll->offset_x, offset.y + scroll->offset};
                        struct DfsEntry {
                            i32 node;
                            Vec2f off;
                        };
                        std::vector<DfsEntry> dfs;
                        for (i32 child_node : nodes[static_cast<std::size_t>(node_index)].children) {
                            dfs.push_back({child_node, content_offset});
                        }
                        while (!dfs.empty() && !any_descendant_scroll_hovered) {
                            const DfsEntry entry = dfs.back();
                            dfs.pop_back();
                            const std::size_t cli = node_to_layout[static_cast<std::size_t>(entry.node)];
                            flecs::entity ce = layouts[cli].entity;
                            Rectf cs = nodes[static_cast<std::size_t>(entry.node)].solved;
                            cs.x -= entry.off.x;
                            cs.y -= entry.off.y;
                            Vec2f child_off = entry.off;
                            if (const auto* cscroll = ce.get<Ui2ScrollContainer>()) {
                                if (ui2::contains(cs, ui.pointer())) {
                                    any_descendant_scroll_hovered = true;
                                }
                                child_off.x += cscroll->offset_x;
                                child_off.y += cscroll->offset;
                            }
                            for (i32 grandchild : nodes[static_cast<std::size_t>(entry.node)].children) {
                                dfs.push_back({grandchild, child_off});
                            }
                        }
                    }

                    ui2::ScrollState state{
                        .offset = {scroll->offset_x, scroll->offset},
                        .content_size = {content_width, content_height},
                        .viewport_size = {scroll->viewport.w, scroll->viewport.h},
                    };
                    const ui2::ScrollOptions options{
                        .wheel_step = {scroll->wheel_step_x, scroll->wheel_step},
                        .scrollbar_thickness = scroll->scrollbar_thickness,
                        .min_thumb = scroll->min_thumb,
                        .show_scrollbar = scroll->show_scrollbar,
                        .enabled = true,
                        .wheel_enabled = !any_descendant_scroll_hovered,
                    };
                    ui2::ScrollResult result = ui2::scroll_region(ui,
                                                                  ecs_ui2_id(static_cast<EcsId>(entity.id())),
                                                                  scroll->viewport,
                                                                  state,
                                                                  options,
                                                                  ui2_order(entity),
                                                                  &scroll->v_grab_offset,
                                                                  &scroll->h_grab_offset);
                    scroll->offset_x = result.state.offset.x;
                    scroll->offset = result.state.offset.y;
                    scroll->changed = result.changed;
                    runtime_dirty = runtime_dirty || result.changed;
                    scroll->dragging = result.dragging;
                    scroll->v_track = {scroll->viewport.x + scroll->viewport.w, scroll->viewport.y, bar, scroll->viewport.h};
                    scroll->v_thumb = ui2::layout_scrollbar(scroll->v_track, result.state, ui2::ScrollAxis::Vertical, options).thumb;
                    scroll->h_track = {scroll->viewport.x, scroll->viewport.y + scroll->viewport.h, scroll->viewport.w, bar};
                    scroll->h_thumb = ui2::layout_scrollbar(scroll->h_track, result.state, ui2::ScrollAxis::Horizontal, options).thumb;
                    scroll->content = {scroll->viewport.x - scroll->offset_x, scroll->viewport.y - scroll->offset, content_width, content_height};
                    child_offset.x += scroll->offset_x;
                    child_offset.y += scroll->offset;
                    child_clipped = true;
                    child_clip = clipped ? ui2::intersect_rect(clip, solved) : solved;
                }

                // Push children in reverse order so they're processed left-to-right.
                const auto& children = nodes[static_cast<std::size_t>(node_index)].children;
                for (auto it = children.rbegin(); it != children.rend(); ++it) {
                    stk.push_back({*it, child_offset, child_clipped, child_clip});
                }
            }
        }
        cache.root_bounds = root.bounds;
        cache.solved_version = cache.tree_version;
        ++cache.solve_count;
        if (runtime_dirty) {
            ++cache.tree_version;
        }
    });
}

} // namespace

namespace ui2 {

LayoutStyle fill() {
    return {
        .width = grow(),
        .height = grow(),
    };
}

LayoutStyle overlay(UiAnchor anchor, Vec2f offset) {
    LayoutStyle style = fill();
    style.overlay = true;
    style.anchor = anchor;
    style.anchor_offset = offset;
    return style;
}

LayoutStyle fixed_box(f32 width, f32 height) {
    return {
        .width = fixed(width),
        .height = fixed(height),
    };
}

LayoutStyle column(SizeAxis width, SizeAxis height, UiAlign main, UiAlign cross, f32 spacing) {
    return {
        .width = width,
        .height = height,
        .spacing = spacing,
        .axis = ui2::UiLayoutAxis::Vertical,
        .main = main,
        .cross = cross,
    };
}

LayoutStyle row(SizeAxis width, SizeAxis height, UiAlign main, UiAlign cross, f32 spacing) {
    return {
        .width = width,
        .height = height,
        .spacing = spacing,
        .axis = ui2::UiLayoutAxis::Horizontal,
        .main = main,
        .cross = cross,
    };
}

} // namespace ui2

Ui2EntityBuilder::Ui2EntityBuilder(EcsEntity entity)
    : _entity(entity) {
}

Ui2EntityBuilder& Ui2EntityBuilder::root(Rectf bounds) {
    _entity.set(Ui2Root{.bounds = bounds});
    _entity.raw().ensure<Ui2LayoutCache>();
    ui2_mark_dirty(_entity);
    return *this;
}

Ui2EntityBuilder& Ui2EntityBuilder::layout(ui2::LayoutStyle style) {
    bool changed = true;
    if (const auto* current = _entity.get<Ui2Layout>()) {
        changed = !ecs_ui2_detail::layout_style_equal(current->style, style);
    }
    _entity.set(Ui2Layout{.style = style});
    if (changed) {
        ui2_mark_dirty(_entity);
    }
    return *this;
}

Ui2EntityBuilder& Ui2EntityBuilder::scroll_container(Ui2ScrollContainer scroll) {
    _entity.set(scroll);
    ui2_mark_dirty(_entity);
    return *this;
}

Ui2EntityBuilder& Ui2EntityBuilder::panel(ui2::Panel widget) {
    _entity.set(std::move(widget));
    return *this;
}

Ui2EntityBuilder& Ui2EntityBuilder::themed_panel(const ui2::Theme& theme) {
    return panel(ui2::themed_panel(theme));
}

Ui2EntityBuilder& Ui2EntityBuilder::label(ui2::Label widget) {
    _entity.set(std::move(widget));
    return *this;
}

Ui2EntityBuilder& Ui2EntityBuilder::themed_label(const ui2::Theme& theme, std::string text) {
    return label(ui2::themed_label(theme, std::move(text)));
}

Ui2EntityBuilder& Ui2EntityBuilder::wrapped_text(ui2::WrappedText widget) {
    _entity.set(std::move(widget));
    return *this;
}

Ui2EntityBuilder& Ui2EntityBuilder::separator(ui2::Separator widget) {
    _entity.set(std::move(widget));
    return *this;
}

Ui2EntityBuilder& Ui2EntityBuilder::button(ui2::Button widget) {
    _entity.set(std::move(widget));
    return *this;
}

Ui2EntityBuilder& Ui2EntityBuilder::themed_button(const ui2::Theme& theme, std::string label) {
    return button(ui2::themed_button(theme, std::move(label)));
}

Ui2EntityBuilder& Ui2EntityBuilder::themed_danger_button(const ui2::Theme& theme, std::string label) {
    return button(ui2::themed_danger_button(theme, std::move(label)));
}

Ui2EntityBuilder& Ui2EntityBuilder::toggle(ui2::Toggle widget) {
    _entity.set(std::move(widget));
    return *this;
}

Ui2EntityBuilder& Ui2EntityBuilder::slider(ui2::Slider widget) {
    _entity.set(std::move(widget));
    return *this;
}

Ui2EntityBuilder& Ui2EntityBuilder::progress(ui2::ProgressBar widget) {
    _entity.set(std::move(widget));
    return *this;
}

Ui2EntityBuilder& Ui2EntityBuilder::themed_progress(const ui2::Theme& theme, f32 value) {
    return progress(ui2::themed_progress_bar(theme, value));
}

Ui2EntityBuilder& Ui2EntityBuilder::image(ui2::Image widget) {
    _entity.set(std::move(widget));
    return *this;
}

Ui2EntityBuilder& Ui2EntityBuilder::nine_slice(ui2::NineSlicePanel widget) {
    _entity.set(std::move(widget));
    return *this;
}

Ui2EntityBuilder& Ui2EntityBuilder::spacer(ui2::Spacer widget) {
    _entity.set(std::move(widget));
    return *this;
}

Ui2EntityBuilder& Ui2EntityBuilder::icon_button(ui2::IconButton widget) {
    _entity.set(std::move(widget));
    return *this;
}

Ui2EntityBuilder& Ui2EntityBuilder::icon_slot(ui2::IconSlot widget) {
    _entity.set(std::move(widget));
    return *this;
}

Ui2EntityBuilder& Ui2EntityBuilder::meter(ui2::Meter widget) {
    _entity.set(std::move(widget));
    return *this;
}

Ui2EntityBuilder& Ui2EntityBuilder::prompt(ui2::PromptLabel widget) {
    _entity.set(std::move(widget));
    return *this;
}

Ui2EntityBuilder& Ui2EntityBuilder::prompt_row(ui2::PromptRow widget) {
    _entity.set(std::move(widget));
    return *this;
}

Ui2EntityBuilder& Ui2EntityBuilder::menu_list(ui2::MenuList widget) {
    _entity.set(std::move(widget));
    return *this;
}

Ui2EntityBuilder& Ui2EntityBuilder::menu_bar(ui2::MenuBar widget) {
    _entity.set(std::move(widget));
    return *this;
}

Ui2EntityBuilder& Ui2EntityBuilder::tab_bar(ui2::TabBar widget) {
    _entity.set(std::move(widget));
    return *this;
}

Ui2EntityBuilder& Ui2EntityBuilder::splitter(ui2::Splitter widget) {
    _entity.set(std::move(widget));
    return *this;
}

Ui2EntityBuilder& Ui2EntityBuilder::dock_panel(ui2::DockPanel widget) {
    _entity.set(std::move(widget));
    return *this;
}

Ui2EntityBuilder& Ui2EntityBuilder::breadcrumb_bar(ui2::BreadcrumbBar widget) {
    _entity.set(std::move(widget));
    return *this;
}

Ui2EntityBuilder& Ui2EntityBuilder::asset_browser(ui2::AssetBrowser widget) {
    _entity.set(std::move(widget));
    return *this;
}

Ui2EntityBuilder& Ui2EntityBuilder::status_bar(ui2::StatusBar widget) {
    _entity.set(std::move(widget));
    return *this;
}

Ui2EntityBuilder& Ui2EntityBuilder::log_console(ui2::LogConsole widget) {
    _entity.set(std::move(widget));
    return *this;
}

Ui2EntityBuilder& Ui2EntityBuilder::property_inspector(ui2::PropertyInspector widget) {
    _entity.set(std::move(widget));
    return *this;
}

Ui2EntityBuilder& Ui2EntityBuilder::node_graph(ui2::NodeGraph widget) {
    _entity.set(std::move(widget));
    return *this;
}

Ui2EntityBuilder& Ui2EntityBuilder::dialog(ui2::DialogBox widget) {
    _entity.set(std::move(widget));
    return *this;
}

Ui2EntityBuilder& Ui2EntityBuilder::dialogue_view(ui2::DialogueView widget) {
    _entity.set(std::move(widget));
    return *this;
}

Ui2EntityBuilder& Ui2EntityBuilder::nameplate(ui2::Nameplate widget) {
    _entity.set(std::move(widget));
    return *this;
}

Ui2EntityBuilder& Ui2EntityBuilder::selection_rect(ui2::SelectionRect widget) {
    _entity.set(std::move(widget));
    return *this;
}

Ui2EntityBuilder& Ui2EntityBuilder::target_reticle(ui2::TargetReticle widget) {
    _entity.set(std::move(widget));
    return *this;
}

Ui2EntityBuilder& Ui2EntityBuilder::scroll_view(ui2::ScrollView widget) {
    _entity.set(std::move(widget));
    return *this;
}

Ui2EntityBuilder& Ui2EntityBuilder::resource_row(ui2::ResourceRow widget) {
    _entity.set(std::move(widget));
    return *this;
}

Ui2EntityBuilder& Ui2EntityBuilder::labeled_bar(ui2::LabeledBar widget) {
    _entity.set(std::move(widget));
    return *this;
}

Ui2EntityBuilder& Ui2EntityBuilder::icon_meter(ui2::IconMeter widget) {
    _entity.set(std::move(widget));
    return *this;
}

Ui2EntityBuilder& Ui2EntityBuilder::rich_text_line(ui2::RichTextLine widget) {
    _entity.set(std::move(widget));
    return *this;
}

Ui2EntityBuilder& Ui2EntityBuilder::text_area(ui2::TextArea widget) {
    _entity.set(std::move(widget));
    return *this;
}

Ui2EntityBuilder& Ui2EntityBuilder::advanced_text(ui2::AdvancedText widget) {
    _entity.set(std::move(widget));
    return *this;
}

Ui2EntityBuilder& Ui2EntityBuilder::toast_stack(ui2::ToastStack widget) {
    _entity.set(std::move(widget));
    return *this;
}

Ui2EntityBuilder& Ui2EntityBuilder::floating_text(ui2::FloatingText widget) {
    _entity.set(std::move(widget));
    return *this;
}

Ui2EntityBuilder& Ui2EntityBuilder::animated_value(ui2::AnimatedValue widget) {
    _entity.set(std::move(widget));
    return *this;
}

Ui2EntityBuilder& Ui2EntityBuilder::text_input(ui2::TextInput widget) {
    _entity.set(std::move(widget));
    return *this;
}

Ui2EntityBuilder& Ui2EntityBuilder::number_input(ui2::NumberInput widget) {
    _entity.set(std::move(widget));
    return *this;
}

Ui2EntityBuilder& Ui2EntityBuilder::combo_box(ui2::ComboBox widget) {
    _entity.set(std::move(widget));
    return *this;
}

Ui2EntityBuilder& Ui2EntityBuilder::color_picker(ui2::ColorPicker widget) {
    _entity.set(std::move(widget));
    return *this;
}

Ui2EntityBuilder& Ui2EntityBuilder::icon_grid(ui2::IconGrid widget) {
    _entity.set(std::move(widget));
    return *this;
}

Ui2EntityBuilder& Ui2EntityBuilder::list_view(ui2::ListView widget) {
    _entity.set(std::move(widget));
    return *this;
}

Ui2EntityBuilder& Ui2EntityBuilder::table(ui2::Table widget) {
    _entity.set(std::move(widget));
    return *this;
}

Ui2EntityBuilder& Ui2EntityBuilder::tree_view(ui2::TreeView widget) {
    _entity.set(std::move(widget));
    return *this;
}

Ui2EntityBuilder& Ui2EntityBuilder::property_grid(ui2::PropertyGrid widget) {
    _entity.set(std::move(widget));
    return *this;
}

Ui2EntityBuilder& Ui2EntityBuilder::layer(i32 order) {
    _entity.set(Ui2Layer{.order = order});
    return *this;
}

Ui2EntityBuilder& Ui2EntityBuilder::static_ui(bool enabled) {
    if (enabled) {
        _entity.add<Ui2Static>();
    } else {
        _entity.remove<Ui2Static>();
    }
    return *this;
}

Ui2EntityBuilder& Ui2EntityBuilder::child_of(EcsEntity parent) {
    _entity.child_of(parent);
    ui2_mark_dirty(_entity);
    return *this;
}

Ui2EntityBuilder& Ui2EntityBuilder::child_of(const Ui2EntityBuilder& parent) {
    return child_of(parent.entity());
}

Ui2EntityBuilder ui2_entity(EcsWorld& world, std::string_view name) {
    return Ui2EntityBuilder{world.entity(name)};
}

Ui2EntityBuilder ui2_entity(flecs::world& world, std::string_view name) {
    if (name.empty()) {
        return Ui2EntityBuilder{EcsEntity{world.entity()}};
    }
    return Ui2EntityBuilder{EcsEntity{world.entity(std::string{name}.c_str())}};
}

void ui2_mark_dirty(EcsEntity entity) {
    ui2_mark_dirty(entity.raw());
}

void ui2_mark_dirty(flecs::entity entity) {
    mark_ui2_entity_dirty(entity);
}

namespace {

// Puts `text`'s translation into the entity's widget if the language changed
// since it last did; true if a widget's text changed.
bool apply_ui2_text_to(flecs::entity entity, Ui2Text& text, std::vector<MessageArg>& args) {
    const Localization* l10n = active_localization();
    // Generations start at 1, so a fresh component (0) is always applied.
    const u64 generation = l10n ? l10n->generation() : 1;
    if (text.applied_generation == generation && text.applied_from == l10n) {
        return false;
    }
    text.applied_from = l10n;
    text.applied_generation = generation;
    args.clear();
    for (const Ui2TextArg& arg : text.args) {
        if (const f64* number = std::get_if<f64>(&arg.value)) {
            args.emplace_back(arg.name, *number);
        } else {
            args.emplace_back(arg.name, std::string_view{std::get<std::string>(arg.value)});
        }
    }
    std::string value = l10n ? l10n->tr(text.key, args) : text.key;
    if (auto* label = entity.get_mut<ui2::Label>()) {
        label->text = std::move(value);
    } else if (auto* wrapped = entity.get_mut<ui2::WrappedText>()) {
        wrapped->text = std::move(value);
    } else if (auto* button = entity.get_mut<ui2::Button>()) {
        button->label = std::move(value);
    } else if (auto* toggle = entity.get_mut<ui2::Toggle>()) {
        toggle->label = std::move(value);
    } else if (auto* icon_button = entity.get_mut<ui2::IconButton>()) {
        icon_button->label = std::move(value);
    } else if (auto* prompt = entity.get_mut<ui2::PromptLabel>()) {
        prompt->text = std::move(value);
    } else if (auto* nameplate = entity.get_mut<ui2::Nameplate>()) {
        nameplate->label = std::move(value);
    } else if (auto* bar = entity.get_mut<ui2::LabeledBar>()) {
        bar->label = std::move(value);
    } else if (auto* advanced = entity.get_mut<ui2::AdvancedText>()) {
        advanced->markup = std::move(value);
    } else {
        text.applied_generation = 0; // no widget yet: try again next time
        return false;
    }
    return true;
}

} // namespace

Ui2EntityBuilder& Ui2EntityBuilder::text_key(std::string key, std::vector<Ui2TextArg> args) {
    flecs::entity entity = _entity.raw();
    entity.set(Ui2Text{.key = std::move(key), .args = std::move(args)});
    std::vector<MessageArg> scratch;
    if (apply_ui2_text_to(entity, *entity.get_mut<Ui2Text>(), scratch)) {
        mark_ui2_entity_dirty(entity);
    }
    return *this;
}

std::size_t apply_ui2_text(EcsWorld& world) {
    return apply_ui2_text(world.raw());
}

std::size_t apply_ui2_text(flecs::world& world) {
    std::vector<flecs::entity> changed;
    std::vector<MessageArg> args;
    world.each([&](flecs::entity entity, Ui2Text& text) {
        if (apply_ui2_text_to(entity, text, args)) {
            changed.push_back(entity);
        }
    });
    for (flecs::entity entity : changed) {
        mark_ui2_entity_dirty(entity);
    }
    return changed.size();
}

void register_ui2_components(EcsWorld& world) {
    register_ui2_components(world.raw());
}

void register_ui2_components(flecs::world& world) {
    for_each_ui2_widget_type([&]<typename T>() {
        world.component<T>(Ui2RecordTraits<T>::component_name);
    });
    world.component<Ui2Layer>("Ui2Layer");
    world.component<Ui2Static>("Ui2Static");
    world.component<Ui2Root>("Ui2Root");
    world.component<Ui2LayoutCache>("Ui2LayoutCache");
    world.component<Ui2Layout>("Ui2Layout");
    world.component<Ui2ScrollContainer>("Ui2ScrollContainer");
    world.component<Ui2SyncKey>("Ui2SyncKey");
    world.component<Ui2SyncIndex>("Ui2SyncIndex");
    world.component<Ui2Text>("Ui2Text");

    register_ui2_dirty_observers<Ui2Root>(world, "root", true);
    register_ui2_dirty_observers<Ui2Layout>(world, "layout", true);
    register_ui2_dirty_observers<Ui2ScrollContainer>(world, "scroll_container", true);
    for_each_ui2_widget_type([&]<typename T>() {
        register_ui2_dirty_observers<T>(world, Ui2RecordTraits<T>::observer_name);
    });
}

void update_ui2_world(EcsWorld& world, ui2::Context& ui, Ui2WorldRenderScratch& scratch, Ui2WorldRenderOptions options) {
    update_ui2_world(world.raw(), ui, scratch, options);
}

void update_ui2_world(flecs::world& world, ui2::Context& ui, Ui2WorldRenderScratch& scratch, Ui2WorldRenderOptions options) {
    std::vector<Ui2WorldRecord>& records = scratch.records;
    records.clear();
    apply_ui2_text(world);
    solve_ui2_layout(world, ui, scratch);

    // Container surfaces participate in the same
    // ordered draw stream as widgets, mirroring the immediate front-end. Only entities
    // that actually paint something are emitted, so transparent containers cost nothing.
    world.each([&](flecs::entity entity, Ui2Layout& layout) {
        if (!include_ui2_entity(entity, options)) {
            return;
        }
        if (!layout.style.draw_surface) {
            return;
        }
        records.push_back({
            .kind = Ui2WorldRecordKind::Surface,
            .entity_id = static_cast<EcsId>(entity.id()),
            .order = ui2_order(entity),
            .layout = &layout,
            .draw = &draw_ui2_surface,
            .widget = &layout,
        });
    });

    for_each_ui2_widget_type([&]<typename T>() {
        append_ui2_widget_records<T>(world, records, options);
    });

    for (Ui2WorldRecord& record : records) {
        flecs::entity entity = world.entity(record.entity_id);
        auto [clipped, clip] = ui2_clip_for(entity);
        record.clipped = clipped;
        record.clip = clip;
        // Sort by the parent-clamped layer so a child never draws behind its ancestors.
        // Entities outside any layout tree keep their own order from emission.
        if (const auto* layout = entity.get<Ui2Layout>()) {
            record.order = layout->effective_order;
        }
    }

    const auto draw_slot = [](const Ui2WorldRecord& record) {
        // A node's own surface draws before its widget when both share an entity/order.
        return record.kind == Ui2WorldRecordKind::Surface ? 0 : 1;
    };
    std::stable_sort(records.begin(), records.end(), [&](const Ui2WorldRecord& lhs, const Ui2WorldRecord& rhs) {
        if (lhs.order != rhs.order) {
            return lhs.order < rhs.order;
        }
        if (lhs.entity_id != rhs.entity_id) {
            return lhs.entity_id < rhs.entity_id;
        }
        return draw_slot(lhs) < draw_slot(rhs);
    });

    for (Ui2WorldRecord& record : records) {
        if (record.clipped) {
            ui.push_clip(record.clip);
        }
        if (record.draw != nullptr) {
            record.draw(ui, record.widget, record.entity_id);
        }
        if (record.clipped) {
            ui.pop_clip();
        }
    }

    world.each([&](flecs::entity entity, Ui2ScrollContainer& scroll) {
        if (!include_ui2_entity(entity, options) || !scroll.show_scrollbar || scroll.scrollbar_thickness <= 0.0f) {
            return;
        }
        const ui2::ScrollOptions scroll_options{
            .wheel_step = {scroll.wheel_step_x, scroll.wheel_step},
            .scrollbar_thickness = scroll.scrollbar_thickness,
            .min_thumb = scroll.min_thumb,
            .show_scrollbar = scroll.show_scrollbar,
            .enabled = true,
        };
        const ui2::ScrollState state{
            .offset = {scroll.offset_x, scroll.offset},
            .content_size = {scroll.content_width, scroll.content_height},
            .viewport_size = {scroll.viewport.w, scroll.viewport.h},
        };
        const ui2::ScrollbarLayout vertical = ui2::layout_scrollbar(scroll.v_track, state, ui2::ScrollAxis::Vertical, scroll_options);
        if (vertical.scrollable) {
            ui.fill_rect(scroll.v_track, scroll.scrollbar_track);
            ui.fill_rect(vertical.thumb, scroll.scrollbar_thumb);
        }
        if (scroll.horizontal) {
            const ui2::ScrollbarLayout horizontal = ui2::layout_scrollbar(scroll.h_track, state, ui2::ScrollAxis::Horizontal, scroll_options);
            if (horizontal.scrollable) {
                ui.fill_rect(scroll.h_track, scroll.scrollbar_track);
                ui.fill_rect(horizontal.thumb, scroll.scrollbar_thumb);
            }
        }
    });
}

void update_ui2_world(EcsWorld& world, ui2::Context& ui, Ui2WorldRenderOptions options) {
    update_ui2_world(world.raw(), ui, options);
}

void update_ui2_world(flecs::world& world, ui2::Context& ui, Ui2WorldRenderOptions options) {
    thread_local Ui2WorldRenderScratch scratch;
    update_ui2_world(world, ui, scratch, options);
}

} // namespace kin
