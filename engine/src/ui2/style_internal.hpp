#pragma once

#include <kin/ui2/context.hpp>
#include <kin/ui2/theme.hpp>

namespace kin::ui2::detail {

enum class WidgetColorState {
    Normal,
    Hovered,
    Pressed,
    Focused,
    Disabled,
};

struct InteractiveColors {
    Color hovered{};
    Color pressed{};
    Color focused{};
    Color disabled{};
    Color selected{};
};

const SurfaceStyle& widget_surface(const WidgetStyle& style, WidgetColorState state);
Color widget_fill(const WidgetStyle& style, WidgetColorState state);
Color widget_border(const WidgetStyle& style, WidgetColorState state);

SurfaceStyle resolved_widget_surface(const WidgetStyle& style, const Interaction& it, bool selected = false, bool enabled = true);
Color resolved_widget_fill(const WidgetStyle& style, const Interaction& it, bool selected = false, bool enabled = true);
Color resolved_widget_border(const WidgetStyle& style, const Interaction& it, bool selected = false, bool enabled = true);

bool default_text_style(const TextStyle& style);
bool default_shadow_style(const ShadowStyle& style);
bool default_surface_style(const SurfaceStyle& style);
bool default_widget_style(const WidgetStyle& style);

TextStyle themed_text_style(const TextStyle& style, const TextStyle& themed);
ShadowStyle themed_shadow_style(const ShadowStyle& style, const ShadowStyle& themed);
SurfaceStyle themed_surface_style(const SurfaceStyle& style, const SurfaceStyle& themed);
WidgetStyle themed_widget_style(const WidgetStyle& style, const WidgetStyle& themed);

InteractiveSurfaceStyle interactive_surface_from(SurfaceStyle normal, InteractiveColors colors);
SurfaceStyle with_fill(SurfaceStyle surface, Color fill);
SurfaceStyle with_fill_border(SurfaceStyle surface, Color fill, Color border);

} // namespace kin::ui2::detail
