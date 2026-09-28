#pragma once

#include <kin/dialogue/dialogue.hpp>
#include <kin/ui2/widgets.hpp>

namespace kin {

ui2::NodeGraph dialogue_to_node_graph(const DialogueDocument& document);

} // namespace kin
