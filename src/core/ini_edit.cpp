#include "core/ini_edit.h"

#include <vector>

namespace holevr::config {
namespace {
std::string Trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}
}  // namespace

std::string SetIniValue(const std::string& text, const std::string& key, const std::string& value) {
    // Split keeping each line's own ending.
    std::vector<std::string> lines;
    size_t start = 0;
    while (start < text.size()) {
        size_t nl = text.find('\n', start);
        size_t end = nl == std::string::npos ? text.size() : nl + 1;
        lines.push_back(text.substr(start, end - start));
        start = end;
    }
    int found = -1;
    for (size_t i = 0; i < lines.size(); ++i) {
        std::string t = Trim(lines[i]);
        if (t.empty() || t[0] == ';' || t[0] == '#' || t[0] == '[') continue;
        size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        if (Trim(t.substr(0, eq)) == key) found = static_cast<int>(i);
    }
    const bool crlf = text.find("\r\n") != std::string::npos;
    const char* nl = crlf ? "\r\n" : "\n";
    if (found < 0) {
        std::string out = text;
        if (!out.empty() && out.back() != '\n') out += nl;
        out += key + "=" + value + nl;
        return out;
    }
    std::string& line = lines[static_cast<size_t>(found)];
    size_t eq = line.find('=');
    // Keep what came before the value (indent, key, spaces, '=' and the
    // spaces after it) and the line ending; replace only the value.
    size_t vstart = line.find_first_not_of(" \t", eq + 1);
    if (vstart == std::string::npos || line[vstart] == '\r' || line[vstart] == '\n') vstart = eq + 1;
    size_t vend = line.size();
    while (vend > vstart && (line[vend - 1] == '\n' || line[vend - 1] == '\r')) --vend;
    std::string ending = line.substr(vend);
    if (ending.empty() && static_cast<size_t>(found) + 1 < lines.size()) ending = nl;
    line = line.substr(0, vstart) + value + ending;
    std::string out;
    for (const auto& l : lines) out += l;
    return out;
}

}  // namespace holevr::config
