#include "example_common.hpp"
#include <kin/ui2/text.hpp>
#include <kin/assets/content.hpp>
#include <kin/platform/log.hpp>
#include <algorithm>
#include <map>
#include <stdexcept>

namespace examples {
ui2::Font siege_ui_font(float size) {
#if KIN_ENABLE_TTF
    const int points = static_cast<int>(std::clamp(size, 8.0f, 32.0f));
    const auto root = siege_language_dir() / "fonts";
    static std::map<std::pair<std::filesystem::path, int>, ui2::Font> cache;
    const auto key = std::make_pair(root, points);
    if (const auto found = cache.find(key); found != cache.end()) return found->second;
    for (const char* file : {"latin.ttf", "arabic.ttf", "japanese.otf"}) {
        if (!content_file_exists(root / file))
            throw std::runtime_error("Missing bundled game font: " + (root / file).string());
    }
    auto font = ui2::load_ttf_font(root / "latin.ttf", static_cast<float>(points),
        ui2::TtfFontOptions{.fallbacks = {root / "arabic.ttf", root / "japanese.otf"},
            .language_fallbacks = {{"ja", root / "japanese.otf"}, {"ar", root / "arabic.ttf"}}});
    KIN_LOG_INFO("fonts", "Signal Siege bundled font chain loaded (no system discovery)");
    cache.emplace(key, font);
    return font;
#else
    return ui2::bitmap_font();
#endif
}
} // namespace examples
