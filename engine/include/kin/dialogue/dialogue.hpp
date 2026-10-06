#pragma once

#include <kin/core/types.hpp>
#include <kin/renderer/color.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

namespace kin {

enum class DialogueNodeKind {
    Line,
    Choice,
    Jump,
    Event,
    End,
};

enum class DialogueConditionKind {
    Bool,
    Number,
    String,
    VisitedNode,
    VisitedChoice,
};

enum class DialogueCompare {
    Equal,
    NotEqual,
    Less,
    LessEqual,
    Greater,
    GreaterEqual,
};

enum class DialogueEffectKind {
    SetBool,
    SetNumber,
    AddNumber,
    SetString,
    EmitEvent,
    Jump,
};

struct DialogueVariable {
    std::variant<bool, f64, std::string> value = false;
};

struct DialogueSpeaker {
    std::string id;
    std::string name;
    std::string portrait;
    Color name_color = colors::white;
};

struct DialogueCondition {
    DialogueConditionKind kind = DialogueConditionKind::Bool;
    std::string key;
    DialogueCompare compare = DialogueCompare::Equal;
    DialogueVariable value{};
};

struct DialogueEffect {
    DialogueEffectKind kind = DialogueEffectKind::EmitEvent;
    std::string key;
    DialogueVariable value{};
};

struct DialogueChoice {
    std::string id;
    std::string text_markup;
    std::string target;
    bool enabled = true;
    bool visited = false;
    std::vector<DialogueCondition> conditions;
    std::vector<DialogueEffect> effects;
};

struct DialogueLine {
    std::string speaker_id;
    std::string text_markup;
    std::string voice_cue;
    std::string portrait;
    std::vector<std::string> tags;
    std::string next;
};

struct DialogueNode {
    std::string id;
    DialogueNodeKind kind = DialogueNodeKind::Line;
    DialogueLine line;
    std::vector<DialogueChoice> choices;
    std::string target;
    std::string event_id;
    std::vector<DialogueEffect> effects;
};

struct DialogueDocument {
    std::string id;
    std::string start_node;
    std::vector<DialogueSpeaker> speakers;
    std::vector<DialogueNode> nodes;
};

struct DialogueHistoryEntry {
    std::string node_id;
    std::string speaker_id;
    std::string text_markup;
};

struct DialogueEvent {
    std::string id;
    std::string node_id;
};

struct DialogueChoiceView {
    std::string id;
    std::string text_markup;
    bool enabled = true;
    bool visited = false;
};

struct DialogueViewModel {
    bool active = false;
    bool ended = false;
    std::string node_id;
    std::string speaker_id;
    std::string speaker_name;
    std::string speaker_portrait;
    Color speaker_color = colors::white;
    std::string text_markup;
    std::vector<DialogueChoiceView> choices;
    std::vector<DialogueHistoryEntry> history;
};

struct DialogueStateSnapshot {
    std::string current_node;
    bool ended = false;
    std::unordered_map<std::string, DialogueVariable> variables;
    std::vector<std::string> visited_nodes;
    std::vector<std::string> visited_choices;
    std::vector<DialogueHistoryEntry> history;
};

struct DialogueState {
    std::string current_node;
    bool ended = false;
    std::unordered_map<std::string, DialogueVariable> variables;
    std::vector<std::string> visited_nodes;
    std::vector<std::string> visited_choices;
    std::vector<DialogueHistoryEntry> history;
    std::vector<DialogueEvent> events;
};

// Translated dialogue. While there is an active localization (see
// kin/l10n/localization.hpp), DialoguePlayer::current_view shows each line,
// choice and speaker name from it where it has the key, and the document's
// own text where it does not:
//
//   dialogue.<document id>.<node id>               a line
//   dialogue.<document id>.<node id>.<choice id>   a choice
//   dialogue.<document id>.speakers.<speaker id>   a speaker's name, else
//   speakers.<speaker id>                          one shared by every document
//
// (Without a document id, "dialogue.<node id>".) Translated text is a message
// (see message_format.hpp) given the dialogue's variables, so "{gold}" and
// plurals of them work. History entries are shown in the current language too.
std::string dialogue_text_key(const DialogueDocument& document, std::string_view node_id,
                              std::string_view choice_id = {});
std::string dialogue_speaker_key(const DialogueDocument& document, std::string_view speaker_id);
// The document's own text under those keys (speakers as speakers.<id>), as a
// language for Localization::add or write_language_file: what translators
// start from, and what lets Localization::validate see untranslated lines.
struct LanguageFile;
LanguageFile dialogue_language_file(const DialogueDocument& document, std::string_view locale = "en");

class DialoguePlayer {
public:
    void start(const DialogueDocument& document, std::string_view start_node = {});
    void reset();
    void advance();
    bool choose(i32 index);
    bool choose(std::string_view choice_id);
    void skip_line();

    const DialogueState& state() const { return _state; }
    DialogueViewModel current_view(bool show_disabled_choices = false) const;
    std::vector<DialogueEvent> consume_events();

    DialogueStateSnapshot snapshot() const;
    void restore(const DialogueDocument& document, const DialogueStateSnapshot& snapshot);

private:
    const DialogueDocument* _document = nullptr;
    DialogueState _state;

    const DialogueNode* node(std::string_view id) const;
    const DialogueSpeaker* speaker(std::string_view id) const;
    void resolve_presentable();
    bool condition_met(const DialogueCondition& condition) const;
    void apply_effect(const DialogueEffect& effect);
    void jump_to(std::string_view id);
    void emit(std::string id, std::string node_id = {});
};

struct DialogueLoadResult {
    std::optional<DialogueDocument> document;
    std::string error;
    bool ok() const { return document.has_value(); }
};

DialogueLoadResult parse_dialogue(std::string_view json);
DialogueLoadResult load_dialogue(const std::filesystem::path& path);
bool save_dialogue(const DialogueDocument& document, const std::filesystem::path& path, std::string* error = nullptr);
std::string dialogue_to_json(const DialogueDocument& document);

} // namespace kin
