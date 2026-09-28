#include "style_internal.hpp"

namespace kin::ui2::detail {
namespace {

bool same_padding(UiPadding a, UiPadding b) {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

} // namespace

const SurfaceStyle& widget_surface(const WidgetStyle& style, WidgetColorState state) {
    switch (state) {
    case WidgetColorState::Hovered:
        return style.surface.hovered;
    case WidgetColorState::Pressed:
        return style.surface.pressed;
    case WidgetColorState::Focused:
        return style.surface.focused;
    case WidgetColorState::Disabled:
        return style.surface.disabled;
    case WidgetColorState::Normal:
    default:
        return style.surface.normal;
    }
}

Color widget_fill(const WidgetStyle& style, WidgetColorState state) {
    return widget_surface(style, state).fill;
}

Color widget_border(const WidgetStyle& style, WidgetColorState state) {
    return widget_surface(style, state).border;
}

SurfaceStyle resolved_widget_surface(const WidgetStyle& style, const Interaction& it, bool selected, bool enabled) {
    return resolve(style.surface, it, selected, enabled);
}

Color resolved_widget_fill(const WidgetStyle& style, const Interaction& it, bool selected, bool enabled) {
    return resolved_widget_surface(style, it, selected, enabled).fill;
}

Color resolved_widget_border(const WidgetStyle& style, const Interaction& it, bool selected, bool enabled) {
    return resolved_widget_surface(style, it, selected, enabled).border;
}

bool default_text_style(const TextStyle& style) {
    const TextStyle defaults{};
    return !style.font && style.scale == defaults.scale && style.color == defaults.color;
}

bool default_shadow_style(const ShadowStyle& style) {
    const ShadowStyle defaults{};
    return style.color == defaults.color &&
           style.offset == defaults.offset &&
           style.spread == defaults.spread &&
           style.radius == defaults.radius &&
           style.layers == defaults.layers &&
           style.enabled == defaults.enabled;
}

bool default_surface_style(const SurfaceStyle& style) {
    const SurfaceStyle defaults{};
    return style.fill == defaults.fill &&
           style.border == defaults.border &&
           default_shadow_style(style.shadow) &&
           style.border_width == defaults.border_width &&
           style.radius == defaults.radius &&
           style.border_mode == defaults.border_mode &&
           style.draw_fill == defaults.draw_fill &&
           !style.use_skin &&
           style.skin_tint == defaults.skin_tint;
}

bool default_widget_style(const WidgetStyle& style) {
    const WidgetStyle defaults{};
    return default_surface_style(style.surface.normal) &&
           default_surface_style(style.surface.hovered) &&
           default_surface_style(style.surface.pressed) &&
           default_surface_style(style.surface.focused) &&
           default_surface_style(style.surface.disabled) &&
           default_surface_style(style.surface.selected) &&
           style.accent == defaults.accent &&
           style.track == defaults.track &&
           same_padding(style.padding, defaults.padding);
}

TextStyle themed_text_style(const TextStyle& style, const TextStyle& themed) {
    TextStyle merged = style;
    const TextStyle defaults{};
    if (!merged.font) {
        merged.font = themed.font;
    }
    if (merged.scale == defaults.scale) {
        merged.scale = themed.scale;
    }
    if (merged.color == defaults.color) {
        merged.color = themed.color;
    }
    return merged;
}

ShadowStyle themed_shadow_style(const ShadowStyle& style, const ShadowStyle& themed) {
    const ShadowStyle defaults{};
    ShadowStyle merged = style;
    if (merged.color == defaults.color) {
        merged.color = themed.color;
    }
    if (merged.offset == defaults.offset) {
        merged.offset = themed.offset;
    }
    if (merged.spread == defaults.spread) {
        merged.spread = themed.spread;
    }
    if (merged.radius == defaults.radius) {
        merged.radius = themed.radius;
    }
    if (merged.layers == defaults.layers) {
        merged.layers = themed.layers;
    }
    if (merged.enabled == defaults.enabled) {
        merged.enabled = themed.enabled;
    }
    return merged;
}

SurfaceStyle themed_surface_style(const SurfaceStyle& style, const SurfaceStyle& themed) {
    const SurfaceStyle defaults{};
    SurfaceStyle merged = style;
    if (merged.fill == defaults.fill) {
        merged.fill = themed.fill;
    }
    if (merged.border == defaults.border) {
        merged.border = themed.border;
    }
    merged.shadow = themed_shadow_style(merged.shadow, themed.shadow);
    if (merged.border_width == defaults.border_width) {
        merged.border_width = themed.border_width;
    }
    if (merged.radius == defaults.radius) {
        merged.radius = themed.radius;
    }
    if (merged.border_mode == defaults.border_mode) {
        merged.border_mode = themed.border_mode;
    }
    if (merged.draw_fill == defaults.draw_fill) {
        merged.draw_fill = themed.draw_fill;
    }
    if (!merged.use_skin && themed.use_skin) {
        merged.skin = themed.skin;
        merged.use_skin = themed.use_skin;
    }
    if (merged.skin_tint == defaults.skin_tint) {
        merged.skin_tint = themed.skin_tint;
    }
    return merged;
}

WidgetStyle themed_widget_style(const WidgetStyle& style, const WidgetStyle& themed) {
    const WidgetStyle defaults{};
    WidgetStyle merged = style;
    merged.surface.normal = themed_surface_style(merged.surface.normal, themed.surface.normal);
    merged.surface.hovered = themed_surface_style(merged.surface.hovered, themed.surface.hovered);
    merged.surface.pressed = themed_surface_style(merged.surface.pressed, themed.surface.pressed);
    merged.surface.focused = themed_surface_style(merged.surface.focused, themed.surface.focused);
    merged.surface.disabled = themed_surface_style(merged.surface.disabled, themed.surface.disabled);
    merged.surface.selected = themed_surface_style(merged.surface.selected, themed.surface.selected);
    if (merged.accent == defaults.accent) {
        merged.accent = themed.accent;
    }
    if (merged.track == defaults.track) {
        merged.track = themed.track;
    }
    if (same_padding(merged.padding, defaults.padding)) {
        merged.padding = themed.padding;
    }
    return merged;
}

InteractiveSurfaceStyle interactive_surface_from(SurfaceStyle normal, InteractiveColors colors) {
    InteractiveSurfaceStyle out;
    out.normal = normal;
    out.hovered = normal;
    out.hovered.fill = colors.hovered;
    out.pressed = normal;
    out.pressed.fill = colors.pressed;
    out.focused = normal;
    out.focused.border = colors.focused;
    out.focused.glow_color = colors.focused;
    out.focused.glow_size = 3.0f;
    out.disabled = normal;
    out.disabled.fill = colors.disabled;
    out.selected = normal;
    out.selected.fill = colors.selected;
    out.selected.border = colors.focused;
    return out;
}

SurfaceStyle with_fill(SurfaceStyle surface, Color fill) {
    surface.fill = fill;
    return surface;
}

SurfaceStyle with_fill_border(SurfaceStyle surface, Color fill, Color border) {
    surface.fill = fill;
    surface.border = border;
    return surface;
}

} // namespace kin::ui2::detail
