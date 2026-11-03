#include "vr/vr_menu_model.h"

#include <cctype>
#include <cmath>
#include <cstdlib>

namespace holevr::vr::vr_menu {

const std::vector<Row>& Rows() {
    using K = RowKind;
    static const std::vector<Row> rows = {
        {"Moving", K::kChoice,
         {{"Smooth", {{"locomotion", "smooth"}}}, {"Teleport", {{"locomotion", "teleport"}}}}, 0},
        {"Turning", K::kChoice, {{"Smooth", {{"turn_mode", "1"}}}, {"Snap", {{"turn_mode", "0"}}}}, 0},
        {"Snap turn",
         K::kChoice,
         {{"15 deg", {{"snap_turn_degrees", "15"}}},
          {"30 deg", {{"snap_turn_degrees", "30"}}},
          {"45 deg", {{"snap_turn_degrees", "45"}}},
          {"60 deg", {{"snap_turn_degrees", "60"}}},
          {"90 deg", {{"snap_turn_degrees", "90"}}}},
         1},
        {"Turn speed",
         K::kChoice,
         {{"60 deg/s", {{"smooth_turn_speed", "60"}}},
          {"90 deg/s", {{"smooth_turn_speed", "90"}}},
          {"120 deg/s", {{"smooth_turn_speed", "120"}}},
          {"180 deg/s", {{"smooth_turn_speed", "180"}}},
          {"240 deg/s", {{"smooth_turn_speed", "240"}}}},
         1},
        {"Move stick", K::kChoice,
         {{"Left", {{"move_stick", "left"}}}, {"Right", {{"move_stick", "right"}}}}, 0},
        {"Move toward",
         K::kChoice,
         {{"Where you look", {{"move_direction", "0"}}}, {"Stick hand", {{"move_direction", "1"}}}},
         0},
        {"Vignette moving",
         K::kChoice,
         {{"Off", {{"comfort_vignette_move", "0"}}},
          {"Light", {{"comfort_vignette_move", "0.35"}}},
          {"Medium", {{"comfort_vignette_move", "0.6"}}},
          {"Strong", {{"comfort_vignette_move", "0.85"}}}},
         0},
        {"Vignette turning",
         K::kChoice,
         {{"Off", {{"comfort_vignette_turn", "0"}}},
          {"Light", {{"comfort_vignette_turn", "0.35"}}},
          {"Medium", {{"comfort_vignette_turn", "0.6"}}},
          {"Strong", {{"comfort_vignette_turn", "0.85"}}}},
         0},
        {"Head in a wall", K::kChoice,
         {{"Fades", {{"head_wall_fade", "1"}}}, {"No fade", {{"head_wall_fade", "0"}}}}, 0},
        {"Height",
         K::kChoice,
         {{"Character", {{"fixed_height", "1"}, {"height_mode", "fixed"}}},
          {"Your own", {{"fixed_height", "0"}, {"height_mode", "real"}}},
          {"Seated", {{"fixed_height", "1"}, {"height_mode", "seated"}}}},
         0},
        {"Real crouch", K::kChoice, {{"On", {{"physical_crouch", "1"}}}, {"Off", {{"physical_crouch", "0"}}}}, 0},
        {"Hop to jump", K::kChoice, {{"Off", {{"physical_jump", "0"}}}, {"On", {{"physical_jump", "1"}}}}, 0},
        {"World size",
         K::kChoice,
         {{"80%", {{"world_scale", "0.8"}}},
          {"90%", {{"world_scale", "0.9"}}},
          {"100%", {{"world_scale", "1"}}},
          {"110%", {{"world_scale", "1.1"}}},
          {"120%", {{"world_scale", "1.2"}}}},
         2},
        {"Room walking", K::kChoice, {{"On", {{"room_scale_follow", "1"}}}, {"Off", {{"room_scale_follow", "0"}}}}, 0},
        {"Grip", K::kChoice, {{"Hold", {{"grip_toggle", "0"}}}, {"Toggle", {{"grip_toggle", "1"}}}}, 0},
        {"Aiming eye", K::kChoice,
         {{"Right", {{"dominant_eye_right", "1"}}}, {"Left", {{"dominant_eye_right", "0"}}}}, 0},
        {"Aim smoothing",
         K::kChoice,
         {{"Off", {{"aim_smoothing", "0"}}},
          {"Light", {{"aim_smoothing", "0.33"}}},
          {"Medium", {{"aim_smoothing", "0.66"}}},
          {"Strong", {{"aim_smoothing", "1"}}}},
         0},
        {"Rifle stock", K::kChoice,
         {{"On", {{"rifle_virtual_stock", "1"}}}, {"Off", {{"rifle_virtual_stock", "0"}}}}, 0},
        {"Gun stops at walls", K::kChoice,
         {{"On", {{"gun_wall_collision", "1"}}}, {"Off", {{"gun_wall_collision", "0"}}}}, 0},
        {"Hands stop at walls", K::kChoice,
         {{"On", {{"hand_wall_collision", "1"}}}, {"Off", {{"hand_wall_collision", "0"}}}}, 0},
        {"Gun kick",
         K::kChoice,
         {{"Off", {{"gun_recoil_kick", "0"}}},
          {"Light", {{"gun_recoil_kick", "0.5"}}},
          {"Normal", {{"gun_recoil_kick", "1"}}},
          {"Strong", {{"gun_recoil_kick", "1.5"}}}},
         2},
        {"Monitor shows",
         K::kChoice,
         {{"Right eye", {{"monitor_view", "right"}}},
          {"Left eye", {{"monitor_view", "left"}}},
          {"Both eyes", {{"monitor_view", "both"}}}},
         0},
        {"Calibrate body", K::kMeasureArms, {{"click, then close menu", {}}}, 0},
        {"Edit hand grips", K::kGripEditor, {{"click, then close menu", {}}, {"open, click to close", {}}}, 0},
        {"Move holsters", K::kMoveHolsters, {{"click, then close menu", {}}}, 0},
    };
    return rows;
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
            if (!v.empty() && SameValue(v, sets[k].value)) score += k == 0 ? 2 : 1;
        }
        if (score > best_score) {
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

}  // namespace holevr::vr::vr_menu
