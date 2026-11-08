// Per-magazine magwell windows (vr/interact/magwell_zone.h) and the game's
// reload animation played by insert progress (vr/body/reload_anim.h).
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "vr/body/reload_anim.h"
#include "vr/interact/magwell_zone.h"

using namespace holevr::vr;
using namespace holevr::vr::interact;
using namespace holevr::vr::ik;

namespace {
int checks = 0;
void Check(bool ok, const char* what) {
    ++checks;
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        std::exit(1);
    }
}
bool Near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) < eps; }
double Turn(const FQuat& a, const FQuat& b) {
    const double d = std::fabs(a.X * b.X + a.Y * b.Y + a.Z * b.Z + a.W * b.W);
    return 2.0 * std::acos(d > 1.0 ? 1.0 : d);
}

// The 8 corners of a box, turned by q and moved to `at`.
std::vector<FVector> Corners(FVector lo, FVector hi, FQuat q = FQuat{}, FVector at = FVector{}) {
    std::vector<FVector> out;
    for (int k = 0; k < 8; ++k) {
        const FVector c{k & 1 ? hi.X : lo.X, k & 2 ? hi.Y : lo.Y, k & 4 ? hi.Z : lo.Z};
        out.push_back(at + q.Rotate(c));
    }
    return out;
}

// The owner's AR with its 10 round magazine, as the 2026-11-06 log measured
// them seated (gun frame from the grip, cm): magazine X 8.2..15.2 Y -3.4..0.1
// Z 0.4..10.6, lower receiver X -6.2..17.0 Z 1.0..12.9 (its Y is not in the
// log: around the magazine), handguard X 16.3..52.0 Z 8.7..15.5.
std::vector<PartBox> ArParts() {
    return {
        {"AR_Lower", {-6.2, -3.6, 1.0}, {17.0, 0.3, 12.9}},
        {"AR_Handguard", {16.3, -4.4, 8.7}, {52.0, 0.2, 15.5}},
        {"AR_Grip", {-7.0, -3.0, -9.0}, {-1.0, 0.0, 2.0}},       // pistol grip, behind the magwell
        {"AR_TriggerGuard", {-1.0, -3.0, -0.5}, {7.5, 0.0, 2.0}},  // in front of the grip, behind the magazine
        {"AR_Upper", {-6.0, -3.0, 12.9}, {18.0, 0.3, 17.0}},       // above the magazine
    };
}

void ArTenRound() {
    const FVector seat{11.7, -1.65, 0.4};  // its origin at the bottom of the magazine
    const auto mag = Corners({8.2, -3.4, 0.4}, {15.2, 0.1, 10.6});
    const MagwellZone z = MeasureMagwell(mag, seat, {0, 0, 1}, ArParts());
    Check(z.ok, "AR 10R: measured");
    Check(z.part == "AR_Lower", "AR 10R: the opening is the lower receiver's bottom");
    Check(Near(z.top, 10.2), "AR 10R: top 10.2 cm above its seat");
    Check(Near(z.bottom, 0.0), "AR 10R: bottom at its seat");
    Check(Near(z.opening, 0.6), "AR 10R: opening 0.6 cm above its seat");
    Check(Near(z.insert, 9.6), "AR 10R: goes 9.6 cm into the gun");
    // It lines up with its top an inch under the opening: 12.14 cm below
    // the seat, not the old 16 (which left its top ~5 cm under the gun).
    Check(Near(LineUpBelow(z, 2.54, 16.0), 12.14), "AR 10R: lines up 12.14 cm below its seat");
    Check(Near(SeatLast(z, 6.0), 4.8), "AR 10R: seats itself from half its travel at most");
    Check(Near(SeatLast(z, 1.0), 1.0), "the last centimetre: kept");

    // A 30 round magazine sticks out below the gun: same opening, longer travel only if its top is higher.
    const FVector seat30{11.7, -1.65, -14.0};
    const auto mag30 = Corners({8.2, -3.4, -14.0}, {15.2, 0.1, 10.6});
    const MagwellZone z30 = MeasureMagwell(mag30, seat30, {0, 0, 1}, ArParts());
    Check(z30.ok && z30.part == "AR_Lower", "AR 30R: measured against the lower receiver");
    Check(Near(z30.insert, 9.6), "AR 30R: the same 9.6 cm inside the gun");
    Check(Near(z30.top, 24.6) && Near(z30.bottom, 0.0), "AR 30R: its own top and bottom");
}

void PistolRaked() {
    // A pistol magazine raked 18 deg (its top towards the muzzle), 12 cm
    // long, seated with its base 1 cm below the frame.
    const double r = 18.0 * kDegToRad;
    const FQuat rake = AxisAngle(FVector{0, 1, 0}, r);  // turns +Z towards +X
    const FVector in = rake.Rotate(FVector{0, 0, 1});
    Check(Near(in.X, std::sin(r)) && Near(in.Z, std::cos(r)), "rake turns the magazine's axis forward");
    const FVector seat{2.0, 0.0, -9.0};
    const auto mag = Corners({-1.5, -1.2, 0.0}, {1.5, 1.2, 12.0}, rake, seat);
    const std::vector<PartBox> parts = {
        {"HG_Frame", {-4.0, -1.6, -8.0}, {9.0, 1.6, 3.0}},
        {"HG_Slide", {-6.0, -1.4, 3.0}, {14.0, 1.4, 6.5}},
    };
    const MagwellZone z = MeasureMagwell(mag, seat, in, parts);
    Check(z.ok && z.part == "HG_Frame", "pistol: the frame is the magwell");
    // The axis through the magazine's box centre meets the frame's bottom (Z -8).
    FVector lo = mag[0], hi = mag[0];
    for (const FVector& c : mag) {
        lo = {std::fmin(lo.X, c.X), std::fmin(lo.Y, c.Y), std::fmin(lo.Z, c.Z)};
        hi = {std::fmax(hi.X, c.X), std::fmax(hi.Y, c.Y), std::fmax(hi.Z, c.Z)};
    }
    const FVector centre = (lo + hi) * 0.5;
    const FVector q = centre + in * ((-8.0 - centre.Z) / in.Z);
    const double s = (q.X - seat.X) * in.X + (q.Y - seat.Y) * in.Y + (q.Z - seat.Z) * in.Z;
    Check(Near(z.opening, s, 1e-6), "pistol: opening where the raked axis meets the frame's bottom");
    Check(Near(z.insert, z.top - z.opening), "pistol: travel is top minus opening");
    Check(z.insert > 9.0 && z.insert < 12.0, "pistol: most of the magazine goes in");
}

void Edges() {
    const auto mag = Corners({-1, -1, 0}, {1, 1, 10});
    // Pointing down: not a magwell from below.
    Check(!MeasureMagwell(mag, {0, 0, 0}, {0, 0, -1}, {{"P", {-3, -3, 2}, {3, 3, 20}}}).ok, "axis must climb");
    // No parts beside it.
    const MagwellZone none = MeasureMagwell(mag, {0, 0, 0}, {0, 0, 1}, {});
    Check(!none.ok, "no parts: not measured");
    Check(Near(LineUpBelow(none, 2.54, 16.0), 16.0), "not measured: the old window");
    Check(Near(SeatLast(none, 1.0), 1.0), "not measured: the given self-seat point");
    Check(Near(SeatLast(none, -2.0), 0.0), "negative self-seat point clamps to 0");
    // A part ending above the magazine top is not its magwell.
    Check(!MeasureMagwell(mag, {0, 0, 0}, {0, 0, 1}, {{"Slide", {-3, -3, 9.5}, {3, 3, 14}}}).ok,
          "part above the magazine's top ignored");
    // A part beside the magazine but off its axis is not its magwell.
    Check(!MeasureMagwell(mag, {0, 0, 0}, {0, 0, 1}, {{"Rail", {5, -1, 2}, {20, 1, 9}}}).ok, "part off the axis ignored");
    // A part hanging far below the seated magazine's bottom (a frame box
    // that also takes in a grip extension): the opening is capped just under
    // the magazine's bottom.
    const MagwellZone deep = MeasureMagwell(mag, {0, 0, 0}, {0, 0, 1}, {{"Frame", {-3, -3, -6}, {3, 3, 8}}});
    Check(deep.ok && Near(deep.opening, -0.5) && Near(deep.insert, 10.5), "opening capped under the magazine");
    // Unit-less axis input is normalised.
    const MagwellZone scaled = MeasureMagwell(mag, {0, 0, 0}, {0, 0, 5}, {{"Frame", {-3, -3, 2}, {3, 3, 12}}});
    Check(scaled.ok && Near(scaled.insert, 8.0), "axis length does not matter");
    // A too-short travel is not trusted.
    Check(!MeasureMagwell(mag, {0, 0, 0}, {0, 0, 1}, {{"Frame", {-3, -3, 9}, {3, 3, 12}}}).ok, "travel under 1.5 cm");
}

void Progress() {
    // The owner's pistol 10 round magazine: 11.9 cm of travel, lines up an
    // inch further out (14.44), seats itself 1 cm from seated. The reload
    // plays out over the push in between (owner 2026-11-07: "by the time the
    // mag is fully inserted i want the animation to finish playing"); before,
    // it ran from the opening to seated and the magazine seated itself half
    // way, so the hand only got half way through it.
    MagwellZone z;
    z.ok = true;
    z.insert = 11.9;
    const double line_up = LineUpBelow(z, 2.54, 14.0), last = SeatLast(z, 1.0);
    Check(Near(line_up, 14.44) && Near(last, 1.0), "pistol 10R: lines up at 14.44, seats itself at 1");
    Check(Near(InsertProgress(line_up, line_up, last), 0.0), "lined up: 0");
    Check(Near(InsertProgress(20.0, line_up, last), 0.0), "further out: still 0");
    Check(Near(InsertProgress((line_up + last) / 2, line_up, last), 0.5), "half way: 0.5");
    Check(Near(InsertProgress(last, line_up, last), 1.0), "where it seats itself: 1 (played out)");
    Check(Near(InsertProgress(0.0, line_up, last), 1.0), "seated: 1");
    Check(Near(InsertProgress(-1.0, line_up, last), 1.0), "past the seat: 1");
    Check(Near(InsertProgress(5.0, 3.0, 3.0), 0.0) && Near(InsertProgress(2.0, 3.0, 3.0), 1.0), "no span: a step");
}

void Probe() {
    // The AR 10R: its origin 16.7 cm under its top, going straight up into
    // the magwell (out = down). Seated: origin at the seat, turned seat_rot.
    const FVector seat{3.0, -1.0, 2.0};
    const FQuat seat_rot = AxisAngle(FVector{0.2, 0.1, 1.0}, 0.9);  // any turn
    const FVector out{0, 0, -1};
    const double top = 16.7;
    TopProbe t = ProbeTop(seat, seat_rot, out, top, seat, seat_rot);
    Check(Near(t.below, 0.0) && Near(t.side, 0.0) && Near(t.tilt, 0.0), "seated: top in place");
    t = ProbeTop(seat, seat_rot, out, top, seat + FVector{0, 0, -12.14}, seat_rot);
    Check(Near(t.below, 12.14) && Near(t.side, 0.0) && Near(t.tilt, 0.0), "straight below: its top below by as much");
    t = ProbeTop(seat, seat_rot, out * 3.0, top, seat + FVector{2.0, 0, -11.0}, seat_rot);
    Check(Near(t.below, 11.0) && Near(t.side, 2.0), "beside the axis: its top beside it (out of any length)");
    // Leaning 30 deg about the origin, held 10 cm below: its top swings
    // 16.7 sin 30 = 8.35 cm to the side and 16.7 (1 - cos 30) = 2.24 cm
    // lower. The origin alone (10 below, 0 off) said it was lined up.
    const double r = 30.0 * kDegToRad;
    const FQuat lean = AxisAngle(FVector{0, 1, 0}, r);
    t = ProbeTop(seat, seat_rot, out, top, seat + FVector{0, 0, -10.0}, lean * seat_rot);
    Check(Near(t.tilt, 30.0, 1e-6), "leaning: its tilt");
    Check(Near(t.side, top * std::sin(r), 1e-6), "leaning: its top swings to the side");
    Check(Near(t.below, 10.0 + top * (1.0 - std::cos(r)), 1e-6), "leaning: its top drops");
    // A turn about its own axis does not move its top or lean it.
    const FQuat spin = AxisAngle(FVector{0, 0, 1}, 1.2);
    t = ProbeTop(seat, seat_rot, out, top, seat + FVector{0, 0, -5.0}, spin * seat_rot);
    Check(Near(t.below, 5.0) && Near(t.side, 0.0) && Near(t.tilt, 0.0), "turned about its axis: unchanged");
    // A raked pistol axis.
    const FVector raked = Normalize(FVector{-std::sin(24.0 * kDegToRad), 0, -std::cos(24.0 * kDegToRad)});
    t = ProbeTop(seat, seat_rot, raked, 11.8, seat + raked * 7.0, seat_rot);
    Check(Near(t.below, 7.0) && Near(t.side, 0.0), "along a raked axis");
}

void Animation() {
    namespace ra = reload_anim;
    ra::ClipData c;
    const FVector axis = Normalize(FVector{0.3, 1.0, 0.2});
    const FQuat base = AxisAngle(FVector{1, 0, 0}, 0.4);
    for (int i = 0; i < ra::kSamples; ++i)
        for (int f = 0; f < ra::kFingers; ++f)
            for (int j = 0; j < 3; ++j) c.pose[i].joint[f][j] = base * AxisAngle(axis, 0.1 * i * (j + 1));
    ra::Pose p;
    ra::At(c, 0.0, &p);
    Check(Turn(p.joint[2][1], c.pose[0].joint[2][1]) < 1e-6, "At(0) is the first pose");
    ra::At(c, 1.0, &p);
    Check(Turn(p.joint[2][1], c.pose[ra::kSamples - 1].joint[2][1]) < 1e-6, "At(1) is the last pose");
    ra::At(c, 2.0, &p);
    Check(Turn(p.joint[2][1], c.pose[ra::kSamples - 1].joint[2][1]) < 1e-6, "past 1 holds the last pose");
    ra::At(c, -1.0, &p);
    Check(Turn(p.joint[2][1], c.pose[0].joint[2][1]) < 1e-6, "below 0 holds the first pose");
    // Between samples: x = 0.5 * 11 = 5.5, half way from pose 5 to pose 6.
    ra::At(c, 0.5, &p);
    Check(Turn(p.joint[0][0], base * AxisAngle(axis, 0.55)) < 1e-6, "between samples: slerp");
    Check(Turn(p.joint[0][2], base * AxisAngle(axis, 0.55 * 3)) < 1e-6, "each joint its own");
    // Delta: the turn since the start, in the joint's own frame. Played on
    // top of the first pose it gives the animation's pose; the same delta
    // backwards when the magazine comes back out.
    ra::Pose d;
    ra::Delta(c, 0.5, &d);
    Check(Turn(d.joint[0][0], AxisAngle(axis, 0.55)) < 1e-6, "delta is base^-1 * pose");
    Check(Turn(c.pose[0].joint[4][1] * d.joint[4][1], p.joint[4][1]) < 1e-6, "first pose * delta = the pose");
    ra::Delta(c, 0.0, &d);
    Check(Turn(d.joint[1][1], FQuat{}) < 1e-6, "no delta at the start");
    // A posed hand gets the same motion on top of its own joint.
    const FQuat own = AxisAngle(FVector{0, 0, 1}, 0.7);
    ra::Delta(c, 1.0, &d);
    Check(Turn(own * d.joint[3][0], own * AxisAngle(axis, 1.1)) < 1e-6, "delta on a posed joint");
}

// Letting go of the magwell (owner 2026-11-07: "i have to push the mag to far
// up"; with the gun in the left hand it never went in). The run's pistol
// magazine: travel 11.6 cm, lines up 15.4 cm below, 3.5 cm and 35 deg.
void LetGo() {
    MagwellZone z;
    z.ok = true;
    z.top = 11.7;
    z.opening = 0.1;
    z.insert = 11.6;
    const double up = 15.4;
    // The run: top 5.0-5.4 cm off the axis 1.3-7.5 cm from seated let go
    // ("too far off the axis"). In the magwell it now stays on.
    Check(LetGoWhy(z, 1.3, up, 5.1, 12, 3.5, 35, 12) == nullptr, "in the magwell: 5 cm drift holds");
    Check(LetGoWhy(z, 7.5, up, 5.3, 47, 3.5, 35, 12) == nullptr, "in the magwell: lean holds");
    Check(LetGoWhy(z, 4.0, up, 12.5, 10, 3.5, 35, 12) != nullptr, "in the magwell: hand far off lets go");
    // Below the opening the old limits.
    Check(LetGoWhy(z, 13.0, up, 5.1, 10, 3.5, 35, 12) != nullptr, "below the opening: 5 cm drift lets go");
    Check(LetGoWhy(z, 13.0, up, 4.9, 10, 3.5, 35, 12) == nullptr, "below the opening: within 1.5 cm holds");
    Check(LetGoWhy(z, 13.0, up, 1.0, 46, 3.5, 35, 12) != nullptr, "below the opening: leaning lets go");
    // Pulled back out, wherever the hand is.
    Check(LetGoWhy(z, 18.5, up, 0.0, 0, 3.5, 35, 12) != nullptr, "pulled out lets go");
    Check(LetGoWhy(z, 18.3, up, 0.0, 0, 3.5, 35, 12) == nullptr, "not yet pulled out");
    // Not measured: no magwell to hold it.
    MagwellZone none;
    Check(LetGoWhy(none, 2.0, 14.0, 7.5, 10, 5.5, 40, 12) != nullptr, "not measured: drift lets go");
    // The pistol now seats itself 3.5 cm from seated (half its travel at most).
    Check(std::fabs(SeatLast(z, 3.5) - 3.5) < 1e-9, "pistol seats from 3.5 cm");
    Check(std::fabs(InsertProgress(3.5, up, SeatLast(z, 3.5)) - 1.0) < 1e-9, "animation done where it seats");
}
}  // namespace

int main() {
    ArTenRound();
    PistolRaked();
    Edges();
    Progress();
    Probe();
    LetGo();
    Animation();
    std::printf("magwell_zone: %d checks passed\n", checks);
    return 0;
}
