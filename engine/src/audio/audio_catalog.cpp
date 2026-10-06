#include <kin/audio/audio_catalog.hpp>

#include <kin/platform/log.hpp>

#include <algorithm>
#include <charconv>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace kin {
namespace {

std::string trim_comment(std::string line) {
    if (const auto comment = line.find('#'); comment != std::string::npos) {
        line.erase(comment);
    }
    return line;
}

bool parse_f32(std::string_view text, f32& value) {
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto parsed = std::from_chars(begin, end, value);
    return parsed.ec == std::errc{} && parsed.ptr == end;
}

bool parse_i32(std::string_view text, i32& value) {
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto parsed = std::from_chars(begin, end, value);
    return parsed.ec == std::errc{} && parsed.ptr == end;
}

bool parse_i64(std::string_view text, i64& value) {
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto parsed = std::from_chars(begin, end, value);
    return parsed.ec == std::errc{} && parsed.ptr == end && value >= 0;
}

bool parse_bool(std::string_view text, bool& value) {
    if (text == "true" || text == "1" || text == "yes") {
        value = true;
        return true;
    }
    if (text == "false" || text == "0" || text == "no") {
        value = false;
        return true;
    }
    return false;
}

std::vector<std::string> split_csv(std::string_view text) {
    std::vector<std::string> result;
    while (!text.empty()) {
        const std::size_t comma = text.find(',');
        const std::string_view item = comma == std::string_view::npos ? text : text.substr(0, comma);
        if (!item.empty()) {
            result.emplace_back(item);
        }
        if (comma == std::string_view::npos) {
            break;
        }
        text = text.substr(comma + 1);
    }
    return result;
}

} // namespace

void AudioCatalog::clear() {
    _clips.clear();
    _buses.clear();
    _cues.clear();
    _ducks.clear();
}

void AudioCatalog::set_root(std::filesystem::path root) {
    _root = std::move(root);
}

void AudioCatalog::add_clip(AudioClipRef clip_value) {
    if (!clip_value.id.empty()) {
        _clips[clip_value.id] = std::move(clip_value);
    }
}

void AudioCatalog::add_bus(AudioBus bus_value) {
    if (!bus_value.id.empty()) {
        bus_value.fade_target = bus_value.volume;
        _buses[bus_value.id] = std::move(bus_value);
    }
}

void AudioCatalog::add_cue(AudioCue cue_value) {
    if (!cue_value.id.empty()) {
        _cues[cue_value.id] = std::move(cue_value);
    }
}

void AudioCatalog::add_duck(AudioDuck duck_value) {
    if (duck_value.bus.empty() || duck_value.when.empty()) {
        return;
    }
    // One rule per pair: a later one replaces it.
    const auto same = std::ranges::find_if(_ducks, [&](const AudioDuck& existing) {
        return existing.bus == duck_value.bus && existing.when == duck_value.when;
    });
    if (same != _ducks.end()) {
        *same = std::move(duck_value);
    } else {
        _ducks.push_back(std::move(duck_value));
    }
}

const AudioClipRef* AudioCatalog::clip(std::string_view id) const {
    const auto found = _clips.find(std::string{id});
    return found == _clips.end() ? nullptr : &found->second;
}

const AudioBus* AudioCatalog::bus(std::string_view id) const {
    const auto found = _buses.find(std::string{id});
    return found == _buses.end() ? nullptr : &found->second;
}

AudioBus* AudioCatalog::bus(std::string_view id) {
    const auto found = _buses.find(std::string{id});
    return found == _buses.end() ? nullptr : &found->second;
}

const AudioCue* AudioCatalog::cue(std::string_view id) const {
    const auto found = _cues.find(std::string{id});
    return found == _cues.end() ? nullptr : &found->second;
}

std::filesystem::path AudioCatalog::resolve_clip_path(std::string_view clip_id) const {
    const AudioClipRef* ref = clip(clip_id);
    return ref ? _root / std::filesystem::path{ref->path} : std::filesystem::path{};
}

f32 AudioCatalog::effective_bus_volume(std::string_view bus_id) const {
    f32 volume = 1.0f;
    std::string current{bus_id};
    i32 guard = 0;
    while (!current.empty() && guard++ < 32) {
        const AudioBus* bus_value = bus(current);
        if (!bus_value) {
            break;
        }
        if (bus_value->muted) {
            return 0.0f;
        }
        volume *= bus_value->volume;
        current = bus_value->parent;
    }
    return volume;
}

std::string_view audio_category_name(AudioCategory category) {
    switch (category) {
    case AudioCategory::Music: return "music";
    case AudioCategory::Ambient: return "ambient";
    case AudioCategory::Sound: return "sound";
    case AudioCategory::Effect: return "effect";
    case AudioCategory::Ui: return "ui";
    }
    return "sound";
}

std::string_view audio_effect_type_name(AudioEffectType type) {
    switch (type) {
    case AudioEffectType::LowPass: return "lowpass";
    case AudioEffectType::HighPass: return "highpass";
    case AudioEffectType::Reverb: return "reverb";
    case AudioEffectType::Compressor: return "compressor";
    }
    return "lowpass";
}

bool parse_audio_effect_type(std::string_view value, AudioEffectType& out) {
    for (const AudioEffectType type : {AudioEffectType::LowPass, AudioEffectType::HighPass, AudioEffectType::Reverb,
                                       AudioEffectType::Compressor}) {
        if (value == audio_effect_type_name(type)) {
            out = type;
            return true;
        }
    }
    return false;
}

bool parse_audio_category(std::string_view value, AudioCategory& out) {
    if (value == "music") {
        out = AudioCategory::Music;
        return true;
    }
    if (value == "ambient") {
        out = AudioCategory::Ambient;
        return true;
    }
    if (value == "sound" || value == "sfx") {
        out = AudioCategory::Sound;
        return true;
    }
    if (value == "effect") {
        out = AudioCategory::Effect;
        return true;
    }
    if (value == "ui") {
        out = AudioCategory::Ui;
        return true;
    }
    return false;
}

AudioCatalog load_audio_catalog(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file) {
        KIN_LOG_ERROR_F("asset",
                        "audio catalog open failed",
                        (LogFields{
                            {.name = "path", .value = path.string()},
                            {.name = "type", .value = "AudioCatalog"},
                        }));
        throw std::runtime_error("Failed to open audio catalog: " + path.string());
    }

    AudioCatalog catalog;
    catalog.set_root(path.parent_path());
    std::string line;
    i32 line_no = 0;
    const auto fail = [&](std::string_view message) {
        throw std::runtime_error(path.string() + ":" + std::to_string(line_no) + ": " + std::string{message});
    };

    while (std::getline(file, line)) {
        ++line_no;
        std::istringstream in(trim_comment(std::move(line)));
        std::string kind;
        if (!(in >> kind)) {
            continue;
        }

        if (kind == "bus") {
            AudioBus bus;
            if (!(in >> bus.id >> bus.volume)) {
                fail("bus requires id and volume");
            }
            in >> bus.parent;
            catalog.add_bus(std::move(bus));
            continue;
        }

        if (kind == "clip") {
            AudioClipRef clip;
            if (!(in >> clip.id >> clip.path)) {
                fail("clip requires id and path");
            }
            for (std::string token; in >> token;) {
                const std::size_t eq = token.find('=');
                if (eq == std::string::npos) {
                    fail("clip options must be key=value");
                }
                const std::string_view key{token.data(), eq};
                const std::string_view value{token.data() + eq + 1, token.size() - eq - 1};
                if (key == "stream") {
                    if (!parse_bool(value, clip.stream)) fail("invalid stream");
                } else if (key == "loop_start") {
                    if (!parse_i64(value, clip.loop_start)) fail("invalid loop_start");
                } else if (key == "loop_end") {
                    if (!parse_i64(value, clip.loop_end)) fail("invalid loop_end");
                } else {
                    fail("unknown clip option");
                }
            }
            if (clip.loop_end > 0 && clip.loop_end <= clip.loop_start) {
                fail("loop_end must be after loop_start");
            }
            catalog.add_clip(std::move(clip));
            continue;
        }

        if (kind == "cue") {
            AudioCue cue;
            std::string category_text;
            if (!(in >> cue.id >> category_text >> cue.bus)) {
                fail("cue requires id, category, and bus");
            }
            if (!parse_audio_category(category_text, cue.category)) {
                fail("unknown cue category");
            }

            for (std::string token; in >> token;) {
                const std::size_t eq = token.find('=');
                if (eq == std::string::npos) {
                    fail("cue options must be key=value");
                }
                const std::string_view key{token.data(), eq};
                const std::string_view value{token.data() + eq + 1, token.size() - eq - 1};
                if (key == "priority") {
                    if (!parse_i32(value, cue.priority)) fail("invalid priority");
                } else if (key == "max_instances") {
                    if (!parse_i32(value, cue.max_instances)) fail("invalid max_instances");
                } else if (key == "volume") {
                    if (!parse_f32(value, cue.volume)) fail("invalid volume");
                } else if (key == "pitch") {
                    if (!parse_f32(value, cue.pitch)) fail("invalid pitch");
                } else if (key == "pitch_var") {
                    if (!parse_f32(value, cue.pitch_variance)) fail("invalid pitch_var");
                } else if (key == "loop") {
                    if (!parse_bool(value, cue.loop)) fail("invalid loop");
                } else if (key == "spatial") {
                    if (!parse_bool(value, cue.spatial)) fail("invalid spatial");
                } else if (key == "min") {
                    if (!parse_f32(value, cue.min_distance)) fail("invalid min");
                } else if (key == "max") {
                    if (!parse_f32(value, cue.max_distance)) fail("invalid max");
                } else if (key == "clips") {
                    cue.clips = split_csv(value);
                } else {
                    fail("unknown cue option");
                }
            }
            if (cue.clips.empty()) {
                fail("cue requires clips");
            }
            catalog.add_cue(std::move(cue));
            continue;
        }

        if (kind == "effect") {
            std::string bus_id;
            std::string type_text;
            AudioEffect effect;
            if (!(in >> bus_id >> type_text)) {
                fail("effect requires a bus and a type");
            }
            if (!parse_audio_effect_type(type_text, effect.type)) {
                fail("unknown effect type");
            }
            AudioBus* bus = catalog.bus(bus_id);
            if (!bus) {
                fail("effect on a bus not declared above it");
            }
            for (std::string token; in >> token;) {
                const std::size_t eq = token.find('=');
                if (eq == std::string::npos) {
                    fail("effect options must be key=value");
                }
                const std::string_view key{token.data(), eq};
                const std::string_view value{token.data() + eq + 1, token.size() - eq - 1};
                f32* field = key == "cutoff" ? &effect.cutoff
                    : key == "q" ? &effect.q
                    : key == "room" ? &effect.room_size
                    : key == "damping" ? &effect.damping
                    : key == "wet" ? &effect.wet
                    : key == "dry" ? &effect.dry
                    : key == "width" ? &effect.width
                    : key == "threshold" ? &effect.threshold
                    : key == "ratio" ? &effect.ratio
                    : key == "attack" ? &effect.attack
                    : key == "release" ? &effect.release
                    : key == "makeup" ? &effect.makeup
                    : nullptr;
                if (key == "enabled") {
                    if (!parse_bool(value, effect.enabled)) fail("invalid enabled");
                } else if (!field) {
                    fail("unknown effect option");
                } else if (!parse_f32(value, *field)) {
                    fail("invalid effect value");
                }
            }
            bus->effects.push_back(effect);
            continue;
        }

        if (kind == "duck") {
            AudioDuck duck;
            if (!(in >> duck.bus)) {
                fail("duck requires a bus");
            }
            for (std::string token; in >> token;) {
                const std::size_t eq = token.find('=');
                if (eq == std::string::npos) {
                    fail("duck options must be key=value");
                }
                const std::string_view key{token.data(), eq};
                const std::string_view value{token.data() + eq + 1, token.size() - eq - 1};
                if (key == "when") {
                    duck.when = std::string{value};
                } else if (key == "volume") {
                    if (!parse_f32(value, duck.volume) || duck.volume < 0.0f) fail("invalid volume");
                } else if (key == "attack") {
                    if (!parse_f32(value, duck.attack) || duck.attack < 0.0f) fail("invalid attack");
                } else if (key == "release") {
                    if (!parse_f32(value, duck.release) || duck.release < 0.0f) fail("invalid release");
                } else {
                    fail("unknown duck option");
                }
            }
            if (duck.when.empty()) {
                fail("duck requires when=<bus>");
            }
            catalog.add_duck(std::move(duck));
            continue;
        }

        fail("unknown directive");
    }

    KIN_LOG_INFO_F("asset",
                   "audio catalog loaded",
                   (LogFields{
                       {.name = "path", .value = path.string()},
                       {.name = "type", .value = "AudioCatalog"},
                       {.name = "buses", .value = std::to_string(catalog.buses().size())},
                       {.name = "clips", .value = std::to_string(catalog.clips().size())},
                       {.name = "cues", .value = std::to_string(catalog.cues().size())},
                       {.name = "ducks", .value = std::to_string(catalog.ducks().size())},
                   }));
    return catalog;
}

bool save_audio_catalog(const AudioCatalog& catalog, const std::filesystem::path& path) {
    std::ofstream out(path);
    if (!out) {
        return false;
    }

    out << "# kin audio catalog\n";
    std::vector<const AudioBus*> buses;
    buses.reserve(catalog.buses().size());
    for (const auto& [id, bus] : catalog.buses()) {
        buses.push_back(&bus);
    }
    std::ranges::sort(buses, [](const AudioBus* a, const AudioBus* b) { return a->id < b->id; });
    for (const AudioBus* bus : buses) {
        out << "bus " << bus->id << ' ' << bus->volume;
        if (!bus->parent.empty()) {
            out << ' ' << bus->parent;
        }
        out << '\n';
    }

    std::vector<const AudioClipRef*> clips;
    clips.reserve(catalog.clips().size());
    for (const auto& [id, clip] : catalog.clips()) {
        clips.push_back(&clip);
    }
    std::ranges::sort(clips, [](const AudioClipRef* a, const AudioClipRef* b) { return a->id < b->id; });
    if (!clips.empty()) {
        out << '\n';
    }
    for (const AudioClipRef* clip : clips) {
        out << "clip " << clip->id << ' ' << clip->path;
        if (clip->stream) {
            out << " stream=true";
        }
        if (clip->loop_start > 0) {
            out << " loop_start=" << clip->loop_start;
        }
        if (clip->loop_end > 0) {
            out << " loop_end=" << clip->loop_end;
        }
        out << '\n';
    }

    std::vector<const AudioCue*> cues;
    cues.reserve(catalog.cues().size());
    for (const auto& [id, cue] : catalog.cues()) {
        cues.push_back(&cue);
    }
    std::ranges::sort(cues, [](const AudioCue* a, const AudioCue* b) { return a->id < b->id; });
    if (!cues.empty()) {
        out << '\n';
    }
    for (const AudioCue* cue : cues) {
        out << "cue " << cue->id << ' ' << audio_category_name(cue->category) << ' ' << cue->bus
            << " priority=" << cue->priority
            << " max_instances=" << cue->max_instances
            << " volume=" << cue->volume
            << " pitch=" << cue->pitch
            << " pitch_var=" << cue->pitch_variance
            << " loop=" << (cue->loop ? "true" : "false")
            << " spatial=" << (cue->spatial ? "true" : "false")
            << " min=" << cue->min_distance
            << " max=" << cue->max_distance
            << " clips=";
        for (std::size_t i = 0; i < cue->clips.size(); ++i) {
            if (i > 0) {
                out << ',';
            }
            out << cue->clips[i];
        }
        out << '\n';
    }

    bool first_effect = true;
    for (const AudioBus* bus : buses) {
        for (const AudioEffect& effect : bus->effects) {
            out << (first_effect ? "\n" : "") << "effect " << bus->id << ' ' << audio_effect_type_name(effect.type);
            first_effect = false;
            if (!effect.enabled) {
                out << " enabled=false";
            }
            switch (effect.type) {
            case AudioEffectType::LowPass:
            case AudioEffectType::HighPass:
                out << " cutoff=" << effect.cutoff << " q=" << effect.q;
                break;
            case AudioEffectType::Reverb:
                out << " room=" << effect.room_size << " damping=" << effect.damping << " wet=" << effect.wet
                    << " dry=" << effect.dry << " width=" << effect.width;
                break;
            case AudioEffectType::Compressor:
                out << " threshold=" << effect.threshold << " ratio=" << effect.ratio << " attack=" << effect.attack
                    << " release=" << effect.release << " makeup=" << effect.makeup;
                break;
            }
            out << '\n';
        }
    }

    if (!catalog.ducks().empty()) {
        out << '\n';
    }
    for (const AudioDuck& duck : catalog.ducks()) {
        out << "duck " << duck.bus << " when=" << duck.when << " volume=" << duck.volume
            << " attack=" << duck.attack << " release=" << duck.release << '\n';
    }

    return static_cast<bool>(out);
}

void register_audio_catalog_loader(AssetManager& assets) {
    assets.register_loader<AudioCatalog>([](const std::filesystem::path& path) {
        return load_audio_catalog(path);
    });
}

} // namespace kin
