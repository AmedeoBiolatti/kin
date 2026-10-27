#include <kin/ecs/dialogue.hpp>

#include <kin/assets/content.hpp>
#include <kin/core/utf8.hpp>
#include <kin/ecs/render.hpp>
#include <kin/platform/input.hpp>
#include <kin/platform/log.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <system_error>

namespace kin {
namespace {

using FileTime = std::optional<std::filesystem::file_time_type>;

FileTime time_of(const std::filesystem::path& file) {
    std::error_code error;
    const auto time = std::filesystem::last_write_time(file, error);
    return error ? std::nullopt : FileTime(time);
}

} // namespace

struct SceneDialogue::Read {
    std::shared_ptr<DialogueDocument> document;
    FileTime time;
};

std::string dialogue_plain_text(std::string_view markup) {
    std::string text;
    for (std::size_t i = 0; i < markup.size();) {
        if (markup[i] != '[') {
            text.push_back(markup[i++]);
            continue;
        }
        if (i + 1 < markup.size() && markup[i + 1] == '[') {
            text.push_back('[');
            i += 2;
            continue;
        }
        const std::size_t close = markup.find(']', i + 1);
        if (close == std::string_view::npos) {
            text.push_back(markup[i++]);
            continue;
        }
        i = close + 1; // a tag: not shown
    }
    return text;
}

std::size_t dialogue_letter_count(std::string_view markup) {
    const std::string text = dialogue_plain_text(markup);
    std::size_t count = 0;
    for (std::size_t at = 0; at < text.size(); at = utf8_next(text, at)) {
        ++count;
    }
    return count;
}

SceneDialogue::SceneDialogue(std::filesystem::path content, i32 check_frames)
    : _content(std::move(content)),
      _check_frames(check_frames) {}

SceneDialogue::~SceneDialogue() = default;

std::shared_ptr<DialogueDocument> SceneDialogue::read(const std::string& path) {
    auto found = _read.find(path);
    const std::filesystem::path file = _content / path;
    const FileTime now = time_of(file);
    if (found != _read.end() && found->second->document && found->second->time == now) {
        return found->second->document;
    }
    if (found == _read.end()) {
        found = _read.emplace(path, std::make_unique<Read>()).first;
    }
    found->second->time = now;
    const DialogueLoadResult loaded = load_dialogue(file);
    if (!loaded.ok()) {
        const auto [at, fresh] = _problems.insert_or_assign(path, loaded.error.empty() ? std::string("it could not be read") : loaded.error);
        if (fresh) {
            KIN_LOG_WARN_F("dialogue", "dialogue could not be read", (LogFields{{.name = "path", .value = path}, {.name = "error", .value = at->second}}));
        }
        return found->second->document; // the last good one, if any
    }
    _problems.erase(path);
    found->second->document = std::make_shared<DialogueDocument>(*loaded.document);
    return found->second->document;
}

bool SceneDialogue::start(std::string_view path, std::string_view node) {
    std::shared_ptr<DialogueDocument> document = read(std::string(path));
    if (!document) {
        return false;
    }
    if (active()) {
        stop();
    }
    _document = std::move(document);
    _path = std::string(path);
    _speaker_entity.reset();
    _player.start(*_document, node, _variables);
    _shown_node.clear();
    _events.push_back({"dialogue.started", _path});
    refresh();
    return true;
}

void SceneDialogue::stop() {
    if (!_document) {
        return;
    }
    _player.reset();
    _events.push_back({"dialogue.ended", _path});
    _document.reset();
    _speaker_entity.reset();
    _view = {};
    _shown_node.clear();
    _revealed = 0.0f;
    _letters = 0;
}

bool SceneDialogue::active() const {
    return _document != nullptr && !_player.state().ended;
}

bool SceneDialogue::revealing() const {
    return _revealed < static_cast<f32>(_letters);
}

void SceneDialogue::refresh() {
    for (DialogueEvent& event : _player.consume_events()) {
        _events.push_back(std::move(event));
    }
    _variables = _player.state().variables;
    if (_player.state().ended) {
        stop();
        return;
    }
    _view = _player.current_view();
    // At a choice: the line before it shows with its choices.
    if (!_view.choices.empty() && _view.text_markup.empty() && !_view.history.empty()) {
        const DialogueHistoryEntry& last = _view.history.back();
        _view.text_markup = last.text_markup;
        _view.speaker_id = last.speaker_id;
        for (const DialogueSpeaker& speaker : _document->speakers) {
            if (speaker.id == last.speaker_id) {
                _view.speaker_name = speaker.name;
                _view.speaker_color = speaker.name_color;
                _view.speaker_portrait = speaker.portrait;
            }
        }
    }
    if (_view.node_id != _shown_node) {
        _shown_node = _view.node_id;
        _letters = dialogue_letter_count(_view.text_markup);
        // A choice after its line: the line already read, all of it.
        _revealed = letters_per_second > 0.0f && _view.choices.empty() ? 0.0f : static_cast<f32>(_letters);
    }
}

void SceneDialogue::advance() {
    if (!active()) {
        return;
    }
    if (revealing()) {
        _revealed = static_cast<f32>(_letters); // the whole line first
        return;
    }
    if (!_view.choices.empty()) {
        return; // a choice to make
    }
    _player.advance();
    refresh();
}

bool SceneDialogue::choose(i32 index) {
    if (!active() || !_player.choose(index)) {
        return false;
    }
    refresh();
    return true;
}

bool SceneDialogue::choose(std::string_view id) {
    if (!active() || !_player.choose(id)) {
        return false;
    }
    refresh();
    return true;
}

void SceneDialogue::update(f32 dt) {
    if (active() && revealing()) {
        _revealed = letters_per_second > 0.0f ? std::min(static_cast<f32>(_letters), _revealed + letters_per_second * dt)
                                              : static_cast<f32>(_letters);
    }
    // Its file saved again: read again, and on from where it is (when that node is still there).
    if (_check_frames > 0 && ++_frames >= _check_frames) {
        _frames = 0;
        if (_document && active()) {
            const auto found = _read.find(_path);
            if (found != _read.end() && time_of(_content / _path) != found->second->time) {
                const std::shared_ptr<DialogueDocument> again = read(_path);
                if (again && again != _document) {
                    const DialogueStateSnapshot where = _player.snapshot();
                    const bool there = std::ranges::any_of(again->nodes, [&](const DialogueNode& n) { return n.id == where.current_node; });
                    _document = again;
                    if (there) {
                        _player.restore(*_document, where);
                    } else {
                        _player.start(*_document, {}, _variables);
                    }
                    _shown_node.clear();
                    refresh();
                }
            }
        }
    }
}

DialogueVariable SceneDialogue::variable(std::string_view name) const {
    const auto found = _variables.find(std::string(name));
    return found == _variables.end() ? DialogueVariable{} : found->second;
}

void SceneDialogue::set_variable(const std::string& name, DialogueVariable value) {
    _variables[name] = value;
    if (active()) {
        _player.set_variable(name, std::move(value));
        _view = _player.current_view(); // choices it enables or disables
    }
}

DialogueStateSnapshot SceneDialogue::snapshot() const {
    if (_document) {
        return _player.snapshot();
    }
    DialogueStateSnapshot none;
    none.ended = true;
    none.variables = _variables;
    return none;
}

bool SceneDialogue::restore(std::string_view path, const DialogueStateSnapshot& snapshot) {
    _variables = snapshot.variables;
    if (snapshot.ended || path.empty()) {
        stop();
        return true;
    }
    std::shared_ptr<DialogueDocument> document = read(std::string(path));
    if (!document) {
        return false;
    }
    _document = std::move(document);
    _path = std::string(path);
    _player.restore(*_document, snapshot);
    _shown_node.clear();
    refresh();
    return true;
}

std::optional<EcsEntity> talk_target(EcsWorld& world) {
    std::optional<Vec2f> talker;
    world.raw().each([&](flecs::entity entity, const Talker& who) {
        if (!talker && who.enabled) {
            talker = current_world_transform(entity).pos;
        }
    });
    if (!talker) {
        return std::nullopt;
    }
    std::optional<EcsEntity> best;
    f32 nearest = std::numeric_limits<f32>::max();
    world.raw().each([&](flecs::entity entity, const TalkComponent& talk) {
        if (!talk.enabled || talk.asset.id.empty()) {
            return;
        }
        const Vec2f at = current_world_transform(entity).pos;
        const f32 distance = std::hypot(at.x - talker->x, at.y - talker->y);
        if (distance <= talk.radius && distance < nearest) {
            nearest = distance;
            best = EcsEntity{entity};
        }
    });
    return best;
}

void update_talk(EcsWorld& world, SceneDialogue& dialogue, const Input& input, std::string_view action) {
    if (!input.pressed(action)) {
        return;
    }
    if (dialogue.active()) {
        dialogue.advance();
        return;
    }
    const std::optional<EcsEntity> target = talk_target(world);
    if (!target) {
        return;
    }
    const TalkComponent* talk = target->get<TalkComponent>();
    if (talk && dialogue.start(talk->asset.id, talk->start)) {
        dialogue._speaker_entity = target->id();
    }
}

} // namespace kin
