#include "vr/kick.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <initializer_list>

using namespace holevr::vr;
using namespace holevr::vr::kick;
namespace {
int checks = 0;
void Check(bool value, const char* text) {
    ++checks;
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", text); std::exit(1); }
}
double Length(FVector v) { return std::sqrt(v.X*v.X+v.Y*v.Y+v.Z*v.Z); }
bool Near(FVector a, FVector b, double eps=1e-7) { return Length(a-b) < eps; }
bool SameRotation(FQuat a, FQuat b) {
    return Near(a.Rotate({1,0,0}),b.Rotate({1,0,0})) && Near(a.Rotate({0,1,0}),b.Rotate({0,1,0}));
}
FVector Mirror(FVector p) { return {p.X,-p.Y,p.Z}; }
VisualLeg Transform(const VisualLeg& a, FQuat q, FVector p) {
    return {p+q.Rotate(a.hip),p+q.Rotate(a.knee),p+q.Rotate(a.ankle),q*a.foot};
}
}  // namespace
int main() {
    Check(MakePlan(YawOnly(64),{}).side == 1, "right aim chooses right leg");
    Check(MakePlan(YawOnly(-64),{}).side == 0, "left aim chooses left leg");
    Check(MakePlan(YawOnly(4),{}).side == 0, "centre keeps default left leg");
    Check(MakePlan(YawOnly(-175),YawOnly(170)).side == 1, "right selection wraps through -180");
    Check(MakePlan(YawOnly(175),YawOnly(-170)).side == 0, "left selection wraps through +180");
    Check(MakePlan(YawOnly(64),{},false).side == 0, "unavailable right adapter falls back to left");
    Check(!MakePlan({0,0,0,0},{}).valid, "invalid headset rotation rejected");
    for (double pitch : {-80.,-50.,-22.,0.,30.,70.}) {
        for (double yaw : {-177.,-64.,0.,71.,179.}) {
            const Plan p = MakePlan(ToQuat({pitch,yaw,23}),{});
            Check(p.valid, "finite head aim accepted");
            Check(p.aim.Pitch >= -50.00001 && p.aim.Pitch <= 30.00001 && p.aim.Roll == 0,
                  "bounded aim discards head roll");
            // This is the actual Blueprint expression: forward*160, Pitch30,
            // then control rotation. An uncompensated aim misses by 30 degrees.
            const FVector native = ToQuat(p.control).Rotate(ToQuat({30,0,0}).Rotate({160,0,0}));
            Check(Near(native,p.direction*160), "native sweep ends exactly along frozen aim");
        }
    }
    const FQuat left_ref = ToQuat({23,-31,17});
    const FQuat right_ref = ToQuat({-12,76,-41});  // intentionally different local axes
    const FQuat animation = ToQuat({47,13,-11});
    const VisualLeg left{{0,-10,95},{29,-13,61},{72,-6,72},animation*left_ref};
    const FVector right_hip{0,10,95};
    const FQuat native_frame = ToQuat({30,0,0});
    VisualLeg mirrored;
    Check(MapVisual(left,right_hip,true,{},native_frame,left_ref,right_ref,&mirrored), "mirror succeeds");
    Check(Near(mirrored.knee-right_hip,Mirror(left.knee-left.hip)), "right knee mirrors physical position");
    Check(Near(mirrored.ankle-right_hip,Mirror(left.ankle-left.hip)), "right ankle mirrors physical position");
    Check(std::fabs(Length(mirrored.knee-mirrored.hip)-Length(left.knee-left.hip)) < 1e-8,
          "mirroring preserves upper leg length");
    Check(std::fabs(Length(mirrored.ankle-mirrored.knee)-Length(left.ankle-left.knee)) < 1e-8,
          "mirroring preserves lower leg length");
    // Give each reference's local frame a different coordinate for the same
    // physical axis. Mirrored delta must act on those physical axes identically.
    for (FVector axis : {FVector{1,0,0},FVector{0,1,0},FVector{0,0,1}}) {
        const FVector right_local = right_ref.Inverse().Rotate(Mirror(axis));
        Check(Near(mirrored.foot.Rotate(right_local),Mirror(animation.Rotate(axis))),
              "mirrored foot respects different left and right bone reference axes");
    }
    VisualLeg support;
    Check(MapVisual(mirrored,left.hip,true,{},native_frame,right_ref,left_ref,&support), "support can mirror back");
    Check(Near(support.knee,left.knee) && Near(support.ankle,left.ankle) && SameRotation(support.foot,left.foot),
          "mirroring actual native support is reversible");

    const Plan aim = MakePlan(ToQuat({-12,71,0}),{});
    VisualLeg mapped;
    Check(MapVisual(left,right_hip,true,{},ToQuat(aim.aim),left_ref,right_ref,&mapped), "aim mapping succeeds");
    // A point on the native trace ray and the visible leg use the same pivot
    // and aim correction. Their endpoints agree, including rightward direction.
    VisualLeg ray = left;
    ray.ankle = left.hip+native_frame.Rotate({160,0,0});
    VisualLeg ray_out;
    Check(MapVisual(ray,right_hip,true,{},ToQuat(aim.aim),left_ref,right_ref,&ray_out), "trace ray maps");
    Check(Near(ray_out.ankle,right_hip+aim.direction*160), "visual and gameplay trace share hip and direction");

    for (double yaw : {-165.,33.,142.}) {
        const FQuat q = YawOnly(yaw);
        const FVector shift{132,-84,201};
        VisualLeg rotated;
        Check(MapVisual(Transform(left,q,shift),shift+q.Rotate(right_hip),true,q,q*ToQuat(aim.aim),
                        q*left_ref,q*right_ref,&rotated), "rotated scene maps");
        Check(Near(rotated.knee,shift+q.Rotate(mapped.knee)) && Near(rotated.ankle,shift+q.Rotate(mapped.ankle)) &&
              SameRotation(rotated.foot,q*mapped.foot), "mapping is covariant under arbitrary actor yaw and translation");
    }

    const VisualLeg standing{{0,10,95},{0,10,52},{0,10,8},right_ref};
    const VisualLeg standing_left{{0,-10,95},{0,-10,52},{0,-10,8},left_ref};
    VisualLeg blended;
    Check(MapVisual(standing_left,right_hip,true,{},ToQuat(aim.aim),left_ref,right_ref,&blended,0),
          "zero animation alpha accepted");
    Check(Near(blended.knee,standing.knee) && Near(blended.ankle,standing.ankle) && SameRotation(blended.foot,standing.foot),
          "kick starts and ends exactly at original leg pose without correction snap");
    Check(MapVisual(standing_left,right_hip,true,{},ToQuat(aim.aim),left_ref,right_ref,&blended,0.001),
          "tiny animation alpha accepted");
    Check(Length(blended.ankle-standing.ankle) < 0.2, "tiny alpha does not move standing foot dozens of cm");
    Check(MapVisual(left,right_hip,true,{},ToQuat(aim.aim),left_ref,right_ref,&blended,1) &&
          Near(blended.ankle,mapped.ankle) && SameRotation(blended.foot,mapped.foot),
          "full impact alpha reaches exact aimed pose");
    const FQuat qaim = ToQuat(aim.aim);
    Check(MapVisual(left,right_hip,true,{},FQuat{-qaim.X,-qaim.Y,-qaim.Z,-qaim.W},left_ref,right_ref,&blended,1) &&
          Near(blended.ankle,mapped.ankle), "equivalent opposite quaternion signs choose same correction arc");
    // The game has already blended its native left kick halfway. Redirect it
    // once, while the native right support transfers wholly to the left. A
    // second blend with the old left kick would incorrectly lift BOTH feet.
    VisualLeg native_half = left;
    native_half.knee = standing_left.knee*0.5+left.knee*0.5;
    native_half.ankle = standing_left.ankle*0.5+left.ankle*0.5;
    VisualLeg half_right, half_left;
    Check(MapVisual(native_half,right_hip,true,{},ToQuat(aim.aim),left_ref,right_ref,&half_right,0.5) &&
          MapVisual(standing,standing_left.hip,true,{},native_frame,right_ref,left_ref,&half_left,1),
          "half attack and live support both map");
    Check(half_right.ankle.Z > 15 && Near(half_left.ankle,standing_left.ankle),
          "right kick windup lifts only right leg; left remains actual native support");
    // Ball/toe nodes use the same delta mapping, but their own reference axes.
    // Moving only the foot would leave the native left toe's kick curl behind.
    const FQuat left_toe_ref = ToQuat({-8,35,16}), right_toe_ref = ToQuat({17,-21,-7});
    const FQuat curl = ToQuat({35,0,0});
    VisualLeg left_toe = left, right_toe = standing;
    left_toe.foot = curl*left_toe_ref;
    right_toe.foot = right_toe_ref;
    VisualLeg mapped_toe, support_toe;
    Check(MapVisual(left_toe,right_hip,true,{},native_frame,left_toe_ref,right_toe_ref,&mapped_toe),
          "toe swing maps using independent toe references");
    const FVector toe_axis{1,0,0};
    Check(Near(mapped_toe.foot.Rotate(right_toe_ref.Inverse().Rotate(Mirror(toe_axis))),
               Mirror(curl.Rotate(toe_axis))), "right toe receives native kick curl with its own bone axes");
    Check(MapVisual(right_toe,left.hip,true,{},native_frame,right_toe_ref,left_toe_ref,&support_toe) &&
          SameRotation(support_toe.foot,left_toe_ref), "left supporting toe retains no leftover kick curl");

    OriginOverride origin;
    FVector value = left.hip;
    Check(!origin.Apply(100,&value), "socket hook inert outside kick scope");
    origin.Arm(100,left.hip,mapped.hip);
    Check(!origin.Apply(200,&value) && origin.armed, "unrelated mesh cannot consume override");
    Check(origin.Apply(100,&value) && Near(value,mapped.hip) && origin.consumed,
          "expected native left hip is replaced with rendered selected hip");
    value = left.hip;
    Check(!origin.Apply(100,&value) && Near(value,left.hip), "downstream damage socket reads are untouched");
    origin.Arm(100,left.hip,mapped.hip);
    value = {77,88,99};
    Check(!origin.Apply(100,&value) && Near(value,{77,88,99}) && !origin.armed,
          "unexpected socket result safely closes scope without mutation");
    value = left.hip;
    Check(!origin.Apply(100,&value), "mismatched first socket cannot override a later read");
    origin.Arm(100,left.hip,mapped.hip);
    origin.Clear();
    Check(!origin.Apply(100,&value), "post trace cleanup closes scope");
    const double nan = std::numeric_limits<double>::quiet_NaN();
    origin.Arm(100,left.hip,{nan,0,0});
    Check(!origin.armed, "invalid replacement rejected");
    Check(!MapVisual(left,right_hip,true,{},ToQuat(aim.aim),left_ref,right_ref,&blended,nan), "invalid blend rejected");
    Check(!MapVisual(left,right_hip,true,{},FQuat{0,0,0,0},left_ref,right_ref,&mapped), "invalid aim rejected");
    std::printf("kick: %d checks passed\n",checks);
}
