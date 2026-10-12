#include "script.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace summit {

namespace {

f32 wrap(f32 d) {
    d = std::fmod(d + 180.0f, 360.0f);
    if (d < 0.0f) d += 360.0f;
    return d - 180.0f;
}

// The yaw (degrees; 0 looks along -Z) that looks along the horizontal part of `d`.
f32 yaw_to(Vec3f d) { return std::atan2(-d.x, -d.z) * 180.0f / pi; }

f32 flat_distance(Vec3f a, Vec3f b) { return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.z - b.z) * (a.z - b.z)); }

} // namespace

Bot::Bot(const Game& game, i32 player, BotConfig config)
    : _game(&game), _player(player), _config(config), _route(game.level().route) {
    _last_feet = game.feet(player);
}

void Bot::start_at(i32 leg) {
    _leg = std::clamp(leg, 0, i32(_route.size()));
    _sub = Sub::Start;
    _leg_steps = _sub_steps = 0;
}

void Bot::advance_leg() {
    ++_leg;
    _sub = Sub::Start;
    _leg_steps = _sub_steps = 0;
}

f32 Bot::yaw_error(f32 yaw) const { return wrap(yaw - _game->player(_player).yaw); }

void Bot::aim(PlayerCommand& c, f32 yaw, f32 pitch) const {
    const Player& p = _game->player(_player);
    const f32 e = std::clamp(wrap(yaw - p.yaw), -10.0f, 10.0f);
    c.look.x = -e;
    c.look.y = std::clamp(pitch - p.pitch, -6.0f, 6.0f);
}

const char* Bot::phase() const {
    static const char* subs[] = {"start", "rest", "face", "approach", "climb", "shimmy", "climb2", "mantle", "rope", "handover", "step", "done"};
    return subs[int(_sub)];
}

std::string Bot::status() const {
    static const char* subs[] = {"start", "rest", "face", "approach", "climb", "shimmy", "climb2", "mantle", "rope", "handover", "step", "done"};
    if (finished()) return "finished";
    std::string out = "leg " + std::to_string(_leg) + " " + _route[std::size_t(_leg)].label + " / " + subs[int(_sub)];
    if (_sub == Sub::Handover) {
        const TraversalState3D ts = _game->traversal(_player);
        out += " hand " + std::to_string(_hand) + " L" + std::to_string(int(ts.grips[0].phase)) + "k" + std::to_string(int(ts.grips[0].kind)) + " R" +
               std::to_string(int(ts.grips[1].phase)) + "k" + std::to_string(int(ts.grips[1].kind));
    }
    return out;
}

PlayerCommand Bot::next() {
    PlayerCommand c;
    if (finished()) return c;
    const Player& p = _game->player(_player);
    if (p.state != PlayerState::Alive) {
        // Lying, getting up or dead: nothing to do but wait (and the leg may have to be redone after).
        return c;
    }
    const CharacterState3D cs = _game->character_state(_player);
    const TraversalState3D ts = _game->traversal(_player);
    const f32 stamina = ts.resource.value, cap = ts.resource.max;
    const Vec3f feet = cs.position;
    const Leg& L = _route[std::size_t(_leg)];
    ++_leg_steps;
    ++_sub_steps;

    if (_drops > 0) {
        --_drops;
        c.release = true;
        _falling = true;
        return c;
    }
    // A scripted fall: nothing is asked of the hands until it lands.
    if (_falling) {
        if (cs.ground == CharacterGround3D::OnGround && !ts.hanging()) _falling = false;
        else return c;
    }
    if (_leg == _config.drop_leg && !_dropped && ts.hanging() && feet.y >= L.from.y + _config.drop_height) {
        _dropped = true;
        _drops = 6;
        c.release = true;
        return c;
    }

    // Fallen below where the leg starts: go back to a leg that is at this height (after a fall, a respawn).
    if (cs.ground == CharacterGround3D::OnGround && !ts.hanging() && L.kind != LegKind::Summit && feet.y < L.from.y - 2.5f) {
        i32 j = _leg;
        while (j > 0 && !((_route[std::size_t(j)].kind == LegKind::Walk || _route[std::size_t(j)].kind == LegKind::Rest) &&
                          _route[std::size_t(j)].to.y <= feet.y + 1.5f)) {
            --j;
        }
        ++_restarts;
        start_at(j);
        return c;
    }

    const auto to_point = [&](Vec3f target, bool sprint = false) {
        const Vec3f d = target - feet;
        const f32 yaw = yaw_to(d);
        aim(c, yaw, 0.0f);
        const f32 e = std::abs(yaw_error(yaw));
        c.move.y = e < 25.0f ? 1.0f : 0.0f;
        c.sprint = sprint && e < 10.0f;
        // Not getting anywhere: a hop.
        if (_leg_steps % 90 == 0) {
            if (flat_distance(feet, _last_feet) < 0.25f && cs.ground == CharacterGround3D::OnGround) _stuck = 12;
            _last_feet = feet;
        }
        if (_stuck > 0) {
            --_stuck;
            c.jump = true;
        }
    };

    switch (L.kind) {
    case LegKind::Walk: {
        const Vec3f d = L.to - feet;
        const bool arrived = flat_distance(feet, L.to) < (L.camp >= 0 ? 1.5f : 1.0f) && std::abs(d.y) < 2.0f;
        if (arrived) {
            advance_leg();
            return c;
        }
        to_point(L.to);
        break;
    }
    case LegKind::Rest: {
        if (stamina >= cap * _config.rest_to || _sub_steps > 1200) {
            advance_leg();
        }
        break;
    }
    case LegKind::Summit: {
        break;
    }
    case LegKind::Climb:
    case LegKind::Overhang: {
        const Vec3f wall_yaw_dir = -L.normal;
        const f32 wall_yaw = yaw_to(wall_yaw_dir);
        const Vec3f right{L.normal.z, 0.0f, -L.normal.x};
        if (_sub == Sub::Start) {
            _sub = Sub::Rest;
            _sub_steps = 0;
        }
        if (_sub == Sub::Rest) {
            // On the ground (or in a camp) until the bar is full: the planner costs a climb from a full bar.
            if (stamina >= cap * _config.rest_to || _sub_steps > 1500) {
                _sub = Sub::Step;
                _sub_steps = 0;
            }
            break;
        }
        if (_sub == Sub::Step) {
            // To the foot of the face.
            if (flat_distance(feet, L.from) < 0.4f) {
                _sub = Sub::Face;
                _sub_steps = 0;
            } else {
                to_point(L.from);
            }
            break;
        }
        if (_sub == Sub::Face) {
            aim(c, wall_yaw, 10.0f);
            if (std::abs(yaw_error(wall_yaw)) < 2.0f) {
                _sub = Sub::Approach;
                _sub_steps = 0;
            }
            break;
        }
        // From here the hands are asked for.
        c.grab_left = c.grab_right = true;
        aim(c, wall_yaw, 10.0f);
        if (_sub == Sub::Approach) {
            c.move.y = ts.held >= 1 ? 0.0f : 0.6f;
            if (ts.held >= 2) {
                _sub = Sub::Climb;
                _sub_steps = 0;
                _mark = feet;
            } else if (_sub_steps > 240) {
                // Missed: back off and again.
                c.grab_left = c.grab_right = false;
                ++_restarts;
                _sub = Sub::Step;
                _sub_steps = 0;
            }
            break;
        }
        if (_sub == Sub::Climb) {
            c.move.y = 1.0f;
            if (ts.mode == TraversalMode3D::Mantling) {
                _sub = Sub::Mantle;
                _sub_steps = 0;
            } else if (L.kind == LegKind::Overhang && feet.y >= L.from.y + L.roof_height - 2.05f) {
                _sub = Sub::Shimmy;
                _sub_steps = 0;
                _mark = feet;
                c.move.y = 0.0f;
            } else if (ts.held == 0 && cs.ground == CharacterGround3D::OnGround && _sub_steps > 30) {
                c.grab_left = c.grab_right = false;
                ++_restarts;
                _sub = Sub::Step;
                _sub_steps = 0;
            }
            break;
        }
        if (_sub == Sub::Shimmy) {
            const f32 progress = dot(feet - _mark, right) * (L.shimmy >= 0.0f ? 1.0f : -1.0f);
            if (progress >= std::abs(L.shimmy) - 0.05f) {
                _sub = Sub::Climb2;
                _sub_steps = 0;
            } else {
                c.move.x = L.shimmy >= 0.0f ? 1.0f : -1.0f;
            }
            if (ts.held == 0 && cs.ground == CharacterGround3D::OnGround) {
                c.grab_left = c.grab_right = false;
                ++_restarts;
                _sub = Sub::Step;
                _sub_steps = 0;
            }
            break;
        }
        if (_sub == Sub::Climb2) {
            c.move.y = 1.0f;
            if (ts.mode == TraversalMode3D::Mantling) {
                _sub = Sub::Mantle;
                _sub_steps = 0;
            } else if (ts.held == 0 && cs.ground == CharacterGround3D::OnGround && _sub_steps > 30) {
                c.grab_left = c.grab_right = false;
                ++_restarts;
                _sub = Sub::Step;
                _sub_steps = 0;
            }
            break;
        }
        if (_sub == Sub::Mantle) {
            // Keep the hands asked for until the pull-up is over, then walk in.
            if (ts.mode == TraversalMode3D::Walking) {
                c.grab_left = c.grab_right = false;
                c.move.y = 1.0f;
                aim(c, yaw_to(L.to - feet), 0.0f);
                if (flat_distance(feet, L.to) < 1.0f && cs.ground == CharacterGround3D::OnGround) advance_leg();
            } else {
                c.move.y = 1.0f;
            }
            break;
        }
        break;
    }
    case LegKind::Rope: {
        const RopeSpec& rope = _game->level().ropes[std::size_t(std::max(L.rope, 0))];
        const Vec3f wall_dir = -L.normal;
        const f32 wall_yaw = yaw_to(wall_dir);
        const f32 top_y = L.to.y;
        if (_sub == Sub::Start) {
            _sub = Sub::Rest;
            _sub_steps = 0;
        }
        if (_sub == Sub::Rest) {
            if (stamina >= cap * _config.rest_to || _sub_steps > 1500) {
                _sub = Sub::Step;
                _sub_steps = 0;
            }
            break;
        }
        if (_sub == Sub::Step) {
            if (flat_distance(feet, L.from) < 0.4f) {
                _sub = Sub::Face;
                _sub_steps = 0;
            } else {
                to_point(L.from);
            }
            break;
        }
        if (_sub == Sub::Face) {
            aim(c, wall_yaw, 5.0f);
            if (std::abs(yaw_error(wall_yaw)) < 2.0f) {
                _sub = Sub::Approach;
                _sub_steps = 0;
            }
            break;
        }
        if (_sub == Sub::Approach) {
            c.grab_left = c.grab_right = true;
            aim(c, wall_yaw, 5.0f);
            c.move.y = ts.held >= 1 ? 0.0f : 0.5f;
            if (ts.held >= 2 || (ts.held >= 1 && _sub_steps > 60)) {
                _sub = Sub::Rope;
                _sub_steps = 0;
            } else if (_sub_steps > 240) {
                c.grab_left = c.grab_right = false;
                ++_restarts;
                _sub = Sub::Step;
                _sub_steps = 0;
            }
            break;
        }
        if (_sub == Sub::Rope) {
            c.grab_left = c.grab_right = true;
            aim(c, wall_yaw, 5.0f);
            c.move.y = 1.0f;
            // The hands near the lip's height: hand over.
            // The shoulders about at the lip's height (the feet 1.75 m under it): a hand can reach the lip from there.
            if (feet.y >= top_y - 1.75f) {
                _sub = Sub::Handover;
                _sub_steps = 0;
                _hand = 0;
                _hand_steps = 0;
                _retry = 0;
            }
            if (ts.held == 0 && cs.ground == CharacterGround3D::OnGround && _sub_steps > 30) {
                ++_restarts;
                _sub = Sub::Step;
                _sub_steps = 0;
            }
            (void)rope;
            break;
        }
        if (_sub == Sub::Handover) {
            // The lip is a little to the left of the rope (and a hand at a time lets go of the rope and takes it): aimed that
            // way, the probe finds only the lip. `_hand` counts the phase: 0 the left lets go, 1 takes the lip, 2 the right
            // lets go, 3 takes it, 4 both are on it.
            // A grip on the lip's face (not its top) cannot be pulled up from: another try, aimed higher or lower.
            static const f32 pitches[3] = {5.0f, 28.0f, -12.0f};
            static const f32 yaws[3] = {32.0f, 48.0f, 18.0f};
            aim(c, wall_yaw + yaws[(_retry / 3) % 3], pitches[_retry % 3]);
            const GripState3D& gl = ts.grips[0];
            const GripState3D& gr = ts.grips[1];
            const auto on_lip = [](const GripState3D& g) { return g.phase == GripPhase3D::Held && g.kind != GripKind3D::Rope; };
            c.grab_left = c.grab_right = true;
            switch (_hand) {
            case 0:
                c.grab_left = false;
                if (gl.phase == GripPhase3D::Free) _hand = 1;
                break;
            case 1:
                if (on_lip(gl)) _hand = 2;
                else if (gl.phase == GripPhase3D::Held) _hand = 0; // took the rope again: let go and aim again
                else if (gl.phase == GripPhase3D::Free && ++_hand_steps > 40) {
                    ++_retry; // nothing in reach that way: another aim
                    _hand_steps = 0;
                }
                break;
            case 2:
                c.grab_right = false;
                if (gr.phase == GripPhase3D::Free) _hand = 3;
                break;
            case 3:
                if (on_lip(gr)) _hand = 4;
                else if (gr.phase == GripPhase3D::Held) _hand = 2;
                else if (gr.phase == GripPhase3D::Free && ++_hand_steps > 40) {
                    ++_retry;
                    _hand_steps = 0;
                }
                break;
            default:
                c.move.y = 1.0f;
                if (++_hand_steps > 50 && !(gl.ledge && gr.ledge) && ts.mode == TraversalMode3D::Hanging) {
                    ++_retry;
                    _hand = 0;
                    _hand_steps = 0;
                }
                break;
            }
            if (ts.mode == TraversalMode3D::Mantling) {
                _sub = Sub::Mantle;
                _sub_steps = 0;
            }
            if (ts.held == 0 && cs.ground == CharacterGround3D::OnGround && _sub_steps > 30) {
                ++_restarts;
                _sub = Sub::Step;
                _sub_steps = 0;
            }
            break;
        }
        if (_sub == Sub::Mantle) {
            c.grab_left = c.grab_right = ts.mode != TraversalMode3D::Walking;
            if (ts.mode == TraversalMode3D::Walking) {
                c.move.y = 1.0f;
                aim(c, yaw_to(L.to - feet), 0.0f);
                if (flat_distance(feet, L.to) < 1.0f && cs.ground == CharacterGround3D::OnGround) advance_leg();
            } else {
                c.move.y = 1.0f;
            }
        }
        break;
    }
    }
    if (_leg_steps > _config.leg_timeout) {
        // Stuck for good: skip to the next walk leg that is not above (a test's bound will say the run failed).
        ++_restarts;
        _leg_steps = 0;
    }
    return c;
}

RunReport run_bot(Game& game, Bot& bot, u64 max_steps, bool stop_at_summit) {
    RunReport r;
    const i32 n = i32(game.players().size());
    std::vector<PlayerCommand> commands(static_cast<std::size_t>(n));
    for (u64 s = 0; s < max_steps; ++s) {
        commands[0] = bot.next();
        r.log.push_back(commands[0]);
        game.step(commands);
        const Player& p = game.player(0);
        const Vec3f f = game.feet(0);
        if (!std::isfinite(f.x) || !std::isfinite(f.y) || !std::isfinite(f.z)) r.non_finite = true;
        r.min_stamina = std::min(r.min_stamina, game.stamina(0));
        r.highest = std::max(r.highest, f.y - game.level().start.y);
        if (p.summit && stop_at_summit) {
            ++r.steps;
            break;
        }
        ++r.steps;
    }
    const Player& p = game.player(0);
    r.summit = p.summit;
    r.summit_step = p.summit_step;
    r.falls = p.falls;
    r.stuns = p.stuns;
    r.deaths = p.deaths;
    r.final_hash = game.state_hash();
    r.legs_done = bot.leg();
    return r;
}

u64 replay(Game& game, const std::vector<PlayerCommand>& log) {
    const std::size_t n = game.players().size();
    std::vector<PlayerCommand> commands(n);
    for (const PlayerCommand& c : log) {
        commands[0] = c;
        game.step(commands);
    }
    return game.state_hash();
}

} // namespace summit

namespace summit {

bool script_config(const std::string& name, const SummitLevel& level, BotConfig& out) {
    out = {};
    if (name == "climb") return true;
    if (name == "rope") {
        for (std::size_t i = 0; i < level.route.size(); ++i) {
            if (level.route[i].kind == LegKind::Rope) {
                out.stop_after_leg = i32(i);
                return true;
            }
        }
        return false;
    }
    if (name == "fall") {
        // The first climb of 6.9 m or more (the chimney): it lets go near the top, a drop of about 5 m (9.7 m/s: a knock-out).
        for (std::size_t i = 0; i < level.route.size(); ++i) {
            if (level.route[i].kind == LegKind::Climb && level.route[i].height >= 6.9f) {
                out.drop_leg = i32(i);
                out.drop_height = level.route[i].height - 2.4f;
                return true;
            }
        }
        return false;
    }
    return false;
}

bool write_command_log(const std::string& path, u64 seed, const std::vector<PlayerCommand>& log) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) return false;
    const u32 magic = 0x474C4D53u; // "SMLG"
    const u64 count = log.size();
    bool ok = std::fwrite(&magic, sizeof magic, 1, f) == 1 && std::fwrite(&seed, sizeof seed, 1, f) == 1 &&
              std::fwrite(&count, sizeof count, 1, f) == 1;
    for (const PlayerCommand& c : log) {
        const f32 v[4] = {c.move.x, c.move.y, c.look.x, c.look.y};
        const u8 flags = u8((c.jump ? 1 : 0) | (c.sprint ? 2 : 0) | (c.grab_left ? 4 : 0) | (c.grab_right ? 8 : 0) | (c.release ? 16 : 0) |
                            (c.pull_up ? 32 : 0) | (c.interact ? 64 : 0));
        ok = ok && std::fwrite(v, sizeof v, 1, f) == 1 && std::fwrite(&flags, 1, 1, f) == 1;
    }
    return std::fclose(f) == 0 && ok;
}

bool read_command_log(const std::string& path, u64& seed, std::vector<PlayerCommand>& log) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) return false;
    u32 magic = 0;
    u64 count = 0;
    bool ok = std::fread(&magic, sizeof magic, 1, f) == 1 && magic == 0x474C4D53u && std::fread(&seed, sizeof seed, 1, f) == 1 &&
              std::fread(&count, sizeof count, 1, f) == 1 && count < (u64{1} << 28);
    std::vector<PlayerCommand> out;
    for (u64 i = 0; ok && i < count; ++i) {
        f32 v[4];
        u8 flags = 0;
        ok = std::fread(v, sizeof v, 1, f) == 1 && std::fread(&flags, 1, 1, f) == 1;
        PlayerCommand c;
        c.move = {v[0], v[1]};
        c.look = {v[2], v[3]};
        c.jump = flags & 1;
        c.sprint = flags & 2;
        c.grab_left = flags & 4;
        c.grab_right = flags & 8;
        c.release = flags & 16;
        c.pull_up = flags & 32;
        c.interact = flags & 64;
        out.push_back(c);
    }
    std::fclose(f);
    if (ok) log = std::move(out);
    return ok;
}


} // namespace summit
