#include <kin/ecs/text.hpp>

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace kin {

TextLayout layout_text(const TextRenderer& text) {
    TextLayout laid;
    if (!text.face || text.text.empty()) {
        return laid;
    }
    // Broken at '\n', and past `wrap` (the spaces at a break dropped).
    for (const ui2::TextRange range : ui2::wrap_text_ranges(text.face, text.text, {.max_width = std::max(text.wrap, 0.0f)})) {
        std::string_view line = std::string_view(text.text).substr(range.begin, range.end - range.begin);
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r')) {
            line.remove_suffix(1);
        }
        laid.lines.emplace_back(line);
    }
    laid.line_height = ui2::measure_text(text.face, "Ag").y;
    for (const std::string& line : laid.lines) {
        const f32 width = ui2::measure_text(text.face, line).x;
        laid.widths.push_back(width);
        laid.size.x = std::max(laid.size.x, width);
    }
    laid.size.y = laid.line_height * static_cast<f32>(laid.lines.size());
    laid.at = {text.offset.x - text.pivot.x * laid.size.x, text.offset.y - text.pivot.y * laid.size.y};
    return laid;
}

void draw_text_layout(Renderer2D& renderer, const TextRenderer& text, const TextLayout& laid) {
    for (std::size_t i = 0; i < laid.lines.size(); ++i) {
        const f32 spare = laid.size.x - laid.widths[i];
        const f32 x = laid.at.x + (text.align == TextAlign::Left ? 0.0f : text.align == TextAlign::Center ? spare * 0.5f : spare);
        const Vec2f pos{x, laid.at.y + laid.line_height * static_cast<f32>(i)};
        if (text.outline > 0.0f) {
            ui2::draw_text_outlined(renderer, text.face, laid.lines[i], pos, 1.0f, text.color, text.outline, text.outline_color);
        } else {
            ui2::draw_text(renderer, text.face, laid.lines[i], pos, 1.0f, text.color);
        }
    }
}

std::string_view text_align_name(TextAlign align) {
    switch (align) {
    case TextAlign::Left: return "left";
    case TextAlign::Center: return "center";
    case TextAlign::Right: return "right";
    }
    return "center";
}

std::optional<TextAlign> text_align_named(std::string_view name) {
    if (name == "left") return TextAlign::Left;
    if (name == "center") return TextAlign::Center;
    if (name == "right") return TextAlign::Right;
    return std::nullopt;
}

Rectf text_bounds(const TextRenderer& text) {
    const TextLayout laid = layout_text(text);
    if (laid.lines.empty()) {
        return {};
    }
    return {laid.at.x - text.outline, laid.at.y - text.outline, laid.size.x + text.outline * 2.0f,
            laid.size.y + text.outline * 2.0f};
}

} // namespace kin
