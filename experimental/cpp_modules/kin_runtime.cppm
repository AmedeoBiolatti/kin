// Experimental facade: declarations remain owned by the global module.
// This preserves linkage with Kin libraries compiled through ordinary headers.
module;
#include <kin/core/hex.hpp>
#include <kin/core/json.hpp>
#include <kin/pathfinding/pathfinding.hpp>
#include <kin/platform/input.hpp>
#include <kin/platform/window.hpp>
#include <kin/renderer/renderer2d.hpp>
#include <kin/runtime/game_info.hpp>
#include <kin/runtime/scene_app.hpp>
#include <kin/scene/scene.hpp>
#include <kin/scene/scene_manager.hpp>
#include <kin/ui2/text.hpp>

export module kin.runtime;

export namespace kin {
using ::kin::Color;
using ::kin::GameInfo;
using ::kin::Hex;
using ::kin::HexGridNav;
using ::kin::HexLayout;
using ::kin::HexOffset;
using ::kin::HexOrientation;
using ::kin::HexReach;
using ::kin::InputActionContext;
using ::kin::InputMap;
using ::kin::JsonWriter;
using ::kin::Key;
using ::kin::MouseButton;
using ::kin::Rectf;
using ::kin::Renderer2D;
using ::kin::Scene;
using ::kin::SceneContext;
using ::kin::SceneManager;
using ::kin::Texture;
using ::kin::Vec2f;
using ::kin::Vec2i;
using ::kin::find_path;
using ::kin::hex_distance;
using ::kin::i32;
using ::kin::i64;
using ::kin::parse_headless_options;
using ::kin::reachable_cells;
using ::kin::run_scene_app;
using ::kin::u8;
using ::kin::window_config;
}

export namespace kin::ui2 {
using ::kin::ui2::Font;
using ::kin::ui2::draw_text;
using ::kin::ui2::system_ui_font;
}
