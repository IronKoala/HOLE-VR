#include "vr/body/grip_record.h"

#include <cmath>
#include <cstdio>

namespace holevr::vr::grip_record {

using namespace ik;

const char* TipName(int t) {
    static const char* n[kTipCount] = {"thumb", "index_off", "index_rest", "index_pull", "middle", "ring", "pinky"};
    return t >= 0 && t < kTipCount ? n[t] : "?";
}

namespace {
// The first built-in hand (2026-11-01): turned 27 deg off the barrel, web in
// the tang. Saved grips that still carry it get the new hand on load.
const FVector kOldKnuckle{6.2, 0.6, 7.1}, kOldFingers{0.93, 0.37, 0.0}, kOldAcross{0.225, 0.0, 0.974};
}  // namespace

void PistolHand(Record* r) {
    r->knuckle = {6.0, -0.6, 6.8};
    r->fingers = Normalize(FVector{0.999, 0.035, 0.0});
    r->across = Normalize(FVector{0.211, -0.077, 0.974});  // up the grip, 13 degree slant
}

bool HasOldPistolHand(const Record& r) {
    auto near = [](const FVector& a, const FVector& b) { return Length(a - b) < 0.02; };
    return near(r.knuckle, kOldKnuckle) && near(r.fingers, Normalize(kOldFingers)) &&
           near(r.across, Normalize(kOldAcross));
}

Record DefaultPistol() {
    Record r;
    r.name = "pistol";
    r.right_hand = true;
    // Middle knuckle on the front strap's right corner, one finger below the
    // trigger guard (high grip). The hand runs in line with the barrel (7 deg
    // off, measured on the avatar's hand) so a straight wrist puts the
    // forearm behind the gun as in the owner's photos (2026-11-02), with the
    // web of the hand centred low behind the back strap, under the tang.
    PistolHand(&r);
    r.tip[kThumb] = {9.5, -5.9, 11.0};       // forward along the left side of the frame, under the slide
    r.tip[kIndexOff] = {15.5, -1.0, 11.0};   // straight along the right side of the frame
    r.tip[kIndexRest] = {8.6, -4.0, 9.0};    // pad on the trigger face
    r.tip[kIndexPull] = {7.8, -4.0, 9.0};    // trigger pulled back
    r.tip[kMiddle] = {5.0, -5.9, 6.4};       // wrapped round the front strap to the left side
    r.tip[kRing] = {4.6, -5.9, 4.7};
    r.tip[kPinky] = {4.4, -5.7, 3.0};
    // Outline for the clip check. Grip from the 2026-11-01 ray scan (back
    // strap 1.2, front strap 6.8, sides -5.0/-1.9, bottom 1.8, slant 13);
    // frame, slide and guard bars estimated from the part positions.
    double t = std::tan(13.0 * kDegToRad), c = std::cos(13.0 * kDegToRad);
    double zc = (1.8 + 10.4) * 0.5;
    r.outline = {
        {"grip", {4.0 - (7.5 - zc) * t, -3.45, zc}, {5.6 * c * 0.5, 1.55, (10.4 - 1.8) * 0.5 / c}, 13.0},
        {"frame", {7.75, -3.45, 11.2}, {8.75, 1.55, 0.8}, 0.0},
        {"slide", {8.25, -3.45, 13.5}, {9.75, 1.75, 1.5}, 0.0},
        {"guard_bottom", {9.75, -3.45, 8.1}, {2.75, 0.8, 0.3}, 0.0},
        {"guard_front", {12.3, -3.45, 9.1}, {0.3, 0.8, 1.3}, 0.0},
        // Beavertail/tang over the web of the hand (behind the back strap).
        {"tang", {-1.3, -3.45, 10.8}, {0.9, 1.2, 0.5}, 0.0},
    };
    return r;
}

namespace {
void Put(std::string& o, const char* k, const FVector& v) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%s=%.2f %.2f %.2f\n", k, v.X, v.Y, v.Z);
    o += buf;
}
bool Vec(const std::string& s, FVector* v) {
    double x, y, z;
    if (std::sscanf(s.c_str(), "%lf %lf %lf", &x, &y, &z) != 3) return false;
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) return false;
    *v = {x, y, z};
    return true;
}
}  // namespace

std::string Serialize(const Record& r) {
    // Plain strings rather than string streams: a stream pulls about 0.5 MB of
    // the C++ library's locale code into the DLL for nothing.
    std::string o;
    o += "; HOLE VR grip record (written by the in-headset grip editor).\n"
         "; Object frame, cm: X forward (barrel), Y right, Z up.\n"
         "; knuckle = middle finger knuckle; fingers = wrist->knuckle direction;\n"
         "; across = pinky->index knuckle direction; tip_* = fingertip targets.\n";
    o += "name=" + r.name + "\n";
    o += std::string("hand=") + (r.right_hand ? "right" : "left") + "\n";
    Put(o, "knuckle", r.knuckle);
    Put(o, "fingers", r.fingers);
    Put(o, "across", r.across);
    for (int t = 0; t < kTipCount; ++t) Put(o, (std::string("tip_") + TipName(t)).c_str(), r.tip[t]);
    for (int t = 0; t < kTipCount; ++t) {
        if (!r.posed[t]) continue;
        char buf[400];
        int n = std::snprintf(buf, sizeof(buf), "pose_%s=", TipName(t));
        for (int j = 0; j < 3; ++j) {
            const FQuat& q = r.joint[t][j];
            n += std::snprintf(buf + n, sizeof(buf) - n, "%s%.5f %.5f %.5f %.5f", j ? "  " : "", q.X, q.Y, q.Z, q.W);
        }
        o += buf;
        o += "\n";
    }
    for (const OutlineBox& b : r.outline) {
        char buf[200];
        std::snprintf(buf, sizeof(buf), "box=%s %.2f %.2f %.2f %.2f %.2f %.2f %.1f\n", b.name.c_str(), b.centre.X,
                      b.centre.Y, b.centre.Z, b.half.X, b.half.Y, b.half.Z, b.pitch_deg);
        o += buf;
    }
    return o;
}

bool Parse(const std::string& text, Record* r) {
    std::string line;
    bool any = false, boxes = false;
    // One line at a time, as std::getline would split them.
    for (size_t pos = 0; pos < text.size();) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        line.assign(text, pos, end - pos);
        pos = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        FVector vec;
        if (k == "name") {
            r->name = v;
        } else if (k == "hand") {
            r->right_hand = v != "left";
        } else if (k == "knuckle" && Vec(v, &vec)) {
            r->knuckle = vec;
            any = true;
        } else if (k == "fingers" && Vec(v, &vec) && Length(vec) > 1e-3) {
            r->fingers = Normalize(vec);
        } else if (k == "across" && Vec(v, &vec) && Length(vec) > 1e-3) {
            r->across = Normalize(vec);
        } else if (k == "box") {
            char name[64];
            OutlineBox b;
            if (std::sscanf(v.c_str(), "%63s %lf %lf %lf %lf %lf %lf %lf", name, &b.centre.X, &b.centre.Y,
                            &b.centre.Z, &b.half.X, &b.half.Y, &b.half.Z, &b.pitch_deg) == 8) {
                if (!boxes) r->outline.clear();
                boxes = true;
                b.name = name;
                r->outline.push_back(b);
            }
        } else if (k.rfind("pose_", 0) == 0) {
            double q[12];
            if (std::sscanf(v.c_str(), "%lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf", &q[0], &q[1], &q[2], &q[3],
                            &q[4], &q[5], &q[6], &q[7], &q[8], &q[9], &q[10], &q[11]) == 12) {
                bool ok = true;
                for (double x : q) ok = ok && std::isfinite(x);
                for (int t = 0; ok && t < kTipCount; ++t)
                    if (k.substr(5) == TipName(t)) {
                        r->posed[t] = true;
                        for (int j = 0; j < 3; ++j)
                            r->joint[t][j] = Normalized(FQuat{q[j * 4], q[j * 4 + 1], q[j * 4 + 2], q[j * 4 + 3]});
                    }
            }
        } else if (k.rfind("tip_", 0) == 0 && Vec(v, &vec)) {
            for (int t = 0; t < kTipCount; ++t)
                if (k.substr(4) == TipName(t)) r->tip[t] = vec;
        }
    }
    return any;
}

Record Mirror(const Record& r) {
    auto m = [](const FVector& v) { return FVector{v.X, -v.Y, v.Z}; };
    Record o = r;
    o.right_hand = !r.right_hand;
    o.knuckle = m(r.knuckle);
    o.fingers = m(r.fingers);
    o.across = m(r.across);
    for (int t = 0; t < kTipCount; ++t) o.tip[t] = m(r.tip[t]);
    for (OutlineBox& b : o.outline) b.centre = m(b.centre);
    return o;
}

void HandInObject(const Record& r, const HandGeo& g, FQuat* rot, FVector* pos) {
    *rot = AlignFrames(g.across, g.fingers, r.across, r.fingers);
    *pos = r.knuckle - rot->Rotate(g.knuckle);
}

void StoreHand(Record* r, const HandGeo& g, const FQuat& rot, const FVector& pos) {
    r->knuckle = pos + rot.Rotate(g.knuckle);
    r->fingers = Normalize(rot.Rotate(g.fingers));
    r->across = Normalize(rot.Rotate(g.across));
}

double Depth(const Record& r, const FVector& p, std::string* box) {
    double worst = 0;
    for (const OutlineBox& b : r.outline) {
        double a = b.pitch_deg * kDegToRad;
        FVector ax{std::cos(a), 0, -std::sin(a)}, az{std::sin(a), 0, std::cos(a)};
        FVector d = p - b.centre;
        double lx = Dot(d, ax), ly = d.Y, lz = Dot(d, az);
        double ix = b.half.X - std::fabs(lx), iy = b.half.Y - std::fabs(ly), iz = b.half.Z - std::fabs(lz);
        if (ix <= 0 || iy <= 0 || iz <= 0) continue;
        double depth = std::min(ix, std::min(iy, iz));
        if (depth > worst) {
            worst = depth;
            if (box) *box = b.name;
        }
    }
    return worst;
}

std::string SideKey(const std::string& base, bool right_hand, bool usual_hand) {
    if (usual_hand) return base;
    return base + (right_hand ? "-right" : "-left");
}

std::string UsualKey(const std::string& key) {
    auto ends = [&](const std::string& tail) {
        return key.size() >= tail.size() && key.compare(key.size() - tail.size(), tail.size(), tail) == 0;
    };
    if (ends(".charging-left") || ends(".charging-right")) return key;
    if (ends("-left")) return key.substr(0, key.size() - 5);
    if (ends("-right")) return key.substr(0, key.size() - 6);
    return key;
}

}  // namespace holevr::vr::grip_record
