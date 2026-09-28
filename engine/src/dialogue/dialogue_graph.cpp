#include <kin/dialogue/dialogue_graph.hpp>

#include <unordered_map>

namespace kin {

ui2::NodeGraph dialogue_to_node_graph(const DialogueDocument& document) {
    ui2::NodeGraph graph;
    graph.id = ui2::make_id("dialogue.graph");
    std::unordered_map<std::string, i32> index_by_id;
    for (i32 i = 0; i < static_cast<i32>(document.nodes.size()); ++i) {
        const DialogueNode& node = document.nodes[static_cast<std::size_t>(i)];
        index_by_id[node.id] = i;
        ui2::NodeGraphNode graph_node{
            .id = node.id,
            .title = node.id.empty() ? "node" : node.id,
            .position = {static_cast<f32>((i % 4) * 180), static_cast<f32>((i / 4) * 130)},
            .inputs = {{.id = "in", .label = "In", .kind = ui2::NodeGraphPortKind::Input}},
            .outputs = {{.id = "out", .label = "Out", .kind = ui2::NodeGraphPortKind::Output}},
        };
        graph.nodes.push_back(std::move(graph_node));
    }
    const auto add_edge = [&](const DialogueNode& from, std::string_view target, std::string suffix) {
        if (target.empty() || !index_by_id.contains(std::string{target})) return;
        graph.edges.push_back({
            .id = from.id + "." + suffix,
            .from_node = from.id,
            .from_port = "out",
            .to_node = std::string{target},
            .to_port = "in",
        });
    };
    for (const DialogueNode& node : document.nodes) {
        add_edge(node, node.line.next, "next");
        add_edge(node, node.target, "target");
        for (const DialogueChoice& choice : node.choices) {
            add_edge(node, choice.target, choice.id.empty() ? "choice" : choice.id);
        }
    }
    return graph;
}

} // namespace kin
