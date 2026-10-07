#pragma once
#include <kin/assets/content.hpp>
namespace examples {
// Signal Siege, or Run Observatory when `tracker`. `content` is where Signal
// Siege's content is (its main passes KIN_GAME_CONTENT).
int run_example(int argc, char** argv, bool tracker, const kin::ContentSearch& content = {});
}
