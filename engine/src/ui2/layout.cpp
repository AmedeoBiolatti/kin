#include <kin/ui2/layout.hpp>

#include <algorithm>
#include <cmath>

namespace kin::ui2 {
namespace {

bool horizontal(UiLayoutAxis axis) {
    return axis == UiLayoutAxis::Horizontal;
}

f32 main_of(Vec2f v, UiLayoutAxis axis) {
    return horizontal(axis) ? v.x : v.y;
}

f32 cross_of(Vec2f v, UiLayoutAxis axis) {
    return horizontal(axis) ? v.y : v.x;
}

const SizeAxis& main_axis(const LayoutStyle& style, UiLayoutAxis axis) {
    return horizontal(axis) ? style.width : style.height;
}

const SizeAxis& cross_axis(const LayoutStyle& style, UiLayoutAxis axis) {
    return horizontal(axis) ? style.height : style.width;
}

f32 main_margin(const UiPadding& margin, UiLayoutAxis axis) {
    return horizontal(axis) ? margin.left + margin.right : margin.top + margin.bottom;
}

f32 cross_margin(const UiPadding& margin, UiLayoutAxis axis) {
    return horizontal(axis) ? margin.top + margin.bottom : margin.left + margin.right;
}

f32 main_margin_start(const UiPadding& margin, UiLayoutAxis axis) {
    return horizontal(axis) ? margin.left : margin.top;
}

f32 cross_margin_start(const UiPadding& margin, UiLayoutAxis axis) {
    return horizontal(axis) ? margin.top : margin.left;
}

f32 clamp_axis(const SizeAxis& spec, f32 value) {
    if (spec.min > 0.0f) {
        value = std::max(value, spec.min);
    }
    if (spec.max > 0.0f) {
        value = std::min(value, spec.max);
    }
    return std::max(0.0f, value);
}

// Size a child contributes when measuring a Fit parent: Grow children contribute only
// their min so they never inflate the parent.
f32 child_measure_extent(const LayoutNode& child, const SizeAxis& spec, f32 intrinsic_extent) {
    if (spec.mode == SizeMode::Grow) {
        return spec.min;
    }
    if (spec.mode == SizeMode::Percent) {
        return spec.min;
    }
    if (spec.mode == SizeMode::Fixed) {
        return clamp_axis(spec, spec.value); // match resolve_axis: honor min/max on Fixed
    }
    return clamp_axis(spec, intrinsic_extent);
}

f32 resolve_axis(const SizeAxis& spec, f32 intrinsic_extent, f32 available_extent) {
    switch (spec.mode) {
    case SizeMode::Fixed:
        return clamp_axis(spec, spec.value);
    case SizeMode::Fit:
        return clamp_axis(spec, intrinsic_extent);
    case SizeMode::Grow:
        return clamp_axis(spec, available_extent);
    case SizeMode::Percent:
        return clamp_axis(spec, available_extent * spec.value);
    }
    return 0.0f;
}

f32 align_offset(f32 available, f32 size, UiAlign align) {
    switch (align) {
    case UiAlign::Center:
        return (available - size) * 0.5f;
    case UiAlign::End:
        return available - size;
    case UiAlign::Start:
    case UiAlign::Stretch:
        return 0.0f;
    }
    return 0.0f;
}

Rectf axis_rect(UiLayoutAxis axis, f32 main_pos, f32 cross_pos, f32 main_size, f32 cross_size) {
    if (horizontal(axis)) {
        return {main_pos, cross_pos, main_size, cross_size};
    }
    return {cross_pos, main_pos, cross_size, main_size};
}

f32 child_outer_main_for_break(const LayoutNode& child, UiLayoutAxis axis, f32 content_main) {
    const SizeAxis& spec = main_axis(child.style, axis);
    const f32 margin = main_margin(child.style.margin, axis);
    if (spec.mode == SizeMode::Grow) {
        return spec.min + margin;
    }
    const f32 available = std::max(0.0f, content_main - margin);
    return resolve_axis(spec, main_of(child.intrinsic, axis), available) + margin;
}

f32 child_outer_cross_for_line(const LayoutNode& child, UiLayoutAxis axis, f32 content_cross) {
    const SizeAxis& spec = cross_axis(child.style, axis);
    const f32 margin = cross_margin(child.style.margin, axis);
    if (spec.mode == SizeMode::Grow) {
        return spec.min + margin;
    }
    const f32 available = std::max(0.0f, content_cross - margin);
    return resolve_axis(spec, cross_of(child.intrinsic, axis), available) + margin;
}

void measure_grid(LayoutNode& node, const std::vector<LayoutNode>& nodes, LayoutScratch& scratch) {
    const i32 cols_i = std::max(1, node.style.grid_columns);
    const std::size_t cols = static_cast<std::size_t>(cols_i);
    std::size_t flow_count = 0;
    for (const i32 child_index : node.children) {
        if (!nodes[static_cast<std::size_t>(child_index)].style.overlay) {
            ++flow_count;
        }
    }

    const std::size_t rows = flow_count == 0 ? 0 : (flow_count + cols - 1) / cols;
    scratch.col_size.assign(cols, 0.0f);
    scratch.row_size.assign(rows, 0.0f);

    std::size_t flow = 0;
    for (const i32 child_index : node.children) {
        const LayoutNode& child = nodes[static_cast<std::size_t>(child_index)];
        if (child.style.overlay) {
            continue;
        }
        const std::size_t col = flow % cols;
        const std::size_t row = flow / cols;
        const f32 w = child_measure_extent(child, child.style.width, child.intrinsic.x) +
                      child.style.margin.left + child.style.margin.right;
        const f32 h = child_measure_extent(child, child.style.height, child.intrinsic.y) +
                      child.style.margin.top + child.style.margin.bottom;
        scratch.col_size[col] = std::max(scratch.col_size[col], w);
        scratch.row_size[row] = std::max(scratch.row_size[row], h);
        ++flow;
    }

    f32 content_w = 0.0f;
    for (f32 col : scratch.col_size) {
        content_w += col;
    }
    if (flow_count > 0 && cols > 1) {
        content_w += node.style.spacing * static_cast<f32>(cols - 1);
    }

    f32 content_h = 0.0f;
    for (f32 row : scratch.row_size) {
        content_h += row;
    }
    if (rows > 1) {
        content_h += node.style.line_spacing * static_cast<f32>(rows - 1);
    }

    node.intrinsic = {
        content_w + node.style.padding.left + node.style.padding.right,
        content_h + node.style.padding.top + node.style.padding.bottom,
    };
}

void measure(std::vector<LayoutNode>& nodes, LayoutScratch& scratch, const std::vector<f32>* wrapped_cross = nullptr) {
    for (i32 i = static_cast<i32>(nodes.size()) - 1; i >= 0; --i) {
        LayoutNode& node = nodes[static_cast<std::size_t>(i)];
        Vec2f measured = node.intrinsic; // leaves arrive pre-measured

        if (!node.children.empty()) {
            if (node.style.grid_columns > 0) {
                measure_grid(node, nodes, scratch);
                measured = node.intrinsic;
            } else {
                const UiLayoutAxis axis = node.style.axis;
                f32 main_sum = 0.0f;
                f32 cross_max = 0.0f;
                for (const i32 child_index : node.children) {
                    const LayoutNode& child = nodes[static_cast<std::size_t>(child_index)];
                    if (child.style.overlay) {
                        continue;
                    }
                    main_sum += main_margin(child.style.margin, axis) +
                                child_measure_extent(child, main_axis(child.style, axis), main_of(child.intrinsic, axis));
                    cross_max = std::max(cross_max,
                                         cross_margin(child.style.margin, axis) +
                                             child_measure_extent(child, cross_axis(child.style, axis), cross_of(child.intrinsic, axis)));
                }
                const auto flow_count = std::ranges::count_if(node.children, [&](i32 child_index) {
                    return !nodes[static_cast<std::size_t>(child_index)].style.overlay;
                });
                if (flow_count > 1) {
                    main_sum += node.style.spacing * static_cast<f32>(flow_count - 1);
                }

                const f32 content_x = horizontal(axis) ? main_sum : cross_max;
                const f32 content_y = horizontal(axis) ? cross_max : main_sum;
                measured = {
                    content_x + node.style.padding.left + node.style.padding.right,
                    content_y + node.style.padding.top + node.style.padding.bottom,
                };
            }
        }

        if (wrapped_cross && node.style.wrap && node.style.grid_columns <= 0 &&
            static_cast<std::size_t>(i) < wrapped_cross->size() &&
            (*wrapped_cross)[static_cast<std::size_t>(i)] >= 0.0f) {
            if (horizontal(node.style.axis)) {
                measured.y = (*wrapped_cross)[static_cast<std::size_t>(i)];
            } else {
                measured.x = (*wrapped_cross)[static_cast<std::size_t>(i)];
            }
        }

        if (node.style.width.mode == SizeMode::Fixed) {
            measured.x = node.style.width.value;
        }
        if (node.style.height.mode == SizeMode::Fixed) {
            measured.y = node.style.height.value;
        }
        measured.x = clamp_axis(node.style.width, measured.x);
        measured.y = clamp_axis(node.style.height, measured.y);
        node.intrinsic = measured;
    }
}

void arrange_line(std::vector<LayoutNode>& nodes,
                  const std::vector<i32>& flow_children,
                  std::size_t begin,
                  std::size_t end,
                  UiLayoutAxis axis,
                  f32 content_main_origin,
                  f32 line_cross_origin,
                  f32 content_main,
                  f32 line_cross_size,
                  const LayoutStyle& style,
                  LayoutScratch& scratch) {
    const std::size_t count = end - begin;
    if (count == 0) {
        return;
    }

    std::vector<f32>& main_size = scratch.main_size;
    std::vector<f32>& cross_size = scratch.cross_size;
    main_size.assign(count, 0.0f);
    cross_size.assign(count, 0.0f);
    f32 reserved_main = 0.0f;
    f32 grow_weight = 0.0f;

    for (std::size_t k = 0; k < count; ++k) {
        const LayoutNode& child = nodes[static_cast<std::size_t>(flow_children[begin + k])];
        const SizeAxis& spec = main_axis(child.style, axis);
        if (spec.mode == SizeMode::Grow) {
            grow_weight += spec.value > 0.0f ? spec.value : 1.0f;
            reserved_main += main_margin(child.style.margin, axis);
        } else {
            const f32 available = std::max(0.0f, content_main - main_margin(child.style.margin, axis));
            main_size[k] = resolve_axis(spec, main_of(child.intrinsic, axis), available);
            reserved_main += main_size[k];
            reserved_main += main_margin(child.style.margin, axis);
        }
    }

    const f32 spacing_total = count > 0 ? style.spacing * static_cast<f32>(count - 1) : 0.0f;
    const f32 leftover = std::max(0.0f, content_main - reserved_main - spacing_total);
    if (grow_weight > 0.0f) {
        for (std::size_t k = 0; k < count; ++k) {
            const LayoutNode& child = nodes[static_cast<std::size_t>(flow_children[begin + k])];
            const SizeAxis& spec = main_axis(child.style, axis);
            if (spec.mode == SizeMode::Grow) {
                const f32 weight = spec.value > 0.0f ? spec.value : 1.0f;
                main_size[k] = clamp_axis(spec, leftover * (weight / grow_weight));
            }
        }
    }

    for (std::size_t k = 0; k < count; ++k) {
        const LayoutNode& child = nodes[static_cast<std::size_t>(flow_children[begin + k])];
        const SizeAxis& spec = cross_axis(child.style, axis);
        const f32 available_cross = std::max(0.0f, line_cross_size - cross_margin(child.style.margin, axis));
        if (style.cross == UiAlign::Stretch || spec.mode == SizeMode::Grow) {
            cross_size[k] = clamp_axis(spec, available_cross);
        } else {
            cross_size[k] = resolve_axis(spec, cross_of(child.intrinsic, axis), available_cross);
        }
        cross_size[k] = std::min(cross_size[k], available_cross);
    }

    f32 total_main = spacing_total;
    for (std::size_t k = 0; k < count; ++k) {
        const LayoutNode& child = nodes[static_cast<std::size_t>(flow_children[begin + k])];
        total_main += main_size[k] + main_margin(child.style.margin, axis);
    }

    f32 cursor = content_main_origin + align_offset(content_main, total_main, style.main);
    for (std::size_t k = 0; k < count; ++k) {
        LayoutNode& child = nodes[static_cast<std::size_t>(flow_children[begin + k])];
        const f32 outer_cross = cross_size[k] + cross_margin(child.style.margin, axis);
        const f32 cross_pos = line_cross_origin +
                              (style.cross == UiAlign::Stretch
                                   ? 0.0f
                                   : align_offset(line_cross_size, outer_cross, style.cross)) +
                              cross_margin_start(child.style.margin, axis);
        child.solved = axis_rect(axis,
                                 cursor + main_margin_start(child.style.margin, axis),
                                 cross_pos,
                                 main_size[k],
                                 cross_size[k]);
        cursor += main_size[k] + main_margin(child.style.margin, axis) + style.spacing;
    }
}

void arrange_grid(std::vector<LayoutNode>& nodes, LayoutNode& node, const std::vector<i32>& flow_children, Rectf content, LayoutScratch& scratch) {
    const std::size_t count = flow_children.size();
    if (count == 0) {
        return;
    }

    const std::size_t cols = static_cast<std::size_t>(std::max(1, node.style.grid_columns));
    const std::size_t rows = (count + cols - 1) / cols;
    scratch.col_size.assign(cols, 0.0f);
    scratch.row_size.assign(rows, 0.0f);
    std::vector<f32>& grow_weight = scratch.line_cross_extent;
    grow_weight.assign(cols, 0.0f);

    for (std::size_t k = 0; k < count; ++k) {
        const LayoutNode& child = nodes[static_cast<std::size_t>(flow_children[k])];
        const std::size_t col = k % cols;
        const std::size_t row = k / cols;
        const f32 outer_w = child_measure_extent(child, child.style.width, child.intrinsic.x) +
                            child.style.margin.left + child.style.margin.right;
        const f32 outer_h = child_measure_extent(child, child.style.height, child.intrinsic.y) +
                            child.style.margin.top + child.style.margin.bottom;
        scratch.col_size[col] = std::max(scratch.col_size[col], outer_w);
        scratch.row_size[row] = std::max(scratch.row_size[row], outer_h);
        if (child.style.width.mode == SizeMode::Grow) {
            grow_weight[col] = std::max(grow_weight[col], child.style.width.value > 0.0f ? child.style.width.value : 1.0f);
        }
    }

    f32 fixed_width = 0.0f;
    f32 total_weight = 0.0f;
    for (std::size_t col = 0; col < cols; ++col) {
        if (grow_weight[col] > 0.0f) {
            total_weight += grow_weight[col];
        } else {
            fixed_width += scratch.col_size[col];
        }
    }
    const f32 gaps = cols > 1 ? node.style.spacing * static_cast<f32>(cols - 1) : 0.0f;
    const f32 leftover = std::max(0.0f, content.w - fixed_width - gaps);
    if (total_weight > 0.0f) {
        for (std::size_t col = 0; col < cols; ++col) {
            if (grow_weight[col] > 0.0f) {
                scratch.col_size[col] = std::max(scratch.col_size[col], leftover * (grow_weight[col] / total_weight));
            }
        }
    }

    std::vector<f32>& col_origin = scratch.main_size;
    std::vector<f32>& row_origin = scratch.cross_size;
    col_origin.assign(cols, content.x);
    row_origin.assign(rows, content.y);
    for (std::size_t col = 1; col < cols; ++col) {
        col_origin[col] = col_origin[col - 1] + scratch.col_size[col - 1] + node.style.spacing;
    }
    for (std::size_t row = 1; row < rows; ++row) {
        row_origin[row] = row_origin[row - 1] + scratch.row_size[row - 1] + node.style.line_spacing;
    }

    for (std::size_t k = 0; k < count; ++k) {
        LayoutNode& child = nodes[static_cast<std::size_t>(flow_children[k])];
        const std::size_t col = k % cols;
        const std::size_t row = k / cols;
        const Rectf cell{col_origin[col], row_origin[row], scratch.col_size[col], scratch.row_size[row]};
        const f32 avail_w = std::max(0.0f, cell.w - child.style.margin.left - child.style.margin.right);
        const f32 avail_h = std::max(0.0f, cell.h - child.style.margin.top - child.style.margin.bottom);
        f32 w = 0.0f;
        if (node.style.main == UiAlign::Stretch || child.style.width.mode == SizeMode::Grow) {
            w = clamp_axis(child.style.width, avail_w);
        } else {
            w = resolve_axis(child.style.width, child.intrinsic.x, avail_w);
        }
        f32 h = 0.0f;
        if (node.style.cross == UiAlign::Stretch || child.style.height.mode == SizeMode::Grow) {
            h = clamp_axis(child.style.height, avail_h);
        } else {
            h = resolve_axis(child.style.height, child.intrinsic.y, avail_h);
        }
        w = std::min(w, avail_w);
        h = std::min(h, avail_h);
        const f32 x = cell.x + child.style.margin.left +
                      (node.style.main == UiAlign::Stretch ? 0.0f : align_offset(avail_w, w, node.style.main));
        const f32 y = cell.y + child.style.margin.top +
                      (node.style.cross == UiAlign::Stretch ? 0.0f : align_offset(avail_h, h, node.style.cross));
        child.solved = {x, y, w, h};
    }
}

void arrange(std::vector<LayoutNode>& nodes, LayoutScratch& scratch) {
    scratch.wrapped_cross_required.assign(nodes.size(), -1.0f);
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        LayoutNode& node = nodes[i];
        if (node.children.empty()) {
            continue;
        }

        const UiLayoutAxis axis = node.style.axis;
        const Rectf content = inset(node.solved, node.style.padding);
        const f32 content_main = main_of({content.w, content.h}, axis);
        const f32 content_cross = cross_of({content.w, content.h}, axis);
        const f32 content_main_origin = horizontal(axis) ? content.x : content.y;
        const f32 content_cross_origin = horizontal(axis) ? content.y : content.x;
        std::vector<i32>& flow_children = scratch.flow_children;
        flow_children.clear();
        for (const i32 child_index : node.children) {
            LayoutNode& child = nodes[static_cast<std::size_t>(child_index)];
            if (!child.style.overlay) {
                flow_children.push_back(child_index);
                continue;
            }

            const f32 available_w = std::max(0.0f, content.w - child.style.margin.left - child.style.margin.right);
            const f32 available_h = std::max(0.0f, content.h - child.style.margin.top - child.style.margin.bottom);
            const f32 w = resolve_axis(child.style.width, child.intrinsic.x, available_w);
            const f32 h = resolve_axis(child.style.height, child.intrinsic.y, available_h);
            const Rectf margin_rect = inset(content, child.style.margin);
            child.solved = anchor_rect(margin_rect,
                                       child.style.anchor,
                                       {std::min(w, margin_rect.w), std::min(h, margin_rect.h)},
                                       child.style.anchor_offset);
        }

        const std::size_t count = flow_children.size();
        if (count == 0) {
            continue;
        }

        if (node.style.grid_columns > 0) {
            arrange_grid(nodes, node, flow_children, content, scratch);
            continue;
        }

        if (!node.style.wrap) {
            arrange_line(nodes,
                         flow_children,
                         0,
                         count,
                         axis,
                         content_main_origin,
                         content_cross_origin,
                         content_main,
                         content_cross,
                         node.style,
                         scratch);
            continue;
        }

        scratch.line_start.clear();
        scratch.line_cross_extent.clear();
        scratch.line_start.push_back(0);
        f32 line_used = 0.0f;
        f32 line_cross = 0.0f;
        std::size_t line_count = 0;
        for (std::size_t k = 0; k < count; ++k) {
            const LayoutNode& child = nodes[static_cast<std::size_t>(flow_children[k])];
            const f32 outer_main = child_outer_main_for_break(child, axis, content_main);
            const f32 outer_cross = child_outer_cross_for_line(child, axis, content_cross);
            const f32 next_used = line_used + (line_count > 0 ? node.style.spacing : 0.0f) + outer_main;
            if (line_count > 0 && next_used > content_main) {
                scratch.line_cross_extent.push_back(line_cross);
                scratch.line_start.push_back(static_cast<i32>(k));
                line_used = outer_main;
                line_cross = outer_cross;
                line_count = 1;
            } else {
                line_used = next_used;
                line_cross = std::max(line_cross, outer_cross);
                ++line_count;
            }
        }
        scratch.line_cross_extent.push_back(line_cross);
        scratch.line_start.push_back(static_cast<i32>(count));

        f32 block_cross = 0.0f;
        for (f32 extent : scratch.line_cross_extent) {
            block_cross += extent;
        }
        if (scratch.line_cross_extent.size() > 1) {
            block_cross += node.style.line_spacing * static_cast<f32>(scratch.line_cross_extent.size() - 1);
        }

        f32 cross_cursor = content_cross_origin + align_offset(content_cross, block_cross, node.style.line_cross);
        for (std::size_t line = 0; line < scratch.line_cross_extent.size(); ++line) {
            const std::size_t begin = static_cast<std::size_t>(scratch.line_start[line]);
            const std::size_t end = static_cast<std::size_t>(scratch.line_start[line + 1]);
            const f32 extent = scratch.line_cross_extent[line];
            arrange_line(nodes,
                         flow_children,
                         begin,
                         end,
                         axis,
                         content_main_origin,
                         cross_cursor,
                         content_main,
                         extent,
                         node.style,
                         scratch);
            cross_cursor += extent + node.style.line_spacing;
        }

        const f32 padding_cross = horizontal(axis)
                                      ? node.style.padding.top + node.style.padding.bottom
                                      : node.style.padding.left + node.style.padding.right;
        scratch.wrapped_cross_required[i] = block_cross + padding_cross;
    }
}

} // namespace

void solve(std::vector<LayoutNode>& nodes, i32 root, Rectf available, LayoutScratch& scratch) {
    if (nodes.empty() || root < 0 || root >= static_cast<i32>(nodes.size())) {
        return;
    }
    measure(nodes, scratch);
    nodes[static_cast<std::size_t>(root)].solved = available;
    arrange(nodes, scratch);

    bool needs_wrapped_resolve = false;
    for (std::size_t i = 0; i < nodes.size() && i < scratch.wrapped_cross_required.size(); ++i) {
        const f32 required = scratch.wrapped_cross_required[i];
        if (required < 0.0f) {
            continue;
        }
        const UiLayoutAxis axis = nodes[i].style.axis;
        const f32 measured_cross = horizontal(axis) ? nodes[i].intrinsic.y : nodes[i].intrinsic.x;
        if (std::fabs(required - measured_cross) > 0.01f) {
            needs_wrapped_resolve = true;
            break;
        }
    }
    if (needs_wrapped_resolve) {
        measure(nodes, scratch, &scratch.wrapped_cross_required);
        nodes[static_cast<std::size_t>(root)].solved = available;
        arrange(nodes, scratch);
    }
}

void solve(std::vector<LayoutNode>& nodes, i32 root, Rectf available) {
    LayoutScratch scratch;
    solve(nodes, root, available, scratch);
}

} // namespace kin::ui2
