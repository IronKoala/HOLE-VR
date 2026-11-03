// Native unit tests for the VR tab's model and the in-place ini edit.
//   g++ -std=c++20 -Isrc tests/test_vr_options.cpp src/vr/vr_menu_model.cpp src/core/ini_edit.cpp -o /tmp/test_vr_options && /tmp/test_vr_options
#include <cstdio>
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

static void TestRows() {
    const auto& rows = Rows();
    EXPECT(rows.size() >= 15);
    std::set<std::string> labels;
    for (const auto& r : rows) {
        EXPECT(labels.insert(r.label).second);  // unique labels
        EXPECT(!r.choices.empty());
        EXPECT(r.default_choice >= 0 && r.default_choice < static_cast<int>(r.choices.size()));
        if (r.kind == RowKind::kChoice) {
            EXPECT(r.choices.size() >= 2);
            for (const auto& c : r.choices) EXPECT(!c.sets.empty());
        }
        // Short enough for a menu button.
        for (size_t c = 0; c < r.choices.size(); ++c) EXPECT(RowText(r, static_cast<int>(c)).size() <= 40);
    }
}

static const Row& RowNamed(const char* label) {
    for (const auto& r : Rows())
        if (std::string(r.label) == label) return r;
    std::printf("no row %s\n", label);
    std::abort();
}

static void TestCurrent() {
    std::map<std::string, std::string> ini;
    Getter get = [&](const char* k) {
        auto it = ini.find(k);
        return it == ini.end() ? std::string() : it->second;
    };
    const Row& turn = RowNamed("Turning");
    EXPECT(CurrentChoice(turn, get) == 0);  // default smooth
    ini["turn_mode"] = "0";
    EXPECT(CurrentChoice(turn, get) == 1);
    // Numbers compare as numbers.
    const Row& snap = RowNamed("Snap turn");
    ini["snap_turn_degrees"] = "45.0";
    EXPECT(std::string(snap.choices[static_cast<size_t>(CurrentChoice(snap, get))].text) == "45 deg");
    ini["snap_turn_degrees"] = "33";  // not a choice: the default
    EXPECT(CurrentChoice(snap, get) == snap.default_choice);
    // Height: fixed_height decides, height_mode picks seated.
    const Row& height = RowNamed("Height");
    ini["fixed_height"] = "1";
    EXPECT(std::string(height.choices[static_cast<size_t>(CurrentChoice(height, get))].text) == "Character");
    ini["height_mode"] = "seated";
    EXPECT(std::string(height.choices[static_cast<size_t>(CurrentChoice(height, get))].text) == "Seated");
    ini["fixed_height"] = "0";
    ini["height_mode"] = "fixed";  // an old ini with real height and the new key's default
    EXPECT(std::string(height.choices[static_cast<size_t>(CurrentChoice(height, get))].text) == "Your own");
    EXPECT(RowText(turn, 1) == "Turning: Snap");
    EXPECT(SameValue("1", "1.000"));
    EXPECT(SameValue("Seated", "seated"));
    EXPECT(!SameValue("1", "0"));
}

int main() {
    TestIniEdit();
    TestRows();
    TestCurrent();
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("vr options: all tests passed\n");
    return 0;
}
