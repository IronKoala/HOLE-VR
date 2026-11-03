#pragma once
// Editing hole-vr.ini text in place, so a setting changed in the headset
// (the VR tab in Options) keeps the player's comments, order and every other
// line exactly as they were. Pure text functions, unit tested natively
// (tests/test_vr_options.cpp).
#include <string>

namespace holevr::config {

// Returns `text` with the value of `key` set to `value`. The last uncommented
// "key=" line wins when reading, so that is the line changed; spacing around
// '=' is kept. A key that isn't there yet is added at the end on its own line.
std::string SetIniValue(const std::string& text, const std::string& key, const std::string& value);

}  // namespace holevr::config
