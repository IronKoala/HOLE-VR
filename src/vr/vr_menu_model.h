#pragma once
// What the VR tab in Options holds: one row per setting, each with its
// choices and the hole-vr.ini values a choice stands for. Clicking a row
// picks its next choice. Pure data and logic (unit tested natively in
// tests/test_vr_options.cpp); vr/vr_options puts it on the game's menu.
#include <functional>
#include <string>
#include <vector>

namespace holevr::vr::vr_menu {

struct KeyValue {
    const char* key;
    const char* value;
};
struct Choice {
    const char* text;
    std::vector<KeyValue> sets;  // the first one decides which choice is current
};
// kChoice cycles a setting. The others are actions run when clicked:
// kMeasureArms = Calibrate body, kGripEditor = open/close the grip editor
// (choice 1 is shown while it is open), kMoveHolsters = move the holsters.
enum class RowKind { kChoice, kMeasureArms, kGripEditor, kMoveHolsters };
struct Row {
    const char* label;
    RowKind kind = RowKind::kChoice;
    std::vector<Choice> choices;
    int default_choice = 0;  // when the ini has none of the keys
};

const std::vector<Row>& Rows();

// Reads a key's current value; empty when the ini doesn't have it.
using Getter = std::function<std::string(const char* key)>;
// The choice the current settings match best (each matching key counts, the
// first key of a choice counts double), or the row's default if none match.
int CurrentChoice(const Row& row, const Getter& get);
// Values match as numbers when both are numbers, else ignoring case.
bool SameValue(const std::string& a, const std::string& b);
// "Label: choice", what the row's button shows.
std::string RowText(const Row& row, int choice);

}  // namespace holevr::vr::vr_menu
