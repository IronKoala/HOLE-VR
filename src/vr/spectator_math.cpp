#include "vr/spectator_math.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>

namespace holevr::vr::spectator {

View ParseView(const char* text, View fallback) {
    if (!text) return fallback;
    std::string s;
    for (const char* p = text; *p; ++p)
        if (!std::isspace(static_cast<unsigned char>(*p))) s += static_cast<char>(std::tolower(static_cast<unsigned char>(*p)));
    if (s == "both") return View::kBothEyes;
    if (s == "right") return View::kRightEye;
    if (s == "left") return View::kLeftEye;
    return fallback;
}

void StraightAhead(float angle_left, float angle_right, float angle_up, float angle_down, float* u, float* v) {
    const double tl = std::tan(angle_left), tr = std::tan(angle_right);
    const double tu = std::tan(angle_up), td = std::tan(angle_down);
    *u = (tr - tl) > 1e-6 ? static_cast<float>(-tl / (tr - tl)) : 0.5f;
    *v = (tu - td) > 1e-6 ? static_cast<float>(tu / (tu - td)) : 0.5f;
    if (!(*u >= 0.0f && *u <= 1.0f)) *u = 0.5f;
    if (!(*v >= 0.0f && *v <= 1.0f)) *v = 0.5f;
}

Layout ComputeLayout(uint32_t window_w, uint32_t window_h, int eye, float center_u, float center_v,
                     float aspect) {
    Layout out;
    if (window_w < 4 || window_h < 2 || !(aspect > 0.1f && aspect < 10.0f)) return out;
    const uint32_t eye_w = window_w / 2, eye_h = window_h;
    const uint32_t eye_x = eye == 0 ? 0 : eye_w;

    // Biggest rectangle of that shape inside the eye picture.
    uint32_t cw = eye_w, ch = eye_h;
    if (static_cast<double>(eye_w) / eye_h > aspect)
        cw = static_cast<uint32_t>(std::lround(eye_h * static_cast<double>(aspect)));
    else
        ch = static_cast<uint32_t>(std::lround(eye_w / static_cast<double>(aspect)));
    cw = std::clamp<uint32_t>(cw, 1, eye_w);
    ch = std::clamp<uint32_t>(ch, 1, eye_h);

    // Centred on straight ahead, kept inside the eye picture.
    const double cu = std::clamp(static_cast<double>(center_u), 0.0, 1.0);
    const double cv = std::clamp(static_cast<double>(center_v), 0.0, 1.0);
    const long cx = std::clamp<long>(std::lround(cu * eye_w - cw / 2.0), 0, static_cast<long>(eye_w - cw));
    const long cy = std::clamp<long>(std::lround(cv * eye_h - ch / 2.0), 0, static_cast<long>(eye_h - ch));
    out.crop = {eye_x + static_cast<uint32_t>(cx), static_cast<uint32_t>(cy), cw, ch};

    // Scaled to fit the window, centred, keeping its shape.
    const double crop_aspect = static_cast<double>(cw) / ch;
    uint32_t dw = window_w, dh = window_h;
    if (static_cast<double>(window_w) / window_h > crop_aspect)
        dw = static_cast<uint32_t>(std::lround(window_h * crop_aspect));
    else
        dh = static_cast<uint32_t>(std::lround(window_w / crop_aspect));
    dw = std::clamp<uint32_t>(dw, 1, window_w);
    dh = std::clamp<uint32_t>(dh, 1, window_h);
    out.dest = {(window_w - dw) / 2, (window_h - dh) / 2, dw, dh};
    out.valid = true;
    return out;
}

}  // namespace holevr::vr::spectator
