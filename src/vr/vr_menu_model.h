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
    // What the setting does, in a sentence or two: shown beside the list
    // while the row is pointed at (owner 2026-11-06: "a brief description of
    // what the option does just off to the right").
    const char* about = "";
    // A row that does nothing with some choice of another row is hidden then
    // (owner 2026-11-06: "are any of them redundant/moot?"): shown only while
    // the row labelled `only_with` is on one of `only_choices` (bit n =
    // choice n). No `only_with`: always shown.
    const char* only_with = nullptr;
    unsigned only_choices = 0;
};

const std::vector<Row>& Rows();
// The row with this label, or nullptr.
const Row* Find(const char* label);

// Reads a key's current value; empty when the ini doesn't have it.
using Getter = std::function<std::string(const char* key)>;
// The choice the current settings match best (each matching key counts, the
// first key of a choice counts double; a tie goes to the row's default), or
// the row's default if none match.
int CurrentChoice(const Row& row, const Getter& get);
// Values match as numbers when both are numbers, else ignoring case.
bool SameValue(const std::string& a, const std::string& b);
// "Label: choice", what the row's button shows.
std::string RowText(const Row& row, int choice);
// Whether the row is shown with the current settings (see Row::only_with).
bool Shown(const Row& row, const Getter& get);

}  // namespace holevr::vr::vr_menu
