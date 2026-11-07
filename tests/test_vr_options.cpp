// Native unit tests for the VR tab's model and the in-place ini edit.
//   g++ -std=c++20 -Isrc tests/test_vr_options.cpp src/vr/vr_menu_model.cpp src/core/ini_edit.cpp -o /tmp/test_vr_options && /tmp/test_vr_options
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>

#include "core/ini_edit.h"
#include "vr/vr_menu_model.h"

using namespace holevr;
using namespace holevr::vr::vr_menu;

static int failures = 0;
#define EXPECT(c)                                                     \
    do {                                                              \
        if (!(c)) {                                                   \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);  \
            ++failures;                                               \
        }                                                             \
    } while (0)

static void TestIniEdit() {
    const std::string ini = "; comment turn_mode=5\nturn_mode=1\nsnap_turn_degrees = 30\n\n; end\n";
    std::string out = config::SetIniValue(ini, "turn_mode", "0");
    EXPECT(out == "; comment turn_mode=5\nturn_mode=0\nsnap_turn_degrees = 30\n\n; end\n");
    // Spacing around '=' is kept.
    out = config::SetIniValue(ini, "snap_turn_degrees", "45");
    EXPECT(out == "; comment turn_mode=5\nturn_mode=1\nsnap_turn_degrees = 45\n\n; end\n");
    // Missing key: added at the end.
    out = config::SetIniValue(ini, "aim_smoothing", "0.33");
    EXPECT(out == ini + "aim_smoothing=0.33\n");
    // No trailing newline.
    out = config::SetIniValue("a=1", "b", "2");
    EXPECT(out == "a=1\nb=2\n");
    out = config::SetIniValue("a=1", "a", "2");
    EXPECT(out == "a=2");
    // The last line of a key wins when the ini is read, so that one changes.
    out = config::SetIniValue("a=1\nx=0\na=2\n", "a", "3");
    EXPECT(out == "a=1\nx=0\na=3\n");
    // Windows line endings kept.
    out = config::SetIniValue("a=1\r\nb=2\r\n", "a", "9");
    EXPECT(out == "a=9\r\nb=2\r\n");
    out = config::SetIniValue("a=1\r\n", "c", "3");
    EXPECT(out == "a=1\r\nc=3\r\n");
    // A key that is a prefix of another is not confused with it.
    out = config::SetIniValue("turn_mode_x=1\nturn_mode=1\n", "turn_mode", "0");
    EXPECT(out == "turn_mode_x=1\nturn_mode=0\n");
    // Empty value.
    out = config::SetIniValue("a=\nb=2\n", "a", "5");
    EXPECT(out == "a=5\nb=2\n");
}

static const Row& RowNamed(const char* label) {
    for (const auto& r : Rows())
        if (std::string(r.label) == label) return r;
    std::printf("no row %s\n", label);
    std::abort();
}

static void TestRows() {
    const auto& rows = Rows();
    EXPECT(rows.size() >= 15);
    for (const auto& r : rows) {
        // Labels are unique, except rows hidden by the same row on choices
        // that never overlap, so only one of them is ever on screen (the
        // two Turn speed rows: deg/s for smooth turns, deg for snaps).
        for (const auto& o : rows) {
            if (&o == &r || std::string(o.label) != r.label) continue;
            EXPECT(r.only_with && o.only_with && std::string(r.only_with) == o.only_with);
            EXPECT((r.only_choices & o.only_choices) == 0);
        }
        EXPECT(!r.choices.empty());
        EXPECT(r.default_choice >= 0 && r.default_choice < static_cast<int>(r.choices.size()));
        if (r.kind == RowKind::kChoice) {
            EXPECT(r.choices.size() >= 2);
            for (const auto& c : r.choices) EXPECT(!c.sets.empty());
        }
        // Short enough for a menu button.
        for (size_t c = 0; c < r.choices.size(); ++c) EXPECT(RowText(r, static_cast<int>(c)).size() <= 40);
        // Every row says what it does, briefly (a few lines beside the list).
        const size_t about = std::strlen(r.about);
        EXPECT(about >= 20 && about <= 150);
        if (about < 20 || about > 150) std::printf("  %s: description is %zu characters\n", r.label, about);
        // A row hidden by another names a real row, and is shown with at
        // least one of its choices but not all of them.
        if (r.only_with) {
            const Row* by = Find(r.only_with);
            EXPECT(by && by != &r);
            if (by) {
                const unsigned all = (1u << by->choices.size()) - 1;
                EXPECT((r.only_choices & all) != 0 && (r.only_choices & all) != all);
            }
        }
    }
    // Aiming eye is gone (6DoF aiming made it moot); the ini key still works.
    EXPECT(!Find("Aiming eye"));
    for (const auto& r : rows)
        for (const auto& c : r.choices)
            for (const auto& kv : c.sets) EXPECT(std::string(kv.key) != "dominant_eye_right");
}

// Rows that do nothing with the current choices are hidden.
static void TestShown() {
    std::map<std::string, std::string> ini;
    Getter get = [&](const char* k) {
        auto it = ini.find(k);
        return it == ini.end() ? std::string() : it->second;
    };
    auto shown = [&](const char* label) { return Shown(RowNamed(label), get); };
    // Defaults: walking with smooth turns, character height.
    EXPECT(shown("Vignette turning") && shown("Move toward"));
    EXPECT(shown("Real crouch") && shown("Hop to jump"));
    EXPECT(shown("Moving") && shown("Turn style") && shown("Vignette moving") && shown("Calibrate body"));
    // Exactly one Turn speed row is on screen while the stick turns, none when it doesn't.
    auto speed_rows = [&] {
        int n = 0;
        const Row* on = nullptr;
        for (const auto& r : Rows())
            if (std::string(r.label) == "Turn speed" && Shown(r, get)) ++n, on = &r;
        return std::make_pair(n, on);
    };
    EXPECT(speed_rows().first == 1 && std::string(speed_rows().second->choices[0].text) == "60 deg/s");
    ini["locomotion"] = "smooth";
    ini["turn_mode"] = "0";  // snap turns
    ini["snap_turn_degrees"] = "45";
    EXPECT(!shown("Vignette turning") && shown("Move toward"));
    EXPECT(speed_rows().first == 1 && std::string(speed_rows().second->choices[0].text) == "15 deg a flick");
    ini["turn_mode"] = "2";  // no stick turning
    EXPECT(!shown("Vignette turning") && shown("Move toward"));
    EXPECT(speed_rows().first == 0);
    ini["locomotion"] = "teleport";
    ini["turn_mode"] = "0";
    EXPECT(!shown("Move toward") && !shown("Vignette turning"));
    EXPECT(speed_rows().first == 1);  // teleport turns with any style
    ini["turn_mode"] = "1";
    EXPECT(shown("Vignette turning"));
    EXPECT(shown("Vignette moving"));  // falls still count
    ini["fixed_height"] = "1";
    ini["height_mode"] = "seated";
    EXPECT(!shown("Real crouch") && !shown("Hop to jump"));
    ini["fixed_height"] = "0";
    ini["height_mode"] = "real";
    EXPECT(shown("Real crouch") && shown("Hop to jump"));
}

static void TestCurrent() {
    std::map<std::string, std::string> ini;
    Getter get = [&](const char* k) {
        auto it = ini.find(k);
        return it == ini.end() ? std::string() : it->second;
    };
    auto text = [&](const Row& row) {
        return std::string(row.choices[static_cast<size_t>(CurrentChoice(row, get))].text);
    };
    for (const char* old : {"Movement", "Turning", "Snap turn"}) EXPECT(!Find(old));
    const Row& moving = RowNamed("Moving");
    const Row& style = RowNamed("Turn style");
    const Row* smooth_speed = nullptr;
    const Row* snap_step = nullptr;
    for (const auto& r : Rows())
        if (std::string(r.label) == "Turn speed")
            (std::string(r.choices[0].sets[0].key) == "smooth_turn_speed" ? smooth_speed : snap_step) = &r;
    EXPECT(smooth_speed && snap_step);
    // Defaults: walk, smooth turns at 90 deg/s, snaps of 30 deg.
    EXPECT(text(moving) == "Walk" && text(style) == "Smooth");
    EXPECT(text(*smooth_speed) == "90 deg/s" && text(*snap_step) == "30 deg a flick");
    // Each turn style is its own turn_mode: 1 smooth, 0 snap, 2 off.
    ini["turn_mode"] = "0";
    EXPECT(text(style) == "Snap");
    ini["turn_mode"] = "2";
    EXPECT(text(style) == "Off");
    ini["turn_mode"] = "1";
    EXPECT(text(style) == "Smooth");
    for (const auto& c : style.choices) EXPECT(c.sets.size() == 1 && std::string(c.sets[0].key) == "turn_mode");
    ini["locomotion"] = "teleport";
    EXPECT(text(moving) == "Teleport");
    ini["smooth_turn_speed"] = "180";
    EXPECT(text(*smooth_speed) == "180 deg/s");
    ini["smooth_turn_speed"] = "100";  // not a choice: the default
    EXPECT(text(*smooth_speed) == "90 deg/s");
    ini["snap_turn_degrees"] = "45.0";  // numbers compare as numbers
    EXPECT(text(*snap_step) == "45 deg a flick");
    // Height: fixed_height decides, height_mode picks seated.
    const Row& height = RowNamed("Height");
    ini["fixed_height"] = "1";
    EXPECT(std::string(height.choices[static_cast<size_t>(CurrentChoice(height, get))].text) == "Character");
    ini["height_mode"] = "seated";
    EXPECT(std::string(height.choices[static_cast<size_t>(CurrentChoice(height, get))].text) == "Seated");
    ini["fixed_height"] = "0";
    ini["height_mode"] = "fixed";  // an old ini with real height and the new key's default
    EXPECT(std::string(height.choices[static_cast<size_t>(CurrentChoice(height, get))].text) == "Your own");
    EXPECT(RowText(style, 2) == "Turn style: Off");
    EXPECT(RowText(*snap_step, 1) == "Turn speed: 30 deg a flick");
    // Gun kick: five steps past Strong, the last 4x Strong.
    const Row& kick = RowNamed("Gun kick");
    auto kick_value = [&](const char* name) {
        for (const auto& c : kick.choices)
            if (std::string(c.text) == name) return std::atof(c.sets[0].value);
        return -1.0;
    };
    EXPECT(kick.choices.size() == 9);
    EXPECT(std::string(kick.choices[static_cast<size_t>(kick.default_choice)].text) == "Normal");
    EXPECT(std::fabs(kick_value("Extreme") - 4 * kick_value("Strong")) < 1e-9);
    for (size_t c = 1; c < kick.choices.size(); ++c)
        EXPECT(std::atof(kick.choices[c].sets[0].value) > std::atof(kick.choices[c - 1].sets[0].value));
    EXPECT(Find("Roomscale") && !Find("Room walking"));
    EXPECT(SameValue("1", "1.000"));
    EXPECT(SameValue("Seated", "seated"));
    EXPECT(!SameValue("1", "0"));
}

// Teleport and hop to jump stay off unless chosen; the setup tools are
// action rows.
static void TestDefaultsAndActions() {
    std::map<std::string, std::string> ini;
    Getter get = [&](const char* k) {
        auto it = ini.find(k);
        return it == ini.end() ? std::string() : it->second;
    };
    const Row& moving = RowNamed("Moving");
    EXPECT(std::string(moving.choices[static_cast<size_t>(CurrentChoice(moving, get))].text) == "Walk");
    const Row& hop = RowNamed("Hop to jump");
    EXPECT(std::string(hop.choices[static_cast<size_t>(CurrentChoice(hop, get))].text) == "Off");
    EXPECT(RowNamed("Calibrate body").kind == RowKind::kMeasureArms);
    EXPECT(RowNamed("Edit hand grips").kind == RowKind::kGripEditor);
    EXPECT(RowNamed("Edit hand grips").choices.size() == 2);  // closed, open
    EXPECT(RowNamed("Move holsters").kind == RowKind::kMoveHolsters);
    // The monitor shows the right eye unless chosen otherwise.
    const Row& monitor = RowNamed("Monitor shows");
    EXPECT(std::string(monitor.choices[static_cast<size_t>(CurrentChoice(monitor, get))].text) == "Right eye");
    ini["monitor_view"] = "Both";
    EXPECT(std::string(monitor.choices[static_cast<size_t>(CurrentChoice(monitor, get))].text) == "Both eyes");
    // One wall fade row: on, starting 15 cm from a surface, unless chosen
    // otherwise.
    const Row& fade = RowNamed("Wall fade");
    auto fade_text = [&] { return std::string(fade.choices[static_cast<size_t>(CurrentChoice(fade, get))].text); };
    EXPECT(fade_text() == "15 cm away");
    ini["head_wall_fade"] = "1";  // on, no distance yet: the default distance, not the first
    EXPECT(fade_text() == "15 cm away");
    ini["head_wall_fade_start_cm"] = "25";
    EXPECT(fade_text() == "25 cm away");
    ini["head_wall_fade_start_cm"] = "12";  // not a choice: the default distance
    EXPECT(fade_text() == "15 cm away");
    ini["head_wall_fade"] = "0";  // off wins over a matching distance
    ini["head_wall_fade_start_cm"] = "15";
    EXPECT(fade_text() == "Off");
    ini.erase("head_wall_fade");  // only a distance: on, at it
    ini["head_wall_fade_start_cm"] = "5";
    EXPECT(fade_text() == "5 cm away");
    EXPECT(!Find("Head in a wall") && !Find("Wall fade starts"));
    // Choosing a distance turns the fade on too.
    for (const auto& c : fade.choices) {
        bool sets_on = false;
        for (const auto& kv : c.sets)
            if (std::string(kv.key) == "head_wall_fade") sets_on = std::string(kv.value) == "1";
        EXPECT(sets_on == (std::string(c.text) != "Off"));
    }
    // Empty hands stop at walls unless turned off.
    const Row& hand_walls = RowNamed("Hands stop at walls");
    EXPECT(std::string(hand_walls.choices[static_cast<size_t>(CurrentChoice(hand_walls, get))].text) == "On");
    ini["hand_wall_collision"] = "0";
    EXPECT(std::string(hand_walls.choices[static_cast<size_t>(CurrentChoice(hand_walls, get))].text) == "Off");
}

int main() {
    TestIniEdit();
    TestRows();
    TestCurrent();
    TestShown();
    TestDefaultsAndActions();
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("vr options: all tests passed\n");
    return 0;
}
