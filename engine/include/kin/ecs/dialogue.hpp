#pragma once

// Dialogue in a scene: a SceneDialogue runs the dialogue the game started (a
// .kindialogue of its content: kin/dialogue/dialogue.hpp), one at a time,
// its text revealed letter by letter; the game's variables kept between
// dialogues (they read and set them); what happened (its events, it started,
// it ended) told to the game for a step. A UI screen's `dialogue` element
// shows it (SceneScreens, given it), and the player's clicks move it on.
//
//     kin::SceneDialogue dialogue{kin::content_root()};
//     dialogue.start("dialogue/smith.kindialogue");
//     // each step:
//     kin::update_talk(world, dialogue, ctx.input);   // the Talker near a TalkComponent, "talk" pressed
//     if (ctx.input.pressed("talk")) dialogue.advance(); // (update_talk does, while one runs)
//     dialogue.update(ctx.dt);
//     for (const kin::DialogueEvent& e : dialogue.events()) { ... "door_opened" ... }
//     dialogue.clear_events();
//
// Characters say what they say through a TalkComponent: the entity marked
// Talker (the player) near one, pressing the "talk" action, starts its
// dialogue (from its start node, else the file's).

#include <kin/dialogue/dialogue.hpp>
#include <kin/ecs/component.hpp>
#include <kin/ecs/world.hpp>

#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace kin {

class Input;

struct TalkComponent {
    ComponentAssetRef asset{}; // a .kindialogue of the content
    std::string start;         // its node to start at (empty: the file's start)
    f32 radius = 64.0f;        // how near the Talker must be
    std::string prompt = "Talk";
    bool enabled = true;
};

// The one who talks (the player): update_talk measures from it.
struct Talker {
    bool enabled = true;
};

class SceneDialogue {
public:
    explicit SceneDialogue(std::filesystem::path content, i32 check_frames = 30);
    ~SceneDialogue();
    SceneDialogue(const SceneDialogue&) = delete;
    SceneDialogue& operator=(const SceneDialogue&) = delete;

    // `path` (a .kindialogue of the content) started at `node` (empty: its
    // start); false when it can't be read (problems() says why).
    bool start(std::string_view path, std::string_view node = {});
    void stop();
    bool active() const;
    const std::string& path() const { return _path; }
    // Who started it (update_talk's TalkComponent), else none.
    std::optional<EcsId> speaker_entity() const { return _speaker_entity; }

    // The player: on (the whole line first, while it is still being
    // revealed), or a choice (by its place among those shown, or its id).
    void advance();
    bool choose(i32 index);
    bool choose(std::string_view id);

    // Letters revealed a second (0: at once), moved on by update(), which
    // also reads a changed file again (every so many updates).
    f32 letters_per_second = 45.0f;
    void update(f32 dt);

    // What shows now: the line (or, at a choice, the line before it) and its
    // choices; `revealed` letters of the line's text (UTF-8 code points, its
    // markup not counted); revealing() while not all are.
    const DialogueViewModel& view() const { return _view; }
    std::size_t revealed() const { return static_cast<std::size_t>(_revealed); }
    bool revealing() const;
    std::size_t letters() const { return _letters; }

    // The game's variables: kept between dialogues, read and set by them.
    DialogueVariable variable(std::string_view name) const;
    void set_variable(const std::string& name, DialogueVariable value);
    const std::unordered_map<std::string, DialogueVariable>& variables() const { return _variables; }

    // What happened since clear_events(): the dialogue's events, and
    // "dialogue.started" and "dialogue.ended" (node: its path).
    const std::vector<DialogueEvent>& events() const { return _events; }
    void clear_events() { _events.clear(); }

    // For saves: where it is (and the game's variables), and back.
    DialogueStateSnapshot snapshot() const;
    bool restore(std::string_view path, const DialogueStateSnapshot& snapshot);

    const std::map<std::string, std::string>& problems() const { return _problems; }

private:
    struct Read;
    std::shared_ptr<DialogueDocument> read(const std::string& path);
    void refresh(); // the view, the letters, the events, the variables: after the player moved

    std::filesystem::path _content;
    i32 _check_frames = 30;
    i32 _frames = 0;
    std::map<std::string, std::unique_ptr<Read>, std::less<>> _read;
    std::shared_ptr<DialogueDocument> _document; // the one running (kept while it runs)
    std::string _path;
    std::optional<EcsId> _speaker_entity;
    DialoguePlayer _player;
    DialogueViewModel _view;
    f32 _revealed = 0.0f;
    std::size_t _letters = 0;
    std::string _shown_node; // the node the letters are of
    std::unordered_map<std::string, DialogueVariable> _variables;
    std::vector<DialogueEvent> _events;
    std::map<std::string, std::string> _problems;

    friend void update_talk(EcsWorld&, SceneDialogue&, const Input&, std::string_view);
};

// While no dialogue runs: the Talker's nearest TalkComponent in reach starts
// its dialogue when `action` is pressed. While one runs: `action` moves it on.
void update_talk(EcsWorld& world, SceneDialogue& dialogue, const Input& input, std::string_view action = "talk");
// The entity the Talker would talk to now (the nearest in reach); none.
std::optional<EcsEntity> talk_target(EcsWorld& world);

// A dialogue line's markup without its tags ("[b]Hi[/b]": "Hi"), and how
// many letters (code points) that is.
std::string dialogue_plain_text(std::string_view markup);
std::size_t dialogue_letter_count(std::string_view markup);

} // namespace kin
