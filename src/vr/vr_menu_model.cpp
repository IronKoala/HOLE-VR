#include "vr/vr_menu_model.h"

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace holevr::vr::vr_menu {

const std::vector<Row>& Rows() {
    using K = RowKind;
    // Owner 2026-11-06: "Smooth turn, snap turn, teleport all merge into
    // row", then "add an option to turn off joystick turning altogether".
    // One row, Movement, for what were Moving (smooth/teleport), Turning
    // (smooth/snap), Snap turn (how far) and Turn speed, plus no stick
    // turning (turn_mode=2: you turn with your body): Teleport with no
    // turning or 30 deg snaps, then walking with no turning, snap turns of
    // 15-90 deg or smooth turns of 60-240 deg/s (the right arrow steps
    // toward smooth and faster). locomotion is every choice's first key.
    //
    // Hidden while they do nothing: Move toward with Teleport (the arc goes
    // where the stick hand points), Vignette turning unless turning is
    // smooth (a snap is instant), Real crouch and Hop to jump when seated
    // (controls.cpp turns both off then).
    constexpr unsigned kWalk = 0x1FFCu;         // Movement: the walking choices (2-12)
    constexpr unsigned kSmoothTurn = 0x1F00u;   // Movement: the smooth turn choices (8-12)
    constexpr unsigned kStanding = (1u << 0) | (1u << 1);  // Height: Character, Your own
    // "Aiming eye" (dominant_eye_right) is gone from the tab (owner
    // 2026-11-06: useful before 6DoF aiming, not now): it only chose which
    // eye the rifle's shoulder lock lines the sights up with. The ini key
    // still works (right eye unless set), for a later left-handed mode.
    static const std::vector<Row> rows = {
        {"Movement",
         K::kChoice,
         {{"Teleport, no turning", {{"locomotion", "teleport"}, {"turn_mode", "2"}}},
          {"Teleport, snap turn 30 deg", {{"locomotion", "teleport"}, {"turn_mode", "0"}, {"snap_turn_degrees", "30"}}},
          {"Walk, no turning", {{"locomotion", "smooth"}, {"turn_mode", "2"}}},
          {"Walk, snap turn 15 deg", {{"locomotion", "smooth"}, {"turn_mode", "0"}, {"snap_turn_degrees", "15"}}},
          {"Walk, snap turn 30 deg", {{"locomotion", "smooth"}, {"turn_mode", "0"}, {"snap_turn_degrees", "30"}}},
          {"Walk, snap turn 45 deg", {{"locomotion", "smooth"}, {"turn_mode", "0"}, {"snap_turn_degrees", "45"}}},
          {"Walk, snap turn 60 deg", {{"locomotion", "smooth"}, {"turn_mode", "0"}, {"snap_turn_degrees", "60"}}},
          {"Walk, snap turn 90 deg", {{"locomotion", "smooth"}, {"turn_mode", "0"}, {"snap_turn_degrees", "90"}}},
          {"Walk, smooth turn 60 deg/s", {{"locomotion", "smooth"}, {"turn_mode", "1"}, {"smooth_turn_speed", "60"}}},
          {"Walk, smooth turn 90 deg/s", {{"locomotion", "smooth"}, {"turn_mode", "1"}, {"smooth_turn_speed", "90"}}},
          {"Walk, smooth turn 120 deg/s", {{"locomotion", "smooth"}, {"turn_mode", "1"}, {"smooth_turn_speed", "120"}}},
          {"Walk, smooth turn 180 deg/s", {{"locomotion", "smooth"}, {"turn_mode", "1"}, {"smooth_turn_speed", "180"}}},
          {"Walk, smooth turn 240 deg/s", {{"locomotion", "smooth"}, {"turn_mode", "1"}, {"smooth_turn_speed", "240"}}}},
         9,
         "Walk with the stick or teleport (aim an arc, let go to move there). Turn by snaps, smoothly, or not "
         "at all: only with your body."},
        {"Move stick", K::kChoice,
         {{"Left", {{"move_stick", "left"}}}, {"Right", {{"move_stick", "right"}}}}, 0,
         "Which hand's stick moves you. The other stick turns, jumps and crouches."},
        {"Move toward",
         K::kChoice,
         {{"Where you look", {{"move_direction", "0"}}}, {"Stick hand", {{"move_direction", "1"}}}},
         0, "Pushing the stick forward walks the way your head faces, or the way the moving hand points.",
         "Movement", kWalk},
        {"Vignette moving",
         K::kChoice,
         {{"Off", {{"comfort_vignette_move", "0"}}},
          {"Light", {{"comfort_vignette_move", "0.35"}}},
          {"Medium", {{"comfort_vignette_move", "0.6"}}},
          {"Strong", {{"comfort_vignette_move", "0.85"}}}},
         0, "Darkens the edges of your view while you move or fall, to ease motion sickness."},
        {"Vignette turning",
         K::kChoice,
         {{"Off", {{"comfort_vignette_turn", "0"}}},
          {"Light", {{"comfort_vignette_turn", "0.35"}}},
          {"Medium", {{"comfort_vignette_turn", "0.6"}}},
          {"Strong", {{"comfort_vignette_turn", "0.85"}}}},
         0, "Darkens the edges of your view while the stick turns you smoothly, to ease motion sickness.",
         "Movement", kSmoothTurn},
        // One row for what were "Head in a wall" (on/off) and "Wall fade
        // starts" (how far): the distance only mattered with the fade on.
        // On/off comes first so it outweighs the distance; among the
        // distances the ini's own one wins, and with none (or one not
        // listed) the default, 15 cm.
        {"Wall fade",
         K::kChoice,
         {{"Off", {{"head_wall_fade", "0"}}},
          {"5 cm away", {{"head_wall_fade", "1"}, {"head_wall_fade_start_cm", "5"}}},
          {"10 cm away", {{"head_wall_fade", "1"}, {"head_wall_fade_start_cm", "10"}}},
          {"15 cm away", {{"head_wall_fade", "1"}, {"head_wall_fade_start_cm", "15"}}},
          {"20 cm away", {{"head_wall_fade", "1"}, {"head_wall_fade_start_cm", "20"}}},
          {"25 cm away", {{"head_wall_fade", "1"}, {"head_wall_fade_start_cm", "25"}}}},
         3, "Fades your view to black when your head goes into a wall, starting this far from it."},
        {"Height",
         K::kChoice,
         {{"Character", {{"fixed_height", "1"}, {"height_mode", "fixed"}}},
          {"Your own", {{"fixed_height", "0"}, {"height_mode", "real"}}},
          {"Seated", {{"fixed_height", "1"}, {"height_mode", "seated"}}}},
         0,
         "Character: eyes at the character's height. Your own: your real height. Seated: like Character, for "
         "sitting down."},
        {"Real crouch", K::kChoice, {{"On", {{"physical_crouch", "1"}}}, {"Off", {{"physical_crouch", "0"}}}}, 0,
         "Crouch for real and your character crouches too.", "Height", kStanding},
        {"Hop to jump", K::kChoice, {{"Off", {{"physical_jump", "0"}}}, {"On", {{"physical_jump", "1"}}}}, 0,
         "A quick real hop makes your character jump. Jumping on the stick always works.", "Height", kStanding},
        {"World size",
         K::kChoice,
         {{"80%", {{"world_scale", "0.8"}}},
          {"90%", {{"world_scale", "0.9"}}},
          {"100%", {{"world_scale", "1"}}},
          {"110%", {{"world_scale", "1.1"}}},
          {"120%", {{"world_scale", "1.2"}}}},
         2, "How big the world looks around you. Above 100% it looks bigger, so you feel smaller."},
        {"Roomscale", K::kChoice, {{"On", {{"room_scale_follow", "1"}}}, {"Off", {{"room_scale_follow", "0"}}}}, 0,
         "Walking around your real room moves your character with you. Walls still stop it."},
        {"Grip", K::kChoice, {{"Hold", {{"grip_toggle", "0"}}}, {"Toggle", {{"grip_toggle", "1"}}}}, 0,
         "Hold: keep squeezing grip to keep holding. Toggle: squeeze once to take a gun or mag, again to let go."},
        {"Aim smoothing",
         K::kChoice,
         {{"Off", {{"aim_smoothing", "0"}}},
          {"Light", {{"aim_smoothing", "0.33"}}},
          {"Medium", {{"aim_smoothing", "0.66"}}},
          {"Strong", {{"aim_smoothing", "1"}}}},
         0, "Takes small shakes out of the gun hand. Quick swings are never slowed."},
        {"Rifle stock", K::kChoice,
         {{"On", {{"rifle_virtual_stock", "1"}}}, {"Off", {{"rifle_virtual_stock", "0"}}}}, 0,
         "Bring the rifle's butt to your shoulder and it locks there like a real stock, for a steadier aim."},
        {"Gun stops at walls", K::kChoice,
         {{"On", {{"gun_wall_collision", "1"}}}, {"Off", {{"gun_wall_collision", "0"}}}}, 0,
         "The gun in your hand stops at walls and tables instead of going through them."},
        {"Hands stop at walls", K::kChoice,
         {{"On", {{"hand_wall_collision", "1"}}}, {"Off", {{"hand_wall_collision", "0"}}}}, 0,
         "Empty hands stop at walls and tables, and their fingers give way against them."},
        {"Gun kick",
         K::kChoice,
         {{"Off", {{"gun_recoil_kick", "0"}}},
          {"Light", {{"gun_recoil_kick", "0.5"}}},
          {"Normal", {{"gun_recoil_kick", "1"}}},
          {"Strong", {{"gun_recoil_kick", "1.5"}}}},
         2, "How hard each shot flips the gun up and pushes it back in your hand."},
        {"Monitor shows",
         K::kChoice,
         {{"Right eye", {{"monitor_view", "right"}}},
          {"Left eye", {{"monitor_view", "left"}}},
          {"Both eyes", {{"monitor_view", "both"}}}},
         0, "What the game window on your monitor shows, for recording: one eye as a normal picture, or both."},
        {"Calibrate body", K::kMeasureArms, {{"click, then close menu", {}}}, 0,
         "Measures your height and arms so the body fits you. Click, close the menu, then follow the steps."},
        {"Edit hand grips", K::kGripEditor, {{"click, then close menu", {}}, {"open, click to close", {}}}, 0,
         "Opens the editor where you set how each hand holds each item, once the menu closes."},
        {"Move holsters", K::kMoveHolsters, {{"click, then close menu", {}}}, 0,
         "Close the menu, then hold each item where you want its holster until the 5 buzzes end."},
    };
    return rows;
}

const Row* Find(const char* label) {
    if (!label) return nullptr;
    for (const auto& r : Rows())
        if (std::strcmp(r.label, label) == 0) return &r;
    return nullptr;
}

namespace {
bool Number(const std::string& s, double* out) {
    if (s.empty()) return false;
    char* end = nullptr;
    double v = std::strtod(s.c_str(), &end);
    while (end && *end && std::isspace(static_cast<unsigned char>(*end))) ++end;
    if (!end || *end) return false;
    *out = v;
    return true;
}
}  // namespace

bool SameValue(const std::string& a, const std::string& b) {
    double x, y;
    if (Number(a, &x) && Number(b, &y)) return std::fabs(x - y) < 1e-3;
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    return true;
}

int CurrentChoice(const Row& row, const Getter& get) {
    int best = -1, best_score = 0;
    for (size_t c = 0; c < row.choices.size(); ++c) {
        int score = 0;
        const auto& sets = row.choices[c].sets;
        for (size_t k = 0; k < sets.size(); ++k) {
            std::string v = get(sets[k].key);
            if (v.empty()) continue;
            const int weight = k == 0 ? 2 : 1;
            score += SameValue(v, sets[k].value) ? weight : -weight;
        }
        if (score > best_score || (score > 0 && score == best_score && static_cast<int>(c) == row.default_choice)) {
            best_score = score;
            best = static_cast<int>(c);
        }
    }
    if (best < 0) best = row.default_choice;
    if (best < 0 || best >= static_cast<int>(row.choices.size())) best = 0;
    return best;
}

std::string RowText(const Row& row, int choice) {
    if (row.choices.empty()) return row.label;
    if (choice < 0 || choice >= static_cast<int>(row.choices.size())) choice = 0;
    return std::string(row.label) + ": " + row.choices[static_cast<size_t>(choice)].text;
}

bool Shown(const Row& row, const Getter& get) {
    if (!row.only_with) return true;
    const Row* by = Find(row.only_with);
    if (!by || by == &row) return true;
    const int c = CurrentChoice(*by, get);
    return c >= 0 && c < 32 && (row.only_choices & (1u << c)) != 0;
}

}  // namespace holevr::vr::vr_menu
