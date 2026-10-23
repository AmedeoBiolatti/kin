#include "hud.hpp"

#include "environment.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <kin/ui2/layout.hpp>

namespace summit {

namespace {

f32 lerp(f32 a, f32 b, f32 t) {
    return a + (b - a) * t;
}

const Color tone_good = Color::rgb(112, 200, 120);
const Color tone_low = Color::rgb(236, 176, 56);
const Color tone_spent = Color::rgb(224, 66, 56);

} // namespace

const char* status_text(StatusKind s) {
    switch (s) {
    case StatusKind::Standing:
        return "Standing";
    case StatusKind::Walking:
        return "Walking";
    case StatusKind::Sprinting:
        return "Sprinting";
    case StatusKind::InAir:
        return "In the air";
    case StatusKind::Hanging:
        return "Hanging";
    case StatusKind::PullingUp:
        return "Pulling up";
    case StatusKind::KnockedOut:
        return "Knocked out";
    case StatusKind::GettingUp:
        return "Getting up";
    case StatusKind::Dead:
        return "Dead: back to camp";
    }
    return "";
}

StaminaTone stamina_tone(f32 fraction, bool exhausted, const HudRules& rules) {
    if (exhausted || fraction <= 0.0f)
        return StaminaTone::Spent;
    if (fraction < rules.low_stamina)
        return StaminaTone::Low;
    return StaminaTone::Good;
}

Color stamina_colour(f32 fraction, bool exhausted, const HudRules& rules) {
    if (exhausted || fraction <= 0.0f)
        return tone_spent;
    // A short blend between the tones either side of the thresholds, so that it does not flicker across one.
    if (fraction < rules.critical_stamina)
        return tone_spent;
    if (fraction < rules.low_stamina) {
        const f32 t = (fraction - rules.critical_stamina) / std::max(1e-4f, rules.low_stamina - rules.critical_stamina);
        return mix(tone_spent, tone_low, t);
    }
    const f32 t = std::clamp((fraction - rules.low_stamina) / 0.15f, 0.0f, 1.0f);
    return mix(tone_low, tone_good, t);
}

FallWarning fall_warning_for(f32 speed_down, const PlayerRules& rules, const HudRules& hud) {
    if (speed_down >= rules.stun_speed)
        return FallWarning::KnockOut;
    if (speed_down > rules.safe_landing)
        return FallWarning::Hurt;
    if (speed_down >= rules.safe_landing * hud.brace_fraction)
        return FallWarning::Brace;
    return FallWarning::None;
}

std::string clock_text(f32 seconds) {
    const int s = std::max(0, int(seconds));
    char b[32];
    std::snprintf(b, sizeof b, "%d:%02d", s / 60, s % 60);
    return b;
}

void HudNotices::observe(const Game& game, i32 player, const HudRules& rules) {
    camp_seconds = std::max(0.0f, camp_seconds - fixed_step);
    hurt_seconds = std::max(0.0f, hurt_seconds - fixed_step);
    if (hurt_seconds <= 0.0f)
        hurt = 0.0f;
    for (const GameEvent& e : game.events()) {
        if (e.player != player)
            continue;
        if (e.kind == GameEventKind::Camp) {
            camp = i32(e.value);
            camp_seconds = rules.notice_seconds;
        } else if (e.kind == GameEventKind::Hurt) {
            hurt = std::min(100.0f, hurt + e.value);
            hurt_seconds = rules.hurt_seconds;
        } else if (e.kind == GameEventKind::Summit) {
            summit = true;
            summit_step = i32(e.step);
        } else if (e.kind == GameEventKind::Respawned) {
            camp = i32(e.value);
            camp_seconds = rules.notice_seconds;
        }
    }
}

HudModel build_hud_model(const Game& game, i32 player, const HudNotices& notices, const HudRules& rules) {
    HudModel m;
    const Player& p = game.player(player);
    const PlayerRules& pr = game.rules();
    const TraversalState3D ts = game.traversal(player);
    const CharacterState3D cs = game.character_state(player);

    m.stamina_max = std::max(1.0f, pr.max_stamina);
    m.stamina_cap = std::clamp(ts.resource.max, 0.0f, m.stamina_max);
    m.stamina = std::clamp(ts.resource.value, 0.0f, m.stamina_cap);
    m.stamina_fraction = m.stamina / m.stamina_max;
    m.cap_fraction = m.stamina_cap / m.stamina_max;
    m.exhausted = ts.resource.exhausted;
    // The tone is about what can be used: the share of the cap that is left.
    const f32 usable = m.stamina_cap > 0.0f ? m.stamina / m.stamina_cap : 0.0f;
    m.tone = stamina_tone(usable, m.exhausted, rules);
    m.stamina_colour = stamina_colour(usable, m.exhausted, rules);
    m.stamina_pulse = m.exhausted || usable < rules.critical_stamina;

    const auto hand = [&](std::size_t i) {
        switch (ts.grips[i].phase) {
        case GripPhase3D::Reaching:
            return HandState::Reaching;
        case GripPhase3D::Held:
            return HandState::Held;
        case GripPhase3D::Slipping:
            return HandState::Slipping;
        default:
            return HandState::Free;
        }
    };
    m.left = hand(0);
    m.right = hand(1);
    m.hands_held = i32(ts.held);
    m.hanging = ts.mode == TraversalMode3D::Hanging;
    m.pulling_up = ts.mode == TraversalMode3D::Mantling;

    m.health_max = pr.max_health;
    m.health = std::clamp(p.health, 0.0f, m.health_max);
    m.health_fraction = m.health / std::max(1.0f, m.health_max);
    m.injury = m.health_fraction >= 0.9f   ? ""
               : m.health_fraction >= 0.5f ? "Hurt"
               : m.health_fraction >= 0.2f ? "Badly hurt"
                                           : "Critical";
    m.health_colour = m.health_fraction >= 0.9f
                          ? tone_good
                          : mix(tone_spent, tone_low, std::clamp((m.health_fraction - 0.15f) / 0.6f, 0.0f, 1.0f));
    m.hurt_flash = rules.hurt_seconds > 0.0f ? std::clamp(notices.hurt_seconds / rules.hurt_seconds, 0.0f, 1.0f) : 0.0f;

    m.altitude = game.altitude(player);
    m.best_altitude = std::max(p.best_altitude, m.altitude);
    m.summit_altitude = std::max(1.0f, game.level().height());
    m.progress = std::clamp(m.altitude / m.summit_altitude, 0.0f, 1.0f);
    m.camp = p.camp;

    if (p.state == PlayerState::Alive && ts.mode == TraversalMode3D::Walking && cs.ground == CharacterGround3D::InAir) {
        m.fall_speed = std::max(0.0f, -cs.vertical_speed);
        m.fall_warning = fall_warning_for(m.fall_speed, pr);
    }

    switch (p.state) {
    case PlayerState::Stunned:
        m.status = StatusKind::KnockedOut;
        break;
    case PlayerState::GettingUp:
        m.status = StatusKind::GettingUp;
        break;
    case PlayerState::Dead:
        m.status = StatusKind::Dead;
        break;
    case PlayerState::Alive:
        if (ts.mode == TraversalMode3D::Mantling)
            m.status = StatusKind::PullingUp;
        else if (ts.mode == TraversalMode3D::Hanging)
            m.status = StatusKind::Hanging;
        else if (cs.ground == CharacterGround3D::InAir)
            m.status = StatusKind::InAir;
        else if (p.command.sprint && cs.ground_speed > 5.0f)
            m.status = StatusKind::Sprinting;
        else if (cs.ground_speed > 0.3f)
            m.status = StatusKind::Walking;
        else
            m.status = StatusKind::Standing;
        break;
    }

    if (notices.camp >= 0 && notices.camp_seconds > 0.0f) {
        char b[48];
        if (notices.camp == 0)
            std::snprintf(b, sizeof b, "Base camp");
        else
            std::snprintf(b, sizeof b, "Camp %d reached", notices.camp);
        m.camp_notice = b;
        m.camp_notice_alpha = std::clamp(notices.camp_seconds, 0.0f, 1.0f);
    }
    for (const Camp& c : game.level().camps) {
        const Vec3f d = game.feet(player) - c.spawn;
        if (std::sqrt(d.x * d.x + d.z * d.z) < c.radius && std::fabs(d.y) < 3.0f)
            m.at_camp = true;
    }

    m.falls = p.falls;
    m.summit = p.summit;
    m.run_seconds = p.summit && p.summit_step >= 0 ? f32(f64(p.summit_step) * f64(fixed_step)) : f32(game.time());
    if (p.summit) {
        char b[96];
        std::snprintf(b, sizeof b, "Summit! %s, %d %s", clock_text(m.run_seconds).c_str(), p.falls,
                      p.falls == 1 ? "fall" : "falls");
        m.summit_message = b;
    }
    if (game.config().expedition) {
        m.expedition = true;
        m.phase = p.expedition.phase;
        m.sheltered = in_expedition_shelter(game.level(), game.feet(player));
        m.shelter_visited = p.expedition.shelter_visited;
        const ExpeditionLayout& e = game.level().expedition;
        Vec3f target = e.beacon;
        if (m.phase == ExpeditionPhase::Briefing) {
            m.objective = "SUMMIT / THE BEACON";
            m.objective_detail = "Restore the summit signal and call for extraction.";
        } else if (m.phase == ExpeditionPhase::Complete) {
            m.objective = "EXPEDITION COMPLETE";
            m.objective_detail = "Beacon online. Extraction confirmed.";
        } else if (m.phase == ExpeditionPhase::Extraction) {
            m.objective = "Beacon online / request extraction";
            target = e.extraction;
        } else if (p.camp == 0) {
            m.objective = "Reach ridge camp / choose your route";
            target = m.altitude < 11.0f ? e.fork : game.level().camps[1].spawn;
        } else if (p.camp == 1) {
            m.objective = "Reach snow camp / climb the final wall";
            target = game.level().camps[2].spawn;
        } else {
            m.objective = "Restore the summit beacon";
        }
        if (m.phase != ExpeditionPhase::Briefing && m.phase != ExpeditionPhase::Complete) {
            const Vec3f delta = target - game.feet(player);
            const f32 distance = length(delta);
            Vec3f flat{delta.x, 0, delta.z};
            const Vec3f forward = look_vector(p.yaw, 0);
            const Vec3f right{-forward.z, 0, forward.x};
            if (length(flat) > 0.01f)
                flat = normalize(flat);
            const char* direction = dot(flat, forward) < -0.5f ? "Behind"
                                    : dot(flat, right) > 0.3f  ? "Right"
                                    : dot(flat, right) < -0.3f ? "Left"
                                                               : "Ahead";
            char b[96];
            std::snprintf(b, sizeof b, "%s / %.0f m   %s", direction, double(distance),
                          m.sheltered   ? "Sheltered: stop to recover"
                          : p.camp == 0 ? "Orange: climb   Blue: cave"
                                        : "Follow the gold trail markers");
            m.objective_detail = b;
        }
        switch (game.interaction(player)) {
        case ExpeditionInteraction::Begin:
            m.interaction = "Begin expedition";
            break;
        case ExpeditionInteraction::Beacon:
            m.interaction = "Hold to restore the beacon";
            m.interaction_progress = f32(p.expedition.beacon_steps) / beacon_hold_steps;
            break;
        case ExpeditionInteraction::Extract:
            m.interaction = "Hold to request extraction";
            m.interaction_progress = f32(p.expedition.extraction_steps) / extraction_hold_steps;
            break;
        case ExpeditionInteraction::Recover:
            m.interaction = "Cave shelter / stand still to recover health and stamina";
            break;
        default:
            break;
        }
        const i32 end = m.phase == ExpeditionPhase::Complete ? p.expedition.finish_step : i32(game.step_count());
        m.run_seconds = p.expedition.start_step < 0 ? 0 : f32(std::max(0, end - p.expedition.start_step)) * fixed_step;
    }
    return m;
}

// ---- layout
// ----------------------------------------------------------------------------------------------------------------

namespace {

// A node of the layout arena: a fixed size, optionally an overlay anchored in its parent.
struct Builder {
    std::vector<ui2::LayoutNode> nodes;
    i32 add(i32 parent, Vec2f size, ui2::UiAnchor anchor, Vec2f offset, bool overlay) {
        ui2::LayoutNode n;
        n.style.width = ui2::fixed(size.x);
        n.style.height = ui2::fixed(size.y);
        n.style.overlay = overlay;
        n.style.anchor = anchor;
        n.style.anchor_offset = offset;
        n.parent = parent;
        nodes.push_back(n);
        const i32 index = i32(nodes.size()) - 1;
        if (parent >= 0)
            nodes[std::size_t(parent)].children.push_back(index);
        return index;
    }
};

} // namespace

HudLayout hud_layout(Vec2f screen, f32 scale) {
    HudLayout l;
    l.screen = screen;
    // The design is 1280x720: the whole layout is scaled to fit the smaller of the two ratios, so 4:3 and tiny sizes
    // keep the same arrangement (nothing is re-flowed: it is the same picture smaller).
    l.u = std::max(0.05f, std::min(screen.x / 1280.0f, screen.y / 720.0f) * scale);
    const f32 u = l.u;
    const f32 margin = 16.0f * u;
    Builder b;
    ui2::LayoutNode root;
    root.style.width = ui2::fixed(screen.x);
    root.style.height = ui2::fixed(screen.y);
    root.style.padding = ui2::padding(margin);
    b.nodes.push_back(root);
    using A = ui2::UiAnchor;
    const i32 altitude = b.add(0, {250 * u, 74 * u}, A::TopLeft, {}, true);
    const i32 health = b.add(0, {210 * u, 56 * u}, A::TopRight, {}, true);
    const i32 notice = b.add(0, {400 * u, 40 * u}, A::TopCenter, {}, true);
    const i32 debug = b.add(0, {520 * u, 24 * u}, A::TopCenter, {0, 48 * u}, true);
    const i32 summit = b.add(0, {520 * u, 120 * u}, A::TopCenter, {0, 110 * u}, true);
    const i32 warning = b.add(0, {300 * u, 46 * u}, A::Center, {0, 60 * u}, true);
    const i32 hint = b.add(0, {384 * u, 24 * u}, A::BottomLeft, {}, true);
    // The bottom centre: the hands above the stamina bar, one column.
    const i32 column = b.add(0, {420 * u, 74 * u}, A::BottomCenter, {}, true);
    b.nodes[std::size_t(column)].style.axis = ui2::UiLayoutAxis::Vertical;
    b.nodes[std::size_t(column)].style.spacing = 8 * u;
    b.nodes[std::size_t(column)].style.cross = ui2::UiAlign::Center;
    const i32 hands = b.add(column, {92 * u, 40 * u}, A::TopLeft, {}, false);
    b.nodes[std::size_t(hands)].style.axis = ui2::UiLayoutAxis::Horizontal;
    b.nodes[std::size_t(hands)].style.spacing = 12 * u;
    const i32 hl = b.add(hands, {40 * u, 40 * u}, A::TopLeft, {}, false);
    const i32 hr = b.add(hands, {40 * u, 40 * u}, A::TopLeft, {}, false);
    const i32 stamina = b.add(column, {420 * u, 26 * u}, A::TopLeft, {}, false);
    ui2::solve(b.nodes, 0, {0.0f, 0.0f, screen.x, screen.y});
    const auto at = [&](i32 i) { return b.nodes[std::size_t(i)].solved; };
    l.altitude = at(altitude);
    l.health = at(health);
    l.notice = at(notice);
    l.debug = at(debug);
    l.summit = at(summit);
    l.warning = at(warning);
    l.hint = at(hint);
    l.hands = at(hands);
    l.hand_left = at(hl);
    l.hand_right = at(hr);
    l.stamina = at(stamina);
    l.objective = {(screen.x - 520 * u) * 0.5f, 88 * u, 520 * u, 62 * u};
    l.interaction = {(screen.x - 560 * u) * 0.5f, screen.y - 160 * u, 560 * u, 52 * u};
    l.expedition_panel = {(screen.x - 760 * u) * 0.5f, (screen.y - 350 * u) * 0.5f, 760 * u, 350 * u};
    return l;
}

std::string hud_hint_line(const InputBindings& bindings) {
    const auto first = [&](Action a) {
        const std::vector<Binding>& list = bindings.bindings(a);
        return list.empty() ? std::string("-") : binding_label(list.front());
    };
    return "Esc menu    " + first(Action::GrabLeft) + " / " + first(Action::GrabRight) + " hands    " +
           first(Action::Release) + " let go";
}

// ---- menu
// ------------------------------------------------------------------------------------------------------------------

namespace {

bool nav_up(const RawInput& r) {
    return r.key(Key::Up) || r.key(Key::W) || r.pad.button(GamepadButton::DPadUp) ||
           (r.pad.connected && r.pad.left_y > 0.6f);
}
bool nav_down(const RawInput& r) {
    return r.key(Key::Down) || r.key(Key::S) || r.pad.button(GamepadButton::DPadDown) ||
           (r.pad.connected && r.pad.left_y < -0.6f);
}
bool nav_left(const RawInput& r) {
    return r.key(Key::Left) || r.key(Key::A) || r.pad.button(GamepadButton::DPadLeft) ||
           (r.pad.connected && r.pad.left_x < -0.6f);
}
bool nav_right(const RawInput& r) {
    return r.key(Key::Right) || r.key(Key::D) || r.pad.button(GamepadButton::DPadRight) ||
           (r.pad.connected && r.pad.left_x > 0.6f);
}
bool nav_accept(const RawInput& r) {
    return r.key(Key::Enter) || r.key(Key::KeypadEnter) || r.key(Key::Space) || r.pad.button(GamepadButton::A);
}
bool nav_back(const RawInput& r) {
    return r.key(Key::Backspace) || r.pad.button(GamepadButton::B);
}
bool nav_toggle(const RawInput& r) {
    return r.key(Key::Escape) || r.pad.button(GamepadButton::Start);
}

} // namespace

MenuEdges menu_edges(const RawInput& previous, const RawInput& now) {
    MenuEdges e;
    e.input.up = nav_up(now) && !nav_up(previous);
    e.input.down = nav_down(now) && !nav_down(previous);
    e.input.left = nav_left(now) && !nav_left(previous);
    e.input.right = nav_right(now) && !nav_right(previous);
    e.input.accept = nav_accept(now) && !nav_accept(previous);
    e.input.back = nav_back(now) && !nav_back(previous);
    e.toggle = nav_toggle(now) && !nav_toggle(previous);
    return e;
}

namespace {

constexpr f32 volume_step = 0.1f;
constexpr f32 sens_min = 0.02f, sens_max = 0.30f;

std::string percent(f32 v) {
    char b[16];
    std::snprintf(b, sizeof b, "%d%%", int(std::lround(v * 100.0f)));
    return b;
}

} // namespace

MenuAction Menu::toggle() {
    switch (_screen) {
    case MenuScreen::Closed:
        _screen = MenuScreen::Main;
        _focus = 0;
        return MenuAction::None;
    case MenuScreen::Controls:
        _screen = MenuScreen::Main;
        return MenuAction::None;
    case MenuScreen::Main:
        _screen = MenuScreen::Closed;
        return MenuAction::Resume;
    }
    return MenuAction::None;
}

std::string Menu::label(MenuRow row) const {
    switch (row) {
    case MenuRow::Resume:
        return "Resume";
    case MenuRow::Restart:
        return _expedition_complete ? "New expedition" : "Restart from camp";
    case MenuRow::Look:
        return "Look";
    case MenuRow::Camera:
        return "Camera";
    case MenuRow::MasterVolume:
        return "Master volume";
    case MenuRow::SfxVolume:
        return "Effects volume";
    case MenuRow::AmbienceVolume:
        return "Ambience volume";
    case MenuRow::Sensitivity:
        return "Mouse sensitivity";
    case MenuRow::InvertLook:
        return "Invert look";
    case MenuRow::TimeOfDay:
        return "Time of day";
    case MenuRow::Controls:
        return "Controls";
    case MenuRow::Quit:
        return "Quit";
    case MenuRow::Count:
        break;
    }
    return "";
}

std::string Menu::value(MenuRow row) const {
    static const SummitSettings fallback;
    const SummitSettings& s = _settings != nullptr ? *_settings : fallback;
    switch (row) {
    case MenuRow::Look:
        return s.look;
    case MenuRow::Camera:
        return s.first_person ? "First person" : "Third person";
    case MenuRow::MasterVolume:
        return percent(s.master_volume);
    case MenuRow::SfxVolume:
        return percent(s.sfx_volume);
    case MenuRow::AmbienceVolume:
        return percent(s.ambience_volume);
    case MenuRow::Sensitivity: {
        char b[16];
        std::snprintf(b, sizeof b, "%.2f", double(s.bindings.mouse_sensitivity));
        return b;
    }
    case MenuRow::InvertLook:
        return s.bindings.invert_y ? "On" : "Off";
    case MenuRow::TimeOfDay: {
        if (s.time_speed <= 0.0f || !s.effects.time_of_day)
            return "Frozen";
        char b[24];
        std::snprintf(b, sizeof b, "%.2f h/min", double(s.time_speed));
        return b;
    }
    default:
        return "";
    }
}

f32 Menu::slider(MenuRow row) const {
    static const SummitSettings fallback;
    const SummitSettings& s = _settings != nullptr ? *_settings : fallback;
    switch (row) {
    case MenuRow::MasterVolume:
        return s.master_volume;
    case MenuRow::SfxVolume:
        return s.sfx_volume;
    case MenuRow::AmbienceVolume:
        return s.ambience_volume;
    case MenuRow::Sensitivity:
        return std::clamp((s.bindings.mouse_sensitivity - sens_min) / (sens_max - sens_min), 0.0f, 1.0f);
    default:
        return -1.0f;
    }
}

bool Menu::set_slider(MenuRow row, f32 fraction) {
    if (_settings == nullptr)
        return false;
    const f32 f = std::clamp(fraction, 0.0f, 1.0f);
    switch (row) {
    case MenuRow::MasterVolume:
        _settings->master_volume = f;
        return true;
    case MenuRow::SfxVolume:
        _settings->sfx_volume = f;
        return true;
    case MenuRow::AmbienceVolume:
        _settings->ambience_volume = f;
        return true;
    case MenuRow::Sensitivity:
        _settings->bindings.mouse_sensitivity = lerp(sens_min, sens_max, f);
        return true;
    default:
        return false;
    }
}

MenuAction Menu::navigate(const MenuInput& in) {
    if (_screen == MenuScreen::Closed)
        return MenuAction::None;
    if (_screen == MenuScreen::Controls) {
        if (in.back || in.accept)
            _screen = MenuScreen::Main;
        return MenuAction::None;
    }
    if (_settings == nullptr)
        return MenuAction::None;
    SummitSettings& s = *_settings;
    const i32 n = i32(menu_row_count);
    if (in.up)
        _focus = (_focus + n - 1) % n;
    if (in.down)
        _focus = (_focus + 1) % n;
    if (in.back) {
        _screen = MenuScreen::Closed;
        return MenuAction::Resume;
    }
    const MenuRow row = MenuRow(_focus);
    const i32 dir = (in.right ? 1 : 0) - (in.left ? 1 : 0);
    const bool act = in.accept;
    if (!act && dir == 0)
        return MenuAction::None;
    const auto step_slider = [&](f32& v, f32 step) {
        v = std::clamp(std::round((v + f32(dir == 0 ? 1 : dir) * step) / step) * step, 0.0f, 1.0f);
        return MenuAction::Changed;
    };
    switch (row) {
    case MenuRow::Resume:
        if (act) {
            _screen = MenuScreen::Closed;
            return MenuAction::Resume;
        }
        break;
    case MenuRow::Restart:
        if (act) {
            _screen = MenuScreen::Closed;
            return MenuAction::RestartFromCamp;
        }
        break;
    case MenuRow::Look: {
        const std::vector<Look>& all = looks();
        std::size_t i = 0;
        for (; i < all.size(); ++i) {
            if (s.look == all[i].name)
                break;
        }
        const i32 d = dir == 0 ? 1 : dir;
        const i32 count = i32(all.size());
        s.look = all[std::size_t((i32(i) + d + count) % count)].name;
        return MenuAction::Changed;
    }
    case MenuRow::Camera:
        s.first_person = !s.first_person;
        return MenuAction::Changed;
    case MenuRow::MasterVolume:
        return step_slider(s.master_volume, volume_step);
    case MenuRow::SfxVolume:
        return step_slider(s.sfx_volume, volume_step);
    case MenuRow::AmbienceVolume:
        return step_slider(s.ambience_volume, volume_step);
    case MenuRow::Sensitivity: {
        const f32 d = f32(dir == 0 ? 1 : dir) * 0.02f;
        s.bindings.mouse_sensitivity = std::clamp(s.bindings.mouse_sensitivity + d, sens_min, sens_max);
        return MenuAction::Changed;
    }
    case MenuRow::InvertLook:
        s.bindings.invert_y = !s.bindings.invert_y;
        return MenuAction::Changed;
    case MenuRow::TimeOfDay: {
        if (dir == 0) {
            s.time_speed = s.time_speed > 0.0f ? 0.0f : 0.25f;
        } else {
            s.time_speed = std::clamp(s.time_speed + f32(dir) * 0.25f, 0.0f, 3.0f);
        }
        s.effects.time_of_day = true;
        return MenuAction::Changed;
    }
    case MenuRow::Controls:
        if (act) {
            _screen = MenuScreen::Controls;
            return MenuAction::None;
        }
        break;
    case MenuRow::Quit:
        if (act)
            return MenuAction::Quit;
        break;
    case MenuRow::Count:
        break;
    }
    return MenuAction::None;
}

bool restart_from_camp(Game& game, i32 player) {
    const Player& p = game.player(player);
    if (p.state != PlayerState::Alive || game.level().camps.empty())
        return false;
    const Camp& c = game.level().camps[std::size_t(std::clamp(p.camp, 0, i32(game.level().camps.size()) - 1))];
    game.debug_teleport(player, c.spawn);
    game.debug_set_health(player, game.rules().max_health);
    return true;
}

std::string binding_label(const Binding& b) {
    static const char* const pad_names[] = {"Pad A",
                                            "Pad B",
                                            "Pad X",
                                            "Pad Y",
                                            "Pad Back",
                                            "Pad Start",
                                            "Left stick click",
                                            "Right stick click",
                                            "Left bumper",
                                            "Right bumper",
                                            "Pad up",
                                            "Pad down",
                                            "Pad left",
                                            "Pad right"};
    switch (b.device) {
    case Binding::Device::Key: {
        const std::string name(key_name(static_cast<Key>(b.code)));
        // "LeftShift" reads better as "Left Shift".
        std::string out;
        for (std::size_t i = 0; i < name.size(); ++i) {
            if (i > 0 && std::isupper(static_cast<unsigned char>(name[i])) &&
                std::islower(static_cast<unsigned char>(name[i - 1])))
                out += ' ';
            out += name[i];
        }
        return out;
    }
    case Binding::Device::Mouse:
        switch (static_cast<MouseButton>(b.code)) {
        case MouseButton::Left:
            return "Left mouse";
        case MouseButton::Right:
            return "Right mouse";
        case MouseButton::Middle:
            return "Middle mouse";
        default:
            return "Mouse button";
        }
    case Binding::Device::PadButton:
        return pad_names[std::clamp(b.code, 0, 13)];
    case Binding::Device::PadTrigger:
        return b.code == 0 ? "Left trigger" : "Right trigger";
    }
    return "";
}

std::vector<ControlLine> controls_screen(const InputBindings& bindings) {
    static const char* const names[action_count] = {
        "Move forward",     "Move back",         "Move left", "Move right", "Jump",        "Sprint",
        "Left hand (hold)", "Right hand (hold)", "Let go",    "Pull up",    "Rest at camp"};
    std::vector<ControlLine> lines;
    for (std::size_t a = 0; a < action_count; ++a) {
        ControlLine l;
        l.action = names[a];
        const std::vector<Binding>& list = bindings.bindings(Action(a));
        for (std::size_t i = 0; i < list.size(); ++i) {
            if (i > 0)
                l.keys += ", ";
            l.keys += binding_label(list[i]);
        }
        if (l.keys.empty())
            l.keys = "(not bound)";
        lines.push_back(std::move(l));
    }
    return lines;
}

MenuLayout menu_layout(Vec2f screen, i32 rows) {
    MenuLayout l;
    constexpr f32 w = 460.0f, title = 48.0f, row = 34.0f, footer = 30.0f, pad = 16.0f;
    const f32 h = pad * 2 + title + row * f32(std::max(1, rows)) + footer;
    l.u = std::max(0.05f, std::min(screen.x * 0.94f / w, screen.y * 0.94f / h));
    l.u = std::min(l.u, 2.0f);
    const f32 u = l.u;
    l.panel = ui2::centered_rect({screen.x * 0.5f, screen.y * 0.5f}, {w * u, h * u});
    l.title = {l.panel.x + pad * u, l.panel.y + pad * u, (w - 2 * pad) * u, title * u};
    for (i32 i = 0; i < rows; ++i) {
        l.rows.push_back(
            {l.panel.x + pad * u, l.panel.y + (pad + title + row * f32(i)) * u, (w - 2 * pad) * u, row * u});
    }
    l.footer = {l.panel.x + pad * u, l.panel.y + (pad + title + row * f32(rows)) * u, (w - 2 * pad) * u, footer * u};
    return l;
}

Rectf menu_row_track(const Rectf& row) {
    return {row.x + row.w * 0.52f, row.y + row.h * 0.3f, row.w * 0.30f, row.h * 0.4f};
}

i32 menu_row_at(const MenuLayout& layout, Vec2f pointer) {
    for (std::size_t i = 0; i < layout.rows.size(); ++i) {
        if (ui2::contains(layout.rows[i], pointer))
            return i32(i);
    }
    return -1;
}

} // namespace summit
