#include <kin/assets/asset_manager.hpp>
#include <kin/dialogue/dialogue.hpp>
#include <kin/dialogue/dialogue_graph.hpp>
#include <kin/l10n/localization.hpp>

#include <algorithm>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

using namespace kin;

DialogueDocument make_document() {
    DialogueDocument doc{
        .id = "fixture",
        .start_node = "start",
        .speakers = {{.id = "npc", .name = "Guide", .name_color = Color::rgb(128, 226, 160)}},
    };
    doc.nodes.push_back({
        .id = "start",
        .kind = DialogueNodeKind::Line,
        .line = {.speaker_id = "npc", .text_markup = "Hello", .next = "choice"},
    });
    doc.nodes.push_back({
        .id = "choice",
        .kind = DialogueNodeKind::Choice,
        .choices = {
            {.id = "yes",
             .text_markup = "Yes",
             .target = "event",
             .effects = {
                 {.kind = DialogueEffectKind::SetBool, .key = "accepted", .value = {.value = true}},
                 {.kind = DialogueEffectKind::AddNumber, .key = "score", .value = {.value = 2.0}},
                 {.kind = DialogueEffectKind::EmitEvent, .key = "choice.accepted"},
             }},
            {.id = "locked",
             .text_markup = "Locked",
             .target = "end",
             .conditions = {{.kind = DialogueConditionKind::Number,
                             .key = "score",
                             .compare = DialogueCompare::GreaterEqual,
                             .value = {.value = 10.0}}}},
        },
    });
    doc.nodes.push_back({
        .id = "event",
        .kind = DialogueNodeKind::Event,
        .target = "end",
        .event_id = "met.guide",
    });
    doc.nodes.push_back({.id = "end", .kind = DialogueNodeKind::End});
    return doc;
}

void test_linear_choice_effects_and_events() {
    DialogueDocument doc = make_document();
    DialoguePlayer player;
    player.start(doc);
    DialogueViewModel view = player.current_view();
    assert(view.active);
    assert(view.text_markup == "Hello");
    assert(view.speaker_name == "Guide");

    player.advance();
    view = player.current_view();
    assert(view.choices.size() == 1);
    assert(view.choices[0].id == "yes");

    assert(player.choose("yes"));
    assert(player.state().ended);
    assert(std::get<bool>(player.state().variables.at("accepted").value));
    assert(std::get<f64>(player.state().variables.at("score").value) == 2.0);
    const std::vector<DialogueEvent> events = player.consume_events();
    assert(events.size() == 2);
    assert(events[0].id == "choice.accepted");
    assert(events[1].id == "met.guide");
}

void test_snapshot_restore_and_invalid_target() {
    DialogueDocument doc = make_document();
    DialoguePlayer player;
    player.start(doc);
    player.advance();
    DialogueStateSnapshot snapshot = player.snapshot();

    DialoguePlayer restored;
    restored.restore(doc, snapshot);
    assert(restored.current_view().choices.size() == 1);
    assert(restored.state().current_node == "choice");

    DialogueDocument broken = doc;
    broken.nodes[0].line.next = "missing";
    player.start(broken);
    player.advance();
    assert(player.state().ended);
    const std::vector<DialogueEvent> events = player.consume_events();
    assert(!events.empty());
    assert(events.back().id == "dialogue.missing_node");
}

void test_json_roundtrip_asset_and_graph() {
    DialogueDocument doc = make_document();
    const std::string json = dialogue_to_json(doc);
    DialogueLoadResult parsed = parse_dialogue(json);
    assert(parsed.ok());
    assert(parsed.document->nodes.size() == doc.nodes.size());
    assert(parsed.document->speakers.size() == doc.speakers.size());

    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "kin_dialogue_tests";
    std::filesystem::create_directories(dir);
    const std::filesystem::path file = dir / "fixture.kindialogue";
    std::string error;
    assert(save_dialogue(doc, file, &error));
    DialogueLoadResult loaded = load_dialogue(file);
    assert(loaded.ok());
    assert(loaded.document->id == "fixture");

    AssetManager assets{dir};
    std::shared_ptr<const DialogueDocument> asset = assets.load<DialogueDocument>("fixture.kindialogue");
    assert(asset);
    assert(asset->start_node == "start");
    assets.discover();
    const AssetMetadata* metadata = assets.metadata("fixture.kindialogue");
    assert(metadata);
    assert(metadata->type == AssetType::Dialogue);

    ui2::NodeGraph graph = dialogue_to_node_graph(doc);
    assert(graph.nodes.size() == doc.nodes.size());
    assert(!graph.edges.empty());
}

} // namespace

// Lines, choices, speakers and history come from the active localization
// where it has their keys, with the dialogue's variables put in.
void test_localized_view() {
    DialogueDocument doc = make_document();
    assert(dialogue_text_key(doc, "start") == "dialogue.fixture.start");
    assert(dialogue_text_key(doc, "choice", "yes") == "dialogue.fixture.choice.yes");

    LanguageFile en = dialogue_language_file(doc);
    assert(en.locale == "en" && en.strings.size() == 4);
    assert(en.strings.at("dialogue.fixture.start") == "Hello");
    assert(en.strings.at("dialogue.fixture.choice.locked") == "Locked");
    assert(en.strings.at("speakers.npc") == "Guide");

    LanguageFile fr{.locale = "fr", .name = "Français"};
    fr.strings.emplace("dialogue.fixture.start", "Bonjour ({score, plural, one {# point} other {# points}})");
    fr.strings.emplace("dialogue.fixture.choice.yes", "Oui");
    fr.strings.emplace("speakers.npc", "Guide (fr)");
    Localization l10n;
    l10n.add("dialogue", std::move(en));
    l10n.add("fr", std::move(fr));
    set_active_localization(&l10n);

    DialoguePlayer player;
    player.start(doc);
    assert(player.current_view().text_markup == "Hello"); // the base locale shows the base text
    l10n.set_locale("fr");
    DialogueViewModel view = player.current_view();
    assert(view.text_markup == "Bonjour ({score, plural, one {# point} other {# points}})"); // no score yet
    assert(view.speaker_name == "Guide (fr)");
    player.advance();
    view = player.current_view(true);
    assert(view.choices[0].text_markup == "Oui");
    assert(view.choices[1].text_markup == "Locked"); // untranslated: the base text
    assert(view.history.size() == 1);

    // History follows the language, with the variables as they are now.
    player.choose("yes");
    view = player.current_view();
    assert(view.history[0].text_markup == "Bonjour (2 points)");
    l10n.set_locale("en");
    assert(player.current_view().history[0].text_markup == "Hello");

    // Without the active localization, the document's text.
    set_active_localization(nullptr);
    l10n.set_locale("fr");
    assert(player.current_view().history[0].text_markup == "Hello");
    const auto issues = l10n.validate();
    const auto found = [&](std::string_view key, std::string_view fragment) {
        return std::ranges::any_of(issues, [&](const LocalizationIssue& issue) {
            return issue.key == key && issue.message.find(fragment) != std::string::npos;
        });
    };
    assert(issues.size() == 3);
    assert(found("dialogue.fixture.choice.locked", "not translated"));
    assert(found("dialogue.fixture.start", "{score}"));  // a variable the base text does not use
    assert(found("dialogue.fixture.start", "no many")); // French has a form for millions
}

int main() {
    test_localized_view();
    test_linear_choice_effects_and_events();
    test_snapshot_restore_and_invalid_target();
    test_json_roundtrip_asset_and_graph();
    return 0;
}
