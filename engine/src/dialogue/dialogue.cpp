#include <kin/dialogue/dialogue.hpp>

#include <kin/assets/content.hpp>
#include <kin/core/json.hpp>
#include <kin/core/json_value.hpp>
#include <kin/l10n/localization.hpp>

#include <algorithm>
#include <fstream>
#include <sstream>

namespace kin {
namespace {

std::string dialogue_prefix(const DialogueDocument& document) {
    return document.id.empty() ? std::string{"dialogue."} : "dialogue." + document.id + ".";
}

// The dialogue's variables as message arguments (views into `variables`).
std::vector<MessageArg> variable_args(const std::unordered_map<std::string, DialogueVariable>& variables) {
    std::vector<MessageArg> args;
    args.reserve(variables.size());
    for (const auto& [name, variable] : variables) {
        if (const auto* number = std::get_if<f64>(&variable.value)) {
            args.emplace_back(name, *number);
        } else if (const auto* boolean = std::get_if<bool>(&variable.value)) {
            args.emplace_back(name, *boolean ? std::string_view{"true"} : std::string_view{"false"});
        } else {
            args.emplace_back(name, std::string_view{std::get<std::string>(variable.value)});
        }
    }
    return args;
}

bool contains_string(const std::vector<std::string>& values, std::string_view value) {
    return std::ranges::find(values, value) != values.end();
}

f64 number_value(const DialogueVariable& value) {
    if (const auto* number = std::get_if<f64>(&value.value)) {
        return *number;
    }
    if (const auto* boolean = std::get_if<bool>(&value.value)) {
        return *boolean ? 1.0 : 0.0;
    }
    return 0.0;
}

bool bool_value(const DialogueVariable& value) {
    if (const auto* boolean = std::get_if<bool>(&value.value)) {
        return *boolean;
    }
    if (const auto* number = std::get_if<f64>(&value.value)) {
        return *number != 0.0;
    }
    if (const auto* text = std::get_if<std::string>(&value.value)) {
        return !text->empty();
    }
    return false;
}

std::string string_value(const DialogueVariable& value) {
    if (const auto* text = std::get_if<std::string>(&value.value)) {
        return *text;
    }
    if (const auto* boolean = std::get_if<bool>(&value.value)) {
        return *boolean ? "true" : "false";
    }
    if (const auto* number = std::get_if<f64>(&value.value)) {
        std::ostringstream out;
        out << *number;
        return out.str();
    }
    return {};
}

bool compare_number(f64 lhs, DialogueCompare compare, f64 rhs) {
    switch (compare) {
    case DialogueCompare::Equal: return lhs == rhs;
    case DialogueCompare::NotEqual: return lhs != rhs;
    case DialogueCompare::Less: return lhs < rhs;
    case DialogueCompare::LessEqual: return lhs <= rhs;
    case DialogueCompare::Greater: return lhs > rhs;
    case DialogueCompare::GreaterEqual: return lhs >= rhs;
    }
    return false;
}

bool compare_string(std::string_view lhs, DialogueCompare compare, std::string_view rhs) {
    switch (compare) {
    case DialogueCompare::Equal: return lhs == rhs;
    case DialogueCompare::NotEqual: return lhs != rhs;
    default: return false;
    }
}

std::string node_kind_name(DialogueNodeKind kind) {
    switch (kind) {
    case DialogueNodeKind::Line: return "line";
    case DialogueNodeKind::Choice: return "choice";
    case DialogueNodeKind::Jump: return "jump";
    case DialogueNodeKind::Event: return "event";
    case DialogueNodeKind::End: return "end";
    }
    return "line";
}

DialogueNodeKind parse_node_kind(std::string_view value) {
    if (value == "choice") return DialogueNodeKind::Choice;
    if (value == "jump") return DialogueNodeKind::Jump;
    if (value == "event") return DialogueNodeKind::Event;
    if (value == "end") return DialogueNodeKind::End;
    return DialogueNodeKind::Line;
}

std::string compare_name(DialogueCompare compare) {
    switch (compare) {
    case DialogueCompare::Equal: return "eq";
    case DialogueCompare::NotEqual: return "ne";
    case DialogueCompare::Less: return "lt";
    case DialogueCompare::LessEqual: return "le";
    case DialogueCompare::Greater: return "gt";
    case DialogueCompare::GreaterEqual: return "ge";
    }
    return "eq";
}

DialogueCompare parse_compare(std::string_view value) {
    if (value == "ne") return DialogueCompare::NotEqual;
    if (value == "lt") return DialogueCompare::Less;
    if (value == "le") return DialogueCompare::LessEqual;
    if (value == "gt") return DialogueCompare::Greater;
    if (value == "ge") return DialogueCompare::GreaterEqual;
    return DialogueCompare::Equal;
}

std::string condition_kind_name(DialogueConditionKind kind) {
    switch (kind) {
    case DialogueConditionKind::Bool: return "bool";
    case DialogueConditionKind::Number: return "number";
    case DialogueConditionKind::String: return "string";
    case DialogueConditionKind::VisitedNode: return "visited_node";
    case DialogueConditionKind::VisitedChoice: return "visited_choice";
    }
    return "bool";
}

DialogueConditionKind parse_condition_kind(std::string_view value) {
    if (value == "number") return DialogueConditionKind::Number;
    if (value == "string") return DialogueConditionKind::String;
    if (value == "visited_node") return DialogueConditionKind::VisitedNode;
    if (value == "visited_choice") return DialogueConditionKind::VisitedChoice;
    return DialogueConditionKind::Bool;
}

std::string effect_kind_name(DialogueEffectKind kind) {
    switch (kind) {
    case DialogueEffectKind::SetBool: return "set_bool";
    case DialogueEffectKind::SetNumber: return "set_number";
    case DialogueEffectKind::AddNumber: return "add_number";
    case DialogueEffectKind::SetString: return "set_string";
    case DialogueEffectKind::EmitEvent: return "emit_event";
    case DialogueEffectKind::Jump: return "jump";
    }
    return "emit_event";
}

DialogueEffectKind parse_effect_kind(std::string_view value) {
    if (value == "set_bool") return DialogueEffectKind::SetBool;
    if (value == "set_number") return DialogueEffectKind::SetNumber;
    if (value == "add_number") return DialogueEffectKind::AddNumber;
    if (value == "set_string") return DialogueEffectKind::SetString;
    if (value == "jump") return DialogueEffectKind::Jump;
    return DialogueEffectKind::EmitEvent;
}

DialogueVariable parse_variable(const JsonValue& value) {
    if (value.is_bool()) return {.value = value.as_bool()};
    if (value.is_number()) return {.value = value.as_number()};
    if (value.is_string()) return {.value = value.as_string()};
    return {};
}

void write_variable(JsonWriter& json, const DialogueVariable& value) {
    if (const auto* boolean = std::get_if<bool>(&value.value)) {
        json.value(*boolean);
    } else if (const auto* number = std::get_if<f64>(&value.value)) {
        json.value(*number);
    } else if (const auto* text = std::get_if<std::string>(&value.value)) {
        json.value(*text);
    } else {
        json.value_null();
    }
}

std::vector<DialogueCondition> parse_conditions(const JsonValue* raw) {
    std::vector<DialogueCondition> result;
    if (!raw || !raw->is_array()) return result;
    for (const JsonValue& item : raw->items()) {
        DialogueCondition condition;
        condition.kind = parse_condition_kind(item.string_at("kind", "bool"));
        condition.key = item.string_at("key");
        condition.compare = parse_compare(item.string_at("compare", "eq"));
        if (const JsonValue* value = item.find("value")) {
            condition.value = parse_variable(*value);
        }
        result.push_back(std::move(condition));
    }
    return result;
}

std::vector<DialogueEffect> parse_effects(const JsonValue* raw) {
    std::vector<DialogueEffect> result;
    if (!raw || !raw->is_array()) return result;
    for (const JsonValue& item : raw->items()) {
        DialogueEffect effect;
        effect.kind = parse_effect_kind(item.string_at("kind", "emit_event"));
        effect.key = item.string_at("key");
        if (const JsonValue* value = item.find("value")) {
            effect.value = parse_variable(*value);
        }
        result.push_back(std::move(effect));
    }
    return result;
}

void write_conditions(JsonWriter& json, const std::vector<DialogueCondition>& conditions) {
    json.begin_array();
    for (const DialogueCondition& condition : conditions) {
        json.begin_object()
            .field("kind", condition_kind_name(condition.kind))
            .field("key", condition.key)
            .field("compare", compare_name(condition.compare))
            .key("value");
        write_variable(json, condition.value);
        json.end_object();
    }
    json.end_array();
}

void write_effects(JsonWriter& json, const std::vector<DialogueEffect>& effects) {
    json.begin_array();
    for (const DialogueEffect& effect : effects) {
        json.begin_object()
            .field("kind", effect_kind_name(effect.kind))
            .field("key", effect.key)
            .key("value");
        write_variable(json, effect.value);
        json.end_object();
    }
    json.end_array();
}

} // namespace

const DialogueNode* DialoguePlayer::node(std::string_view id) const {
    if (!_document) return nullptr;
    for (const DialogueNode& candidate : _document->nodes) {
        if (candidate.id == id) return &candidate;
    }
    return nullptr;
}

const DialogueSpeaker* DialoguePlayer::speaker(std::string_view id) const {
    if (!_document) return nullptr;
    for (const DialogueSpeaker& candidate : _document->speakers) {
        if (candidate.id == id) return &candidate;
    }
    return nullptr;
}

void DialoguePlayer::start(const DialogueDocument& document, std::string_view start_node) {
    _document = &document;
    _state = {};
    _state.current_node = start_node.empty() ? document.start_node : std::string{start_node};
    resolve_presentable();
}

void DialoguePlayer::reset() {
    _document = nullptr;
    _state = {};
}

void DialoguePlayer::emit(std::string id, std::string node_id) {
    if (id.empty()) return;
    _state.events.push_back({std::move(id), std::move(node_id)});
}

void DialoguePlayer::jump_to(std::string_view id) {
    _state.current_node = std::string{id};
    resolve_presentable();
}

bool DialoguePlayer::condition_met(const DialogueCondition& condition) const {
    if (condition.kind == DialogueConditionKind::VisitedNode) {
        return compare_string(contains_string(_state.visited_nodes, condition.key) ? "true" : "false", condition.compare, bool_value(condition.value) ? "true" : "false");
    }
    if (condition.kind == DialogueConditionKind::VisitedChoice) {
        return compare_string(contains_string(_state.visited_choices, condition.key) ? "true" : "false", condition.compare, bool_value(condition.value) ? "true" : "false");
    }
    const auto found = _state.variables.find(condition.key);
    const DialogueVariable actual = found == _state.variables.end() ? DialogueVariable{} : found->second;
    if (condition.kind == DialogueConditionKind::Number) {
        return compare_number(number_value(actual), condition.compare, number_value(condition.value));
    }
    if (condition.kind == DialogueConditionKind::String) {
        return compare_string(string_value(actual), condition.compare, string_value(condition.value));
    }
    return compare_string(bool_value(actual) ? "true" : "false", condition.compare, bool_value(condition.value) ? "true" : "false");
}

void DialoguePlayer::apply_effect(const DialogueEffect& effect) {
    switch (effect.kind) {
    case DialogueEffectKind::SetBool:
        _state.variables[effect.key] = {.value = bool_value(effect.value)};
        break;
    case DialogueEffectKind::SetNumber:
        _state.variables[effect.key] = {.value = number_value(effect.value)};
        break;
    case DialogueEffectKind::AddNumber:
        _state.variables[effect.key] = {.value = number_value(_state.variables[effect.key]) + number_value(effect.value)};
        break;
    case DialogueEffectKind::SetString:
        _state.variables[effect.key] = {.value = string_value(effect.value)};
        break;
    case DialogueEffectKind::EmitEvent:
        emit(effect.key, _state.current_node);
        break;
    case DialogueEffectKind::Jump:
        _state.current_node = effect.key;
        break;
    }
}

void DialoguePlayer::resolve_presentable() {
    if (!_document || _state.current_node.empty()) {
        _state.ended = true;
        return;
    }
    for (i32 guard = 0; guard < 128; ++guard) {
        const DialogueNode* current = node(_state.current_node);
        if (!current) {
            emit("dialogue.missing_node", _state.current_node);
            _state.ended = true;
            return;
        }
        if (!contains_string(_state.visited_nodes, current->id)) {
            _state.visited_nodes.push_back(current->id);
        }
        switch (current->kind) {
        case DialogueNodeKind::Line:
        case DialogueNodeKind::Choice:
            _state.ended = false;
            return;
        case DialogueNodeKind::Jump:
            _state.current_node = current->target;
            break;
        case DialogueNodeKind::Event:
            emit(current->event_id, current->id);
            for (const DialogueEffect& effect : current->effects) apply_effect(effect);
            if (!current->target.empty()) {
                _state.current_node = current->target;
            }
            break;
        case DialogueNodeKind::End:
            _state.ended = true;
            return;
        }
    }
    emit("dialogue.resolve_guard", _state.current_node);
    _state.ended = true;
}

void DialoguePlayer::advance() {
    const DialogueNode* current = node(_state.current_node);
    if (!current || current->kind != DialogueNodeKind::Line) {
        return;
    }
    _state.history.push_back({current->id, current->line.speaker_id, current->line.text_markup});
    if (current->line.next.empty()) {
        _state.ended = true;
        _state.current_node.clear();
        return;
    }
    jump_to(current->line.next);
}

bool DialoguePlayer::choose(i32 index) {
    const DialogueNode* current = node(_state.current_node);
    if (!current || current->kind != DialogueNodeKind::Choice || index < 0) return false;
    i32 visible = 0;
    for (const DialogueChoice& choice : current->choices) {
        bool enabled = choice.enabled;
        for (const DialogueCondition& condition : choice.conditions) {
            enabled = enabled && condition_met(condition);
        }
        if (!enabled) continue;
        if (visible == index) {
            return choose(choice.id);
        }
        ++visible;
    }
    return false;
}

bool DialoguePlayer::choose(std::string_view choice_id) {
    const DialogueNode* current = node(_state.current_node);
    if (!current || current->kind != DialogueNodeKind::Choice) return false;
    for (const DialogueChoice& choice : current->choices) {
        if (choice.id != choice_id) continue;
        bool enabled = choice.enabled;
        for (const DialogueCondition& condition : choice.conditions) {
            enabled = enabled && condition_met(condition);
        }
        if (!enabled) return false;
        _state.visited_choices.push_back(choice.id);
        for (const DialogueEffect& effect : choice.effects) apply_effect(effect);
        jump_to(choice.target);
        return true;
    }
    return false;
}

void DialoguePlayer::skip_line() {
}

DialogueViewModel DialoguePlayer::current_view(bool show_disabled_choices) const {
    DialogueViewModel view;
    view.active = _document != nullptr && !_state.ended;
    view.ended = _state.ended;
    view.node_id = _state.current_node;
    view.history = _state.history;

    const Localization* l10n = _document ? active_localization() : nullptr;
    std::vector<MessageArg> args;
    if (l10n) {
        args = variable_args(_state.variables);
    }
    // The translation under `key` if there is one, else the document's text.
    const auto localized = [&](const std::string& key, const std::string& fallback) -> std::string {
        if (!l10n || !l10n->has(key)) {
            return fallback;
        }
        return l10n->tr(key, args);
    };
    if (l10n) {
        for (DialogueHistoryEntry& entry : view.history) {
            entry.text_markup = localized(dialogue_text_key(*_document, entry.node_id), entry.text_markup);
        }
    }

    const DialogueNode* current = node(_state.current_node);
    if (!current) return view;
    if (current->kind == DialogueNodeKind::Line) {
        view.speaker_id = current->line.speaker_id;
        view.text_markup = localized(dialogue_text_key(*_document, current->id), current->line.text_markup);
        view.speaker_portrait = current->line.portrait;
        if (const DialogueSpeaker* s = speaker(current->line.speaker_id)) {
            view.speaker_name = s->name;
            if (l10n) {
                const std::string own = dialogue_speaker_key(*_document, s->id);
                const std::string shared = "speakers." + s->id;
                view.speaker_name = l10n->has(own) ? l10n->tr(own, args) : localized(shared, s->name);
            }
            view.speaker_color = s->name_color;
            if (view.speaker_portrait.empty()) view.speaker_portrait = s->portrait;
        }
    } else if (current->kind == DialogueNodeKind::Choice) {
        for (const DialogueChoice& choice : current->choices) {
            bool enabled = choice.enabled;
            for (const DialogueCondition& condition : choice.conditions) enabled = enabled && condition_met(condition);
            if (enabled || show_disabled_choices) {
                view.choices.push_back({choice.id,
                                        localized(dialogue_text_key(*_document, current->id, choice.id), choice.text_markup),
                                        enabled,
                                        contains_string(_state.visited_choices, choice.id)});
            }
        }
    }
    return view;
}

std::string dialogue_text_key(const DialogueDocument& document, std::string_view node_id, std::string_view choice_id) {
    std::string key = dialogue_prefix(document);
    key += node_id;
    if (!choice_id.empty()) {
        key += '.';
        key += choice_id;
    }
    return key;
}

std::string dialogue_speaker_key(const DialogueDocument& document, std::string_view speaker_id) {
    return dialogue_prefix(document) + "speakers." + std::string{speaker_id};
}

LanguageFile dialogue_language_file(const DialogueDocument& document, std::string_view locale) {
    LanguageFile file;
    file.locale = normalize_locale(locale);
    file.name = file.locale;
    for (const DialogueSpeaker& speaker : document.speakers) {
        if (!speaker.name.empty()) {
            file.strings.insert_or_assign("speakers." + speaker.id, speaker.name);
        }
    }
    for (const DialogueNode& node : document.nodes) {
        if (node.kind == DialogueNodeKind::Line && !node.line.text_markup.empty()) {
            file.strings.insert_or_assign(dialogue_text_key(document, node.id), node.line.text_markup);
        }
        for (const DialogueChoice& choice : node.choices) {
            if (!choice.text_markup.empty()) {
                file.strings.insert_or_assign(dialogue_text_key(document, node.id, choice.id), choice.text_markup);
            }
        }
    }
    return file;
}

std::vector<DialogueEvent> DialoguePlayer::consume_events() {
    std::vector<DialogueEvent> result = std::move(_state.events);
    _state.events.clear();
    return result;
}

DialogueStateSnapshot DialoguePlayer::snapshot() const {
    return {
        .current_node = _state.current_node,
        .ended = _state.ended,
        .variables = _state.variables,
        .visited_nodes = _state.visited_nodes,
        .visited_choices = _state.visited_choices,
        .history = _state.history,
    };
}

void DialoguePlayer::restore(const DialogueDocument& document, const DialogueStateSnapshot& snapshot) {
    _document = &document;
    _state = {};
    _state.current_node = snapshot.current_node;
    _state.ended = snapshot.ended;
    _state.variables = snapshot.variables;
    _state.visited_nodes = snapshot.visited_nodes;
    _state.visited_choices = snapshot.visited_choices;
    _state.history = snapshot.history;
    if (!_state.ended) resolve_presentable();
}

DialogueLoadResult parse_dialogue(std::string_view json) {
    JsonParseResult parsed = parse_json(json);
    if (!parsed.ok()) {
        return {.error = parsed.error};
    }
    const JsonValue& root = *parsed.value;
    DialogueDocument document;
    document.id = root.string_at("id");
    document.start_node = root.string_at("start_node");
    if (const JsonValue* speakers = root.find("speakers"); speakers && speakers->is_array()) {
        for (const JsonValue& item : speakers->items()) {
            document.speakers.push_back({
                .id = item.string_at("id"),
                .name = item.string_at("name"),
                .portrait = item.string_at("portrait"),
            });
        }
    }
    if (const JsonValue* nodes = root.find("nodes"); nodes && nodes->is_array()) {
        for (const JsonValue& item : nodes->items()) {
            DialogueNode node;
            node.id = item.string_at("id");
            node.kind = parse_node_kind(item.string_at("kind", "line"));
            node.target = item.string_at("target");
            node.event_id = item.string_at("event");
            node.effects = parse_effects(item.find("effects"));
            if (const JsonValue* line = item.find("line")) {
                node.line.speaker_id = line->string_at("speaker");
                node.line.text_markup = line->string_at("text");
                node.line.voice_cue = line->string_at("voice");
                node.line.portrait = line->string_at("portrait");
                node.line.next = line->string_at("next");
            }
            if (const JsonValue* choices = item.find("choices"); choices && choices->is_array()) {
                for (const JsonValue& raw_choice : choices->items()) {
                    node.choices.push_back({
                        .id = raw_choice.string_at("id"),
                        .text_markup = raw_choice.string_at("text"),
                        .target = raw_choice.string_at("target"),
                        .enabled = raw_choice.bool_at("enabled", true),
                        .conditions = parse_conditions(raw_choice.find("conditions")),
                        .effects = parse_effects(raw_choice.find("effects")),
                    });
                }
            }
            document.nodes.push_back(std::move(node));
        }
    }
    if (document.start_node.empty() && !document.nodes.empty()) {
        document.start_node = document.nodes.front().id;
    }
    return {.document = std::move(document)};
}

DialogueLoadResult load_dialogue(const std::filesystem::path& path) {
    const std::optional<std::string> text = read_content_text(path);
    if (!text) return {.error = "failed to open dialogue file"};
    return parse_dialogue(*text);
}

std::string dialogue_to_json(const DialogueDocument& document) {
    std::ostringstream out;
    JsonWriter json(out);
    json.begin_object()
        .field("id", document.id)
        .field("start_node", document.start_node)
        .key("speakers").begin_array();
    for (const DialogueSpeaker& speaker : document.speakers) {
        json.begin_object()
            .field("id", speaker.id)
            .field("name", speaker.name)
            .field("portrait", speaker.portrait)
            .end_object();
    }
    json.end_array().key("nodes").begin_array();
    for (const DialogueNode& node : document.nodes) {
        json.begin_object()
            .field("id", node.id)
            .field("kind", node_kind_name(node.kind));
        if (!node.target.empty()) json.field("target", node.target);
        if (!node.event_id.empty()) json.field("event", node.event_id);
        if (!node.effects.empty()) json.key("effects"), write_effects(json, node.effects);
        if (node.kind == DialogueNodeKind::Line) {
            json.key("line").begin_object()
                .field("speaker", node.line.speaker_id)
                .field("text", node.line.text_markup)
                .field("voice", node.line.voice_cue)
                .field("portrait", node.line.portrait)
                .field("next", node.line.next)
                .end_object();
        }
        if (!node.choices.empty()) {
            json.key("choices").begin_array();
            for (const DialogueChoice& choice : node.choices) {
                json.begin_object()
                    .field("id", choice.id)
                    .field("text", choice.text_markup)
                    .field("target", choice.target)
                    .field("enabled", choice.enabled);
                if (!choice.conditions.empty()) json.key("conditions"), write_conditions(json, choice.conditions);
                if (!choice.effects.empty()) json.key("effects"), write_effects(json, choice.effects);
                json.end_object();
            }
            json.end_array();
        }
        json.end_object();
    }
    json.end_array().end_object();
    return out.str();
}

bool save_dialogue(const DialogueDocument& document, const std::filesystem::path& path, std::string* error) {
    std::ofstream out(path);
    if (!out) {
        if (error) *error = "failed to open dialogue file for writing";
        return false;
    }
    out << dialogue_to_json(document);
    return true;
}

} // namespace kin
