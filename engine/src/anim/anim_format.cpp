#include <kin/anim/anim_format.hpp>

#include <kin/assets/asset_manager.hpp>
#include <kin/assets/content.hpp>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <variant>

namespace kin {
namespace {

struct Line {
    i32 no = 0;
    i32 indent = 0;
    std::vector<std::string> tok;
};

std::string trim_comment(std::string line) {
    if (const std::size_t comment = line.find('#'); comment != std::string::npos) {
        line.erase(comment);
    }
    return line;
}

std::vector<std::string> tokens(std::string line) {
    std::istringstream in(std::move(line));
    std::vector<std::string> out;
    for (std::string token; in >> token;) {
        out.push_back(std::move(token));
    }
    return out;
}

std::vector<Line> read_lines(std::string_view text) {
    std::vector<Line> lines;
    std::istringstream in(std::string{text});
    std::string raw;
    i32 no = 0;
    while (std::getline(in, raw)) {
        ++no;
        i32 indent = 0;
        while (indent < static_cast<i32>(raw.size()) && (raw[static_cast<std::size_t>(indent)] == ' ' || raw[static_cast<std::size_t>(indent)] == '\t')) {
            ++indent;
        }
        std::vector<std::string> tok = tokens(trim_comment(std::move(raw)));
        if (!tok.empty()) {
            lines.push_back({.no = no, .indent = indent, .tok = std::move(tok)});
        }
    }
    return lines;
}

bool is_header(const std::string& token) {
    return token == "animation" || token == "statemachine";
}

template <typename T>
bool parse_number(const std::string& text, T& out) {
    std::istringstream in(text);
    in >> out;
    return static_cast<bool>(in) && in.eof();
}

bool fail(const Line& line, std::string_view msg, std::string& error) {
    error = "line " + std::to_string(line.no) + ": " + std::string{msg};
    return false;
}

Easing parse_easing(std::string_view value) {
    if (value == "easein") {
        return Easing::EaseIn;
    }
    if (value == "easeout") {
        return Easing::EaseOut;
    }
    if (value == "easeinout") {
        return Easing::EaseInOut;
    }
    return Easing::Linear;
}

std::string easing_text(Easing easing) {
    switch (easing) {
    case Easing::Linear: return "linear";
    case Easing::EaseIn: return "easein";
    case Easing::EaseOut: return "easeout";
    case Easing::EaseInOut: return "easeinout";
    }
    return "linear";
}

bool parse_value(const Line& line, std::size_t start, AnimValue& value, Easing& easing, std::string& error) {
    if (start >= line.tok.size()) {
        return fail(line, "missing key value", error);
    }
    const std::string& kind = line.tok[start];
    if (kind == "float") {
        f32 v = 0.0f;
        if (start + 1 >= line.tok.size() || !parse_number(line.tok[start + 1], v)) {
            return fail(line, "malformed float value", error);
        }
        value = v;
        start += 2;
    } else if (kind == "vec2") {
        Vec2f v{};
        if (start + 2 >= line.tok.size() || !parse_number(line.tok[start + 1], v.x) || !parse_number(line.tok[start + 2], v.y)) {
            return fail(line, "malformed vec2 value", error);
        }
        value = v;
        start += 3;
    } else if (kind == "color") {
        i32 r = 0;
        i32 g = 0;
        i32 b = 0;
        i32 a = 0;
        if (start + 4 >= line.tok.size()
            || !parse_number(line.tok[start + 1], r)
            || !parse_number(line.tok[start + 2], g)
            || !parse_number(line.tok[start + 3], b)
            || !parse_number(line.tok[start + 4], a)) {
            return fail(line, "malformed color value", error);
        }
        value = Color{static_cast<u8>(std::clamp(r, 0, 255)),
                      static_cast<u8>(std::clamp(g, 0, 255)),
                      static_cast<u8>(std::clamp(b, 0, 255)),
                      static_cast<u8>(std::clamp(a, 0, 255))};
        start += 5;
    } else if (kind == "int") {
        i32 v = 0;
        if (start + 1 >= line.tok.size() || !parse_number(line.tok[start + 1], v)) {
            return fail(line, "malformed int value", error);
        }
        value = v;
        start += 2;
    } else if (kind == "bool") {
        i32 v = 0;
        if (start + 1 >= line.tok.size() || !parse_number(line.tok[start + 1], v)) {
            return fail(line, "malformed bool value", error);
        }
        value = v != 0;
        start += 2;
    } else if (kind == "string") {
        if (start + 1 >= line.tok.size()) {
            return fail(line, "malformed string value", error);
        }
        value = line.tok[start + 1];
        start += 2;
    } else {
        return fail(line, "unknown value kind", error);
    }
    if (start < line.tok.size()) {
        easing = parse_easing(line.tok[start]);
    }
    return true;
}

bool parse_node(const std::vector<Line>& lines, std::size_t& index, i32 indent, AnimationNode& out, std::string& error);

bool parse_track(const std::vector<Line>& lines, std::size_t& index, i32 indent, PropertyTrack& out, std::string& error) {
    const Line& line = lines[index++];
    if (line.tok.size() < 2) {
        return fail(line, "malformed track", error);
    }
    out.property = line.tok[1];
    for (std::size_t i = 2; i < line.tok.size(); ++i) {
        if (line.tok[i] == "relative") {
            out.space = TrackSpace::Relative;
        } else if (line.tok[i].starts_with("target=")) {
            out.target = line.tok[i].substr(7);
        }
    }
    while (index < lines.size() && lines[index].indent > indent && !is_header(lines[index].tok[0])) {
        const Line& key_line = lines[index++];
        if (key_line.tok.empty() || key_line.tok[0] != "key" || key_line.tok.size() < 4) {
            return fail(key_line, "track body expects key", error);
        }
        Keyframe key;
        if (!parse_number(key_line.tok[1], key.time) || !parse_value(key_line, 2, key.value, key.easing, error)) {
            return error.empty() ? fail(key_line, "malformed key", error) : false;
        }
        out.keys.push_back(std::move(key));
    }
    return true;
}

bool parse_clip(const std::vector<Line>& lines, std::size_t& index, i32 indent, AnimationNode& out, std::string& error) {
    const Line& line = lines[index++];
    Clip clip;
    if (line.tok.size() < 2 || !parse_number(line.tok[1], clip.duration)) {
        return fail(line, "malformed clip", error);
    }
    while (index < lines.size() && lines[index].indent > indent && !is_header(lines[index].tok[0])) {
        const Line& child = lines[index];
        if (child.tok[0] == "track") {
            PropertyTrack track;
            if (!parse_track(lines, index, child.indent, track, error)) {
                return false;
            }
            clip.properties.push_back(std::move(track));
            continue;
        }
        if (child.tok[0] == "sprite") {
            SpriteTrack track;
            while (index < lines.size() && lines[index].indent == child.indent && lines[index].tok[0] == "sprite") {
                const Line& sprite_line = lines[index++];
                if (sprite_line.tok.size() < 3) {
                    return fail(sprite_line, "malformed sprite", error);
                }
                SpriteKey key;
                key.sprite_id = sprite_line.tok[1];
                if (!parse_number(sprite_line.tok[2], key.time)) {
                    return fail(sprite_line, "malformed sprite time", error);
                }
                for (std::size_t i = 3; i < sprite_line.tok.size(); ++i) {
                    if (sprite_line.tok[i] == "pivot" && i + 2 < sprite_line.tok.size()) {
                        key.has_pivot = parse_number(sprite_line.tok[i + 1], key.pivot.x)
                            && parse_number(sprite_line.tok[i + 2], key.pivot.y);
                        i += 2;
                    } else if (sprite_line.tok[i].starts_with("target=")) {
                        track.target = sprite_line.tok[i].substr(7);
                    }
                }
                track.keys.push_back(std::move(key));
            }
            clip.sprites.push_back(std::move(track));
            continue;
        }
        if (child.tok[0] == "event") {
            EventTrack track;
            while (index < lines.size() && lines[index].indent == child.indent && lines[index].tok[0] == "event") {
                const Line& event_line = lines[index++];
                if (event_line.tok.size() < 3) {
                    return fail(event_line, "malformed event", error);
                }
                EventKey key;
                if (!parse_number(event_line.tok[1], key.time)) {
                    return fail(event_line, "malformed event time", error);
                }
                key.event.channel = event_line.tok[2];
                for (std::size_t i = 3; i < event_line.tok.size(); ++i) {
                    if (event_line.tok[i].starts_with("name=")) {
                        key.event.name = event_line.tok[i].substr(5);
                    } else if (event_line.tok[i].starts_with("value=")) {
                        key.event.value = event_line.tok[i].substr(6);
                    } else if (event_line.tok[i].starts_with("target=")) {
                        key.event.target = event_line.tok[i].substr(7);
                    } else if (event_line.tok[i] == "offset" && i + 2 < event_line.tok.size()) {
                        if (!parse_number(event_line.tok[i + 1], key.event.offset.x) || !parse_number(event_line.tok[i + 2], key.event.offset.y)) {
                            return fail(event_line, "malformed event offset", error);
                        }
                        i += 2;
                    }
                }
                track.keys.push_back(std::move(key));
            }
            clip.events.push_back(std::move(track));
            continue;
        }
        return fail(child, "unknown clip directive", error);
    }
    out = clip_node(std::move(clip));
    return true;
}

bool parse_node(const std::vector<Line>& lines, std::size_t& index, i32 indent, AnimationNode& out, std::string& error) {
    if (index >= lines.size()) {
        error = "missing animation node";
        return false;
    }
    const Line& line = lines[index];
    if (line.indent != indent) {
        return fail(line, "unexpected indentation", error);
    }
    if (line.tok[0] == "clip") {
        return parse_clip(lines, index, indent, out, error);
    }
    if (line.tok[0] == "ref") {
        if (line.tok.size() < 2) {
            return fail(line, "malformed ref", error);
        }
        out = ref_node(line.tok[1]);
        ++index;
        return true;
    }
    if (line.tok[0] == "sequence" || line.tok[0] == "parallel") {
        const bool is_parallel = line.tok[0] == "parallel";
        ParallelEnd end = ParallelEnd::All;
        i32 primary = 0;
        for (std::size_t i = 1; i < line.tok.size(); ++i) {
            if (line.tok[i] == "end=any") {
                end = ParallelEnd::Any;
            } else if (line.tok[i] == "end=all") {
                end = ParallelEnd::All;
            } else if (line.tok[i].starts_with("end=primary:")) {
                end = ParallelEnd::Primary;
                parse_number(line.tok[i].substr(12), primary);
            }
        }
        ++index;
        std::vector<AnimationNode> children;
        while (index < lines.size() && lines[index].indent > indent && !is_header(lines[index].tok[0])) {
            AnimationNode child;
            if (!parse_node(lines, index, lines[index].indent, child, error)) {
                return false;
            }
            children.push_back(std::move(child));
        }
        out = is_parallel ? parallel_node(std::move(children), end) : sequence_node(std::move(children));
        if (is_parallel) {
            std::get<Parallel>(out.value).primary = primary;
        }
        return true;
    }
    if (line.tok[0] == "repeat") {
        i32 count = 0;
        if (line.tok.size() < 2 || !parse_number(line.tok[1], count)) {
            return fail(line, "malformed repeat", error);
        }
        ++index;
        if (index >= lines.size() || lines[index].indent <= indent) {
            return fail(line, "repeat requires child", error);
        }
        AnimationNode child;
        if (!parse_node(lines, index, lines[index].indent, child, error)) {
            return false;
        }
        out = repeat_node(std::move(child), count);
        return true;
    }
    return fail(line, "unknown animation node", error);
}

bool parse_animation(const std::vector<Line>& lines, std::size_t& index, AnimationRegistryFragment& out, std::string& error) {
    const Line& header = lines[index++];
    if (header.tok.size() < 2) {
        return fail(header, "malformed animation header", error);
    }
    Animation animation;
    animation.name = header.tok[1];
    animation.is_template = header.tok.size() > 2 && header.tok[2] == "template";
    if (index >= lines.size() || lines[index].indent <= header.indent) {
        return fail(header, "animation missing body", error);
    }
    if (!parse_node(lines, index, lines[index].indent, animation.root, error)) {
        return false;
    }
    if (!validate(animation, error)) {
        error = "line " + std::to_string(header.no) + ": " + error;
        return false;
    }
    if (animation.is_template) {
        out.animation_templates.push_back(std::move(animation));
    } else {
        out.animations.push_back(std::move(animation));
    }
    return true;
}

bool parse_transition(const Line& line, AnimationStateMachine& machine, std::string& error) {
    if (line.tok.size() < 6 || line.tok[2] != "->" || line.tok[4] != "when") {
        return fail(line, "malformed transition", error);
    }
    AnimTransition transition;
    transition.from = line.tok[1] == "any" ? std::string{} : line.tok[1];
    transition.to = line.tok[3];
    std::size_t i = 5;
    while (i < line.tok.size() && line.tok[i] != "mode") {
        Condition condition;
        if (line.tok[i].starts_with("bool:")) {
            if (i + 2 >= line.tok.size() || line.tok[i + 1] != "==") {
                return fail(line, "malformed bool condition", error);
            }
            condition.param = line.tok[i].substr(5);
            if (line.tok[i + 2] == "true" || line.tok[i + 2] == "1") {
                condition.op = CondOp::IsTrue;
            } else if (line.tok[i + 2] == "false" || line.tok[i + 2] == "0") {
                condition.op = CondOp::IsFalse;
            } else {
                return fail(line, "malformed bool condition", error);
            }
            i += 3;
        } else if (line.tok[i].starts_with("float:")) {
            if (i + 2 >= line.tok.size() || !parse_number(line.tok[i + 2], condition.threshold)) {
                return fail(line, "malformed float condition", error);
            }
            condition.param = line.tok[i].substr(6);
            const std::string& op = line.tok[i + 1];
            if (op == ">") {
                condition.op = CondOp::Greater;
            } else if (op == "<") {
                condition.op = CondOp::Less;
            } else if (op == "==") {
                condition.op = CondOp::Equal;
            } else if (op == "!=") {
                condition.op = CondOp::NotEqual;
            } else {
                return fail(line, "unknown float condition operator", error);
            }
            i += 3;
        } else if (line.tok[i].starts_with("trigger:")) {
            condition.param = line.tok[i].substr(8);
            condition.op = CondOp::TriggerSet;
            ++i;
        } else {
            return fail(line, "unknown condition", error);
        }
        transition.when.push_back(std::move(condition));
    }
    if (transition.when.empty()) {
        return fail(line, "transition requires condition", error);
    }
    if (i < line.tok.size()) {
        if (i + 1 >= line.tok.size()) {
            return fail(line, "malformed transition mode", error);
        }
        if (i + 2 != line.tok.size()) {
            return fail(line, "unexpected transition token", error);
        }
        if (line.tok[i + 1] == "push") {
            transition.mode = TransitionMode::PushOverride;
        } else if (line.tok[i + 1] == "replace") {
            transition.mode = TransitionMode::ReplaceBase;
        } else {
            return fail(line, "unknown transition mode", error);
        }
    }
    machine.transitions.push_back(std::move(transition));
    return true;
}

bool parse_state_machine(const std::vector<Line>& lines, std::size_t& index, AnimationRegistryFragment& out, std::string& error) {
    const Line& header = lines[index++];
    if (header.tok.size() < 2) {
        return fail(header, "malformed state machine header", error);
    }
    const std::string name = header.tok[1];
    const bool is_template = header.tok.size() > 2 && header.tok[2] == "template";
    AnimationStateMachine machine;
    while (index < lines.size() && lines[index].indent > header.indent && !is_header(lines[index].tok[0])) {
        const Line& line = lines[index++];
        if (line.tok[0] == "initial") {
            if (line.tok.size() < 2) {
                return fail(line, "malformed initial", error);
            }
            machine.initial = line.tok[1];
        } else if (line.tok[0] == "state") {
            if (line.tok.size() < 3) {
                return fail(line, "malformed state", error);
            }
            machine.states[line.tok[1]] = line.tok[2];
        } else if (line.tok[0] == "transition") {
            if (!parse_transition(line, machine, error)) {
                return false;
            }
        } else {
            return fail(line, "unknown state machine directive", error);
        }
    }
    if (is_template) {
        out.state_machine_templates[name] = std::move(machine);
    } else {
        out.state_machines[name] = std::move(machine);
    }
    return true;
}

void serialize_value(std::ostream& out, const AnimValue& value) {
    switch (value_kind(value)) {
    case AnimValueKind::Float:
        out << "float " << std::get<f32>(value);
        break;
    case AnimValueKind::Vec2: {
        const Vec2f v = std::get<Vec2f>(value);
        out << "vec2 " << v.x << ' ' << v.y;
        break;
    }
    case AnimValueKind::Color: {
        const Color c = std::get<Color>(value);
        out << "color " << static_cast<i32>(c.r) << ' ' << static_cast<i32>(c.g) << ' ' << static_cast<i32>(c.b) << ' ' << static_cast<i32>(c.a);
        break;
    }
    case AnimValueKind::Int:
        out << "int " << std::get<i32>(value);
        break;
    case AnimValueKind::Bool:
        out << "bool " << (std::get<bool>(value) ? 1 : 0);
        break;
    case AnimValueKind::String:
        out << "string " << std::get<std::string>(value);
        break;
    }
}

void indent(std::ostream& out, i32 count) {
    for (i32 i = 0; i < count; ++i) {
        out << ' ';
    }
}

void serialize_node(std::ostream& out, const AnimationNode& node, i32 pad) {
    if (const auto* clip = std::get_if<Clip>(&node.value)) {
        indent(out, pad);
        out << "clip " << clip->duration << '\n';
        for (const PropertyTrack& track : clip->properties) {
            indent(out, pad + 2);
            out << "track " << track.property;
            if (track.space == TrackSpace::Relative) {
                out << " relative";
            }
            if (!track.target.empty()) {
                out << " target=" << track.target;
            }
            out << '\n';
            for (const Keyframe& key : track.keys) {
                indent(out, pad + 4);
                out << "key " << key.time << ' ';
                serialize_value(out, key.value);
                out << ' ' << easing_text(key.easing) << '\n';
            }
        }
        for (const SpriteTrack& track : clip->sprites) {
            for (const SpriteKey& key : track.keys) {
                indent(out, pad + 2);
                out << "sprite " << key.sprite_id << ' ' << key.time;
                if (key.has_pivot) {
                    out << " pivot " << key.pivot.x << ' ' << key.pivot.y;
                }
                if (!track.target.empty()) {
                    out << " target=" << track.target;
                }
                out << '\n';
            }
        }
        for (const EventTrack& track : clip->events) {
            for (const EventKey& key : track.keys) {
                indent(out, pad + 2);
                out << "event " << key.time << ' ' << key.event.channel;
                if (!key.event.name.empty()) {
                    out << " name=" << key.event.name;
                }
                if (!key.event.value.empty()) {
                    out << " value=" << key.event.value;
                }
                const std::string target = key.event.target.empty() ? track.target : key.event.target;
                if (!target.empty()) {
                    out << " target=" << target;
                }
                if (key.event.offset.x != 0.0f || key.event.offset.y != 0.0f) {
                    out << " offset " << key.event.offset.x << ' ' << key.event.offset.y;
                }
                out << '\n';
            }
        }
        return;
    }
    if (const auto* ref = std::get_if<Ref>(&node.value)) {
        indent(out, pad);
        out << "ref " << ref->name << '\n';
        return;
    }
    if (const auto* sequence = std::get_if<Sequence>(&node.value)) {
        indent(out, pad);
        out << "sequence\n";
        for (const AnimationNode& child : sequence->children) {
            serialize_node(out, child, pad + 2);
        }
        return;
    }
    if (const auto* parallel = std::get_if<Parallel>(&node.value)) {
        indent(out, pad);
        out << "parallel end=";
        switch (parallel->end) {
        case ParallelEnd::All: out << "all"; break;
        case ParallelEnd::Any: out << "any"; break;
        case ParallelEnd::Primary: out << "primary:" << parallel->primary; break;
        }
        out << '\n';
        for (const AnimationNode& child : parallel->children) {
            serialize_node(out, child, pad + 2);
        }
        return;
    }
    const Repeat& repeat = std::get<Repeat>(node.value);
    indent(out, pad);
    out << "repeat " << repeat.count << '\n';
    if (repeat.child) {
        serialize_node(out, *repeat.child, pad + 2);
    }
}

std::string cond_text(const Condition& condition) {
    switch (condition.op) {
    case CondOp::IsTrue:
        return "bool:" + condition.param + " == true";
    case CondOp::IsFalse:
        return "bool:" + condition.param + " == false";
    case CondOp::Greater:
        return "float:" + condition.param + " > " + std::to_string(condition.threshold);
    case CondOp::Less:
        return "float:" + condition.param + " < " + std::to_string(condition.threshold);
    case CondOp::Equal:
        return "float:" + condition.param + " == " + std::to_string(condition.threshold);
    case CondOp::NotEqual:
        return "float:" + condition.param + " != " + std::to_string(condition.threshold);
    case CondOp::TriggerSet:
        return "trigger:" + condition.param;
    }
    return {};
}

u64 file_generation(const std::filesystem::path& path) {
    std::error_code ec;
    const auto time = std::filesystem::last_write_time(path, ec);
    return ec ? 0 : static_cast<u64>(time.time_since_epoch().count());
}

bool has_placeholder(std::string_view text) {
    return text.find('{') != std::string_view::npos || text.find('}') != std::string_view::npos;
}

void add_diag(std::vector<AnimationDiagnostic>& diagnostics, std::string path, std::string message) {
    diagnostics.push_back(AnimationDiagnostic{
        .severity = AnimationDiagnosticSeverity::Error,
        .path = std::move(path),
        .message = std::move(message),
    });
}

void collect_animation_names(const AnimationRegistryFragment& fragment, std::unordered_set<std::string>& names) {
    for (const Animation& animation : fragment.animations) {
        names.insert(animation.name);
    }
    for (const Animation& animation : fragment.animation_templates) {
        names.insert(animation.name);
    }
}

void validate_node_references(const AnimationNode& node,
                              const std::unordered_set<std::string>& names,
                              const SpriteValidator& sprite_validator,
                              std::string_view path,
                              std::vector<AnimationDiagnostic>& diagnostics) {
    if (const auto* clip = std::get_if<Clip>(&node.value)) {
        for (std::size_t track_index = 0; track_index < clip->sprites.size(); ++track_index) {
            const SpriteTrack& track = clip->sprites[track_index];
            for (std::size_t key_index = 0; key_index < track.keys.size(); ++key_index) {
                const SpriteKey& key = track.keys[key_index];
                if (sprite_validator && !has_placeholder(key.sprite_id) && !sprite_validator(key.sprite_id)) {
                    add_diag(diagnostics,
                             std::string{path} + ".sprites[" + std::to_string(track_index) + "].keys[" + std::to_string(key_index) + "]",
                             "missing sprite '" + key.sprite_id + "'");
                }
            }
        }
        return;
    }
    if (const auto* ref = std::get_if<Ref>(&node.value)) {
        if (!has_placeholder(ref->name) && !names.contains(ref->name)) {
            add_diag(diagnostics, std::string{path}, "unresolved ref '" + ref->name + "'");
        }
        return;
    }
    if (const auto* sequence = std::get_if<Sequence>(&node.value)) {
        for (std::size_t i = 0; i < sequence->children.size(); ++i) {
            validate_node_references(sequence->children[i], names, sprite_validator, std::string{path} + ".children[" + std::to_string(i) + "]", diagnostics);
        }
        return;
    }
    if (const auto* parallel = std::get_if<Parallel>(&node.value)) {
        for (std::size_t i = 0; i < parallel->children.size(); ++i) {
            validate_node_references(parallel->children[i], names, sprite_validator, std::string{path} + ".children[" + std::to_string(i) + "]", diagnostics);
        }
        return;
    }
    const Repeat& repeat = std::get<Repeat>(node.value);
    if (repeat.child) {
        validate_node_references(*repeat.child, names, sprite_validator, std::string{path} + ".child", diagnostics);
    }
}

void validate_machine(const AnimationStateMachine& machine,
                      const std::unordered_set<std::string>& names,
                      std::string_view path,
                      std::vector<AnimationDiagnostic>& diagnostics) {
    if (!machine.initial.empty() && !machine.states.contains(machine.initial)) {
        add_diag(diagnostics, std::string{path} + ".initial", "initial state '" + machine.initial + "' is not defined");
    }
    for (const auto& [state, animation] : machine.states) {
        if (!has_placeholder(animation) && !names.contains(animation)) {
            add_diag(diagnostics, std::string{path} + ".states." + state, "missing state animation '" + animation + "'");
        }
    }
    for (std::size_t i = 0; i < machine.transitions.size(); ++i) {
        const AnimTransition& transition = machine.transitions[i];
        if (!transition.from.empty() && !machine.states.contains(transition.from)) {
            add_diag(diagnostics, std::string{path} + ".transitions[" + std::to_string(i) + "]", "transition from state '" + transition.from + "' is not defined");
        }
        if (!machine.states.contains(transition.to)) {
            add_diag(diagnostics, std::string{path} + ".transitions[" + std::to_string(i) + "]", "transition target state '" + transition.to + "' is not defined");
        }
    }
}

} // namespace

bool parse_animation_registry_fragment(std::string_view text, AnimationRegistryFragment& out, std::string& error) {
    out = {};
    error.clear();
    const std::vector<Line> lines = read_lines(text);
    std::size_t index = 0;
    while (index < lines.size()) {
        if (lines[index].tok[0] == "animation") {
            if (!parse_animation(lines, index, out, error)) {
                return false;
            }
        } else if (lines[index].tok[0] == "statemachine") {
            if (!parse_state_machine(lines, index, out, error)) {
                return false;
            }
        } else {
            return fail(lines[index], "expected animation or statemachine", error);
        }
    }
    return true;
}

std::string serialize_animation_registry_fragment(const AnimationRegistryFragment& fragment) {
    std::ostringstream out;
    for (const Animation& animation : fragment.animations) {
        out << "animation " << animation.name << '\n';
        serialize_node(out, animation.root, 2);
        out << '\n';
    }
    for (const Animation& animation : fragment.animation_templates) {
        out << "animation " << animation.name << " template\n";
        serialize_node(out, animation.root, 2);
        out << '\n';
    }
    auto write_machine = [&](const auto& machines, bool is_template) {
        std::vector<std::string> names;
        names.reserve(machines.size());
        for (const auto& [name, machine] : machines) {
            names.push_back(name);
        }
        std::ranges::sort(names);
        for (const std::string& name : names) {
            const AnimationStateMachine& machine = machines.at(name);
            out << "statemachine " << name << (is_template ? " template" : "") << '\n';
            out << "  initial " << machine.initial << '\n';
            std::vector<std::string> states;
            states.reserve(machine.states.size());
            for (const auto& [state, animation] : machine.states) {
                states.push_back(state);
            }
            std::ranges::sort(states);
            for (const std::string& state : states) {
                out << "  state " << state << ' ' << machine.states.at(state) << '\n';
            }
            for (const AnimTransition& transition : machine.transitions) {
                out << "  transition " << (transition.from.empty() ? "any" : transition.from) << " -> " << transition.to << " when";
                for (const Condition& condition : transition.when) {
                    out << ' ' << cond_text(condition);
                }
                out << " mode " << (transition.mode == TransitionMode::PushOverride ? "push" : "replace") << '\n';
            }
            out << '\n';
        }
    };
    write_machine(fragment.state_machines, false);
    write_machine(fragment.state_machine_templates, true);
    return out.str();
}

std::vector<AnimationDiagnostic> validate_animation_registry_fragment(const AnimationRegistryFragment& fragment,
                                                                      SpriteValidator sprite_validator) {
    std::vector<AnimationDiagnostic> diagnostics;
    std::unordered_set<std::string> names;
    collect_animation_names(fragment, names);

    for (const Animation& animation : fragment.animations) {
        validate_node_references(animation.root, names, sprite_validator, "animation." + animation.name, diagnostics);
    }
    for (const Animation& animation : fragment.animation_templates) {
        validate_node_references(animation.root, names, sprite_validator, "animation_template." + animation.name, diagnostics);
    }
    for (const auto& [name, machine] : fragment.state_machines) {
        validate_machine(machine, names, "statemachine." + name, diagnostics);
    }
    for (const auto& [name, machine] : fragment.state_machine_templates) {
        validate_machine(machine, names, "statemachine_template." + name, diagnostics);
    }
    return diagnostics;
}

AnimationRegistryFragment load_animation_registry_fragment(const std::filesystem::path& path) {
    const std::optional<std::string> text = read_content_text(path);
    if (!text) {
        throw std::runtime_error("Failed to open animation asset: " + path.string());
    }
    AnimationRegistryFragment fragment;
    std::string error;
    if (!parse_animation_registry_fragment(*text, fragment, error)) {
        throw std::runtime_error(path.string() + ": " + error);
    }
    fragment.source_path = path;
    fragment.generation = file_generation(path);
    return fragment;
}

bool save_animation_registry_fragment(const AnimationRegistryFragment& fragment, const std::filesystem::path& path) {
    std::ofstream out(path);
    if (!out) {
        return false;
    }
    out << serialize_animation_registry_fragment(fragment);
    return static_cast<bool>(out);
}

AnimationStateMachine load_animation_state_machine_asset(const std::filesystem::path& path) {
    AnimationRegistryFragment fragment = load_animation_registry_fragment(path);
    if (!fragment.state_machines.empty()) {
        return fragment.state_machines.begin()->second;
    }
    if (!fragment.state_machine_templates.empty()) {
        return fragment.state_machine_templates.begin()->second;
    }
    throw std::runtime_error("Animation state machine asset has no statemachine: " + path.string());
}

bool save_animation_state_machine_asset(const AnimationStateMachine& machine, const std::filesystem::path& path) {
    AnimationRegistryFragment fragment;
    fragment.state_machines["machine"] = machine;
    return save_animation_registry_fragment(fragment, path);
}

bool AnimationAssetLibrary::merge(const AnimationRegistryFragment& fragment, SpriteValidator sprite_validator) {
    registry.merge(fragment);
    for (const auto& [name, machine] : fragment.state_machines) {
        state_machines[name] = std::make_shared<AnimationStateMachine>(machine);
    }
    for (const auto& [name, machine] : fragment.state_machine_templates) {
        state_machine_templates[name] = std::make_shared<AnimationStateMachine>(machine);
    }
    if (!fragment.source_path.empty()) {
        _generations[fragment.source_path.generic_string()] = fragment.generation;
    }
    _diagnostics = validate_animation_registry_fragment(fragment, std::move(sprite_validator));
    if (!fragment.source_path.empty()) {
        _diagnostics_by_source[fragment.source_path.generic_string()] = _diagnostics;
    }
    return true;
}

std::vector<AnimationDiagnostic> AnimationAssetLibrary::diagnostics_for(const std::filesystem::path& source_path) const {
    const auto found = _diagnostics_by_source.find(source_path.generic_string());
    return found == _diagnostics_by_source.end() ? std::vector<AnimationDiagnostic>{} : found->second;
}

bool AnimationAssetLibrary::reload_if_changed(const std::filesystem::path& path, std::string& error, SpriteValidator sprite_validator) {
    error.clear();
    const u64 generation = file_generation(path);
    const std::string key = path.generic_string();
    if (_generations.contains(key) && _generations[key] == generation) {
        return false;
    }
    try {
        AnimationRegistryFragment fragment = load_animation_registry_fragment(path);
        merge(fragment, std::move(sprite_validator));
        return true;
    } catch (const std::exception& ex) {
        error = ex.what();
        return false;
    }
}

void register_animation_asset_loaders(AssetManager& assets) {
    assets.register_loader<AnimationRegistryFragment>([](const std::filesystem::path& path) {
        return load_animation_registry_fragment(path);
    });
    assets.register_loader<AnimationStateMachine>([](const std::filesystem::path& path) {
        return load_animation_state_machine_asset(path);
    });
}

} // namespace kin
