#include <kin/assets/asset_manager.hpp>
#include <kin/dialogue/dialogue.hpp>
#include <kin/dialogue/dialogue_graph.hpp>
#include <kin/ecs/dialogue.hpp>
#include <kin/ecs/render.hpp>
#include <kin/platform/input.hpp>
#include <kin/scene/scene_asset.hpp>
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

// What is wrong with a dialogue; its places and speakers' colours kept.
void test_problems_and_format() {
    assert(dialogue_problems(make_document()).empty());
    DialogueDocument broken = make_document();
    broken.nodes[0].line.speaker_id = "nobody";
    broken.nodes[1].choices[0].target = "nowhere";
    broken.nodes.push_back({.id = "lost", .kind = DialogueNodeKind::End});
    const std::vector<std::string> problems = dialogue_problems(broken);
    const auto says = [&](std::string_view words) {
        return std::ranges::any_of(problems, [&](const std::string& p) { return p.find(words) != std::string::npos; });
    };
    assert(says("'nobody'") && says("'nowhere'") && says("'lost' is never reached"));
    // A node the game starts it at too: a start, not lost; written and read back.
    DialogueDocument entered = make_document();
    entered.nodes.push_back({.id = "lost", .kind = DialogueNodeKind::End});
    entered.entries = {"lost"};
    assert(dialogue_problems(entered).empty());
    assert(parse_dialogue(dialogue_to_json(entered)).document->entries == std::vector<std::string>{"lost"});
    entered.entries = {"gone"};
    assert(!dialogue_problems(entered).empty()); // a start that is no node

    DialogueDocument placed = make_document();
    placed.nodes[0].place = Vec2f{120.0f, 40.0f};
    placed.nodes[0].line.tags = {"happy"};
    const DialogueLoadResult read = parse_dialogue(dialogue_to_json(placed));
    assert(read.ok() && read.document->nodes[0].place && read.document->nodes[0].place->x == 120.0f);
    assert(read.document->nodes[0].line.tags == std::vector<std::string>{"happy"});
    assert(read.document->speakers[0].name_color == Color::rgb(128, 226, 160));
    assert(dialogue_variable_text(dialogue_variable_from_text("3")) == "3" && std::get<bool>(dialogue_variable_from_text("true").value));
}

// A dialogue run in a scene: revealed letter by letter; the game's variables
// kept between dialogues; what happened told; started by talking.
void test_scene_dialogue() {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "kin_scene_dialogue_test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    assert(save_dialogue(make_document(), dir / "guide.kindialogue"));
    assert(dialogue_letter_count("[b]Hél[/b]lo") == 5 && dialogue_plain_text("a [[b] c") == "a [b] c");

    SceneDialogue dialogue{dir};
    dialogue.letters_per_second = 10.0f;
    assert(!dialogue.start("missing.kindialogue") && dialogue.problems().contains("missing.kindialogue"));
    assert(dialogue.start("guide.kindialogue") && dialogue.active());
    assert(dialogue.view().text_markup == "Hello" && dialogue.letters() == 5 && dialogue.revealed() == 0);
    dialogue.update(0.25f);
    assert(dialogue.revealed() == 2 && dialogue.revealing());
    dialogue.advance(); // the whole line first
    assert(dialogue.revealed() == 5 && !dialogue.revealing() && dialogue.view().node_id == "start");
    dialogue.advance();
    // At the choice: the line before it shows with it, all of it.
    assert(dialogue.view().node_id == "choice" && dialogue.view().text_markup == "Hello" && dialogue.view().choices.size() == 1);
    assert(!dialogue.revealing());
    assert(dialogue.choose(0) && !dialogue.active());
    assert(std::get<f64>(dialogue.variable("score").value) == 2.0);
    std::vector<std::string> told;
    for (const DialogueEvent& e : dialogue.events()) told.push_back(e.id);
    assert((told == std::vector<std::string>{"dialogue.started", "choice.accepted", "met.guide", "dialogue.ended"}));
    dialogue.clear_events();

    // Again: the variables kept (score 2, then 10 opens the locked choice).
    assert(dialogue.start("guide.kindialogue", "choice"));
    assert(dialogue.view().choices.size() == 1);
    dialogue.set_variable("score", {.value = 10.0});
    assert(dialogue.view().choices.size() == 2);
    // Saved and back.
    const DialogueStateSnapshot saved = dialogue.snapshot();
    dialogue.stop();
    assert(dialogue.restore("guide.kindialogue", saved) && dialogue.active() && dialogue.view().node_id == "choice");
    dialogue.stop();

    // Talking: the Talker near a TalkComponent, the talk action pressed.
    kin::EcsWorld world;
    register_builtin_editor_components(world);
    kin::EcsEntity player = world.entity().set(Transform2D{.pos = {0.0f, 0.0f}}).set(Talker{});
    kin::EcsEntity far = world.entity().set(Transform2D{.pos = {500.0f, 0.0f}});
    far.set(TalkComponent{.asset = {.id = "guide.kindialogue"}, .start = {}, .radius = 64.0f, .prompt = "Talk", .enabled = true});
    kin::EcsEntity guide = world.entity().set(Transform2D{.pos = {40.0f, 0.0f}});
    guide.set(TalkComponent{.asset = {.id = "guide.kindialogue"}, .start = "choice", .radius = 64.0f, .prompt = "Talk", .enabled = true});
    assert(talk_target(world) && talk_target(world)->id() == guide.id());
    Input input;
    input.bind("talk", Key::E);
    update_talk(world, dialogue, input); // not pressed
    assert(!dialogue.active());
    input.set_key_pressed(Key::E);
    update_talk(world, dialogue, input);
    assert(dialogue.active() && dialogue.view().node_id == "choice" && dialogue.speaker_entity() == guide.id());
    player.set(Transform2D{.pos = {1000.0f, 0.0f}});
    assert(!talk_target(world));
    fs::remove_all(dir);
}

int main() {
    test_localized_view();
    test_linear_choice_effects_and_events();
    test_snapshot_restore_and_invalid_target();
    test_json_roundtrip_asset_and_graph();
    test_problems_and_format();
    test_scene_dialogue();
    return 0;
}
