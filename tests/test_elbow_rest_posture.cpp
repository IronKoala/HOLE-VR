// Body-near elbow style regression. Hands do not provide elbow ground truth;
// these are preference and continuity checks, not anatomical measurements.
#include "vr/body/arm_posture.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
using namespace holevr::vr; using namespace holevr::vr::ik; using namespace holevr::vr::arm_posture;
static unsigned checks=0,failures=0;
#define EXPECT(c) do { ++checks; if(!(c)){++failures;std::printf("FAIL %d: %s\n",__LINE__,#c);} } while(0)
static bool Near(FVector a,FVector b,double e=1e-6){return Length(a-b)<e;}
static FVector Mirror(FVector v){return {v.X,-v.Y,v.Z};}
static skel::Xf At(FVector p){skel::Xf x;x.translation=p;return x;}
// Local coordinates: forward, outward from this shoulder, up.
// These are rest-style checks with synthetic hand frames (knuckles forward,
// palm in), not real hands: the human joint ranges would read such a hand as
// evidence about the elbow. test_arm_posture covers the ranges, including
// that a hand in line with the rest forearm leaves the rest posture alone.
static Input Pose(int side,FVector local,double scale=1){
 const double sign=side==0?-1:1;Input in;in.joint_limits=false;in.shoulder={0,sign*15*scale,0};
 in.wrist=in.shoulder+FVector{local.X,sign*local.Y,local.Z}*scale;in.upper_cm=30*scale;in.forearm_cm=28*scale;
 in.forward={1,0,0};in.outward={0,sign,0};in.down={0,0,-1};in.dt=1./90;
 in.reference=arm_pose::MeasureReference(At(in.shoulder),At(in.shoulder+in.outward*in.upper_cm),At(in.shoulder+in.outward*(in.upper_cm+in.forearm_cm)),in.down);
 in.forearm_axis_hand=in.outward;in.palm_axis_hand={0,0,-1};
 in.hand_rotation=AlignFrames(in.forearm_axis_hand,in.palm_axis_hand,{1,0,0},in.outward*-1);in.torso_center_valid=true;return in;
}
static void Geometry(const Input& in,const Result& r){
 EXPECT(r.valid&&r.rotations.valid);if(!r.valid)return;EXPECT(Near(r.solved.end,in.wrist,1e-5));
 EXPECT(std::fabs(Length(r.solved.mid-in.shoulder)-in.upper_cm)<1e-5);
 EXPECT(std::fabs(Length(r.solved.end-r.solved.mid)-in.forearm_cm)<1e-5);
 EXPECT(Near(r.rotations.upper.Rotate(in.reference.upper_axis),Normalize(r.solved.mid-in.shoulder)));
 EXPECT(Near(r.rotations.forearm.Rotate(in.reference.forearm_axis),Normalize(r.solved.end-r.solved.mid)));
 EXPECT(std::isfinite(r.cost)&&r.candidates>0&&r.candidates<=64);
}
static double Flare(const Input& in,const Result& r){return Dot(r.solved.mid-in.shoulder,in.outward);}
static Result Settle(Input in){State state;Result r;for(int i=0;i<270;++i)r=Solve(in,&state);return r;}
static void ForwardAndHanging(){
 for(double scale:{.75,1.,1.25})for(double f:{30.,40.}){Result both[2];for(int side=0;side<2;++side){
  const auto in=Pose(side,{f,0,0},scale);const auto r=Settle(in);both[side]=r;Geometry(in,r);
  // Old style flared 12-14 cm with 30/28 cm limbs despite a feasible tuck.
  EXPECT(Flare(in,r)>=-1e-6&&Flare(in,r)<6*scale);EXPECT(Dot(r.solved.mid-in.shoulder,in.down)>8*scale);
  if(scale==1&&side==1)std::printf("forward %.0f: flare %.3f\n",f,Flare(in,r));
 }EXPECT(Near(both[0].solved.mid,Mirror(both[1].solved.mid),1e-5));}
 Result hanging[2];
 for(int side=0;side<2;++side){const auto in=Pose(side,{10,0,-40});const auto r=Settle(in);hanging[side]=r;Geometry(in,r);
  // The low-hand triangle needs elbow room. A smaller lateral number is not
  // an improvement if it rotates the elbow further behind the player's back.
  EXPECT(Flare(in,r)>=-1e-6&&Flare(in,r)<13.5);EXPECT(Dot(r.solved.mid-in.shoulder,in.forward)>-13);
  if(side==1)std::printf("hanging: flare %.3f, forward %.3f\n",Flare(in,r),Dot(r.solved.mid-in.shoulder,in.forward));}
 EXPECT(Near(hanging[0].solved.mid,Mirror(hanging[1].solved.mid),1e-5));
}
static double FurthestOutFeasible(const Input& in){
 const FVector axis=Normalize(in.wrist-in.shoulder);const auto seed=SolveTwoBone(in.shoulder,in.wrist,in.upper_cm,in.forearm_cm,in.down);
 const FVector along=axis*Dot(seed.mid-in.shoulder,axis);const double radius=Length(seed.mid-in.shoulder-along);
 return Dot(along,in.outward)+radius*Length(Reject(in.outward,axis));
}
static void InwardHands(){for(int side=0;side<2;++side){
 auto in=Pose(side,{25,-25,0});auto r=Settle(in);Geometry(in,r);EXPECT(Flare(in,r)>-4.5);
 if(side==1)std::printf("moderate inward: flare %.3f feasible maximum %.3f\n",Flare(in,r),FurthestOutFeasible(in));
 in=Pose(side,{15,-35,0});r=Settle(in);Geometry(in,r);const double limit=FurthestOutFeasible(in);EXPECT(limit<0);
 // This target genuinely requires a medial elbow: compare with feasible
 // geometry rather than imposing an impossible own-shoulder boundary.
 EXPECT(Flare(in,r)>limit-2.5);
 if(side==1)std::printf("central: flare %.3f feasible maximum %.3f\n",Flare(in,r),limit);
}}
static void RecordedLowHandGeometry(){
 struct Case{int side;FVector shoulder,wrist;FQuat hand;double upper,forearm,old_flare,old_forward;};
 // Nov 4 18:26:02/07 empty-hand positions. No elbow tracker or pose marker
 // supplied ground truth. Synthetic bind frame validates geometry/style only;
 // these cases do not assert the player's actual wrist angle or elbow position.
 const Case cases[]={
 {1,{-15.13,12.85,-18.22},{8.90,4.84,-30.94},{.5092,.0158,.5924,.6241},32.44,28.84,21.98,-11.89},
 {1,{-15.13,12.85,-18.22},{6.66,4.40,-29.46},{.5109,.0198,.5931,.6220},32.44,28.84,23.72,-11.84},
 {0,{-11.43,-15.77,-19.75},{-6.87,-3.96,-32.50},{-.7965,-.3418,.4431,.2290},31.78,28.26,25.16,-12.66}};
 for(const auto& c:cases){auto in=Pose(c.side,{0,0,0});in.shoulder=c.shoulder;in.wrist=c.shoulder+c.wrist;
  in.upper_cm=c.upper;in.forearm_cm=c.forearm;in.hand_rotation=Normalized(c.hand);const auto r=Settle(in);Geometry(in,r);
  // Do not exchange the old low-hand flare for a large posterior move.
  EXPECT(Flare(in,r)<c.old_flare+2.5);EXPECT(Dot(r.solved.mid-in.shoulder,in.forward)>c.old_forward-2.5);
  EXPECT(r.torso_clearance_cm>=3-1e-5);
  auto mirrored=Pose(1-c.side,{0,0,0});mirrored.shoulder=Mirror(in.shoulder);mirrored.wrist=Mirror(in.wrist);
  mirrored.upper_cm=in.upper_cm;mirrored.forearm_cm=in.forearm_cm;
  mirrored.hand_rotation=AlignFrames(mirrored.forearm_axis_hand,mirrored.palm_axis_hand,
      Mirror(in.hand_rotation.Rotate(in.forearm_axis_hand)),Mirror(in.hand_rotation.Rotate(in.palm_axis_hand)));
  const auto other=Settle(mirrored);Geometry(mirrored,other);EXPECT(Near(other.solved.mid,Mirror(r.solved.mid),1e-5));
  std::printf("recorded side%d: flare %.3f (old %.3f), forward %.3f (old %.3f)\n",c.side,Flare(in,r),c.old_flare,Dot(r.solved.mid-in.shoulder,in.forward),c.old_forward);}
}
static void RequestedReachesRemainPossible(){for(int side=0;side<2;++side)for(FVector p:{FVector{0,50,0},FVector{8,7,53},FVector{5,8,-52},FVector{20,-45,0},FVector{0,-55,0}}){
 const auto in=Pose(side,p);const auto r=Settle(in);Geometry(in,r);if(p.Y>45)EXPECT(Flare(in,r)>15);
}}
static void WristMotionDoesNotRelocateFreeElbows(){for(int side=0;side<2;++side)for(FVector p:{FVector{30,0,0},FVector{10,0,-40},FVector{25,-25,0}}){
 const auto fixed=Pose(side,p); // with joint limits: test_arm_posture JointLimitsMoveElbow
 const auto base=Solve(fixed,nullptr);for(FVector axis:{FVector{1,0,0},FVector{0,1,0},FVector{0,0,1}}){State a,b;
  for(int angle=-180;angle<=180;angle+=5){auto moved=fixed;moved.hand_rotation=Normalized(AxisAngle(axis,angle*kDegToRad)*fixed.hand_rotation);
   EXPECT(Near(Solve(moved,nullptr).solved.mid,base.solved.mid));const auto live=Solve(moved,&a),still=Solve(fixed,&b);EXPECT(Near(live.solved.mid,still.solved.mid));}
 }
}}
static void AuthoredSupportCanRelieveWrist(){
 // A saved support grip supplies an authored hand frame, unlike a freely
 // moving controller. Existing soft wrist relief may influence its elbow,
 // but still preserves the grip target, limb lengths and torso guard.
 for(int side=0;side<2;++side){
  auto in=Pose(side,{35,0,-5});const auto target_pole=Normalize(in.outward+in.down*.4);
  const auto target=SolveTwoBone(in.shoulder,in.wrist,in.upper_cm,in.forearm_cm,target_pole);
  const auto frames=arm_pose::Orient(in.reference,in.shoulder,target,target_pole);
  in.hand_rotation=Normalized(frames.forearm*in.reference.forearm_to_hand);
  const auto plain=Settle(in);in.wrist_relief=true;const auto relief=Settle(in);Geometry(in,relief);
  const auto bend=[](const Result& r){return r.flex*r.flex+2*r.deviation*r.deviation;};
  EXPECT(bend(relief)<bend(plain)-.01);EXPECT(relief.torso_clearance_cm>=3-1e-5);
  EXPECT(Flare(in,relief)<Flare(in,plain)+15);
 }
}
static void BodyRotationAndContinuity(){const auto yaw=YawOnly(137);
 for(int side=0;side<2;++side)for(FVector p:{FVector{30,0,0},FVector{10,0,-40},FVector{25,-25,0}}){const auto in=Pose(side,p);const auto a=Settle(in);auto spun=in;const FVector shift{100,500,-80};
  spun.shoulder=yaw.Rotate(in.shoulder)+shift;spun.wrist=yaw.Rotate(in.wrist)+shift;spun.torso_center=yaw.Rotate(in.torso_center)+shift;
  spun.forward=yaw.Rotate(in.forward);spun.outward=yaw.Rotate(in.outward);spun.down=yaw.Rotate(in.down);spun.hand_rotation=yaw*in.hand_rotation;
  const auto b=Settle(spun);Geometry(spun,b);EXPECT(Near(b.solved.mid,yaw.Rotate(a.solved.mid)+shift,1e-5));}
 for(double hz:{7.,90.,144.})for(int side=0;side<2;++side){auto in=Pose(side,{58,0,0});in.dt=1/hz;State state;Solve(in,&state);
  for(int n=0;n<int(2*hz);++n){const auto previous=state;const auto axis=Normalize(FVector{1,.003*std::sin(n*.7),.003*std::cos(n*.9)});
   in.wrist=in.shoulder+axis*(58*(.992+.006*std::sin(n*.6)));const auto r=Solve(in,&state);Geometry(in,r);
   EXPECT(state.extension_locked);EXPECT(Near(r.pole,Arc(previous.axis,axis).Rotate(previous.pole)));}
  for(int n=1;n<=int(2*hz);++n){const auto previous=state;in.wrist=in.shoulder+FVector{58*(1-.35*std::min(n/hz,1.)),0,0};
   const auto r=Solve(in,&state);Geometry(in,r);EXPECT(AngleBetween(Arc(previous.axis,state.axis).Rotate(previous.pole),r.pole)<=360*kDegToRad/hz+1e-6);}
 }
}
static constexpr double kBoundaryFractions[3][2]={{.15,.70},{.35,.85},{.15,.40}};
static void PreferenceBoundaryJitter(){
 // Small tracking variation near the transition to side/overhead reaching
 // must not turn a soft rest preference into a new abrupt elbow plane.
 for(double hz:{7.,90.,144.})for(int side=0;side<2;++side)for(int direction=0;direction<3;++direction)
  // A named table: the arrays behind a ?: of initializer_lists end with the
  // range expression (GCC reads freed stack here; clang happened not to).
  for(double fraction:kBoundaryFractions[direction]){
   auto in=Pose(side,{30,0,0});in.dt=1/hz;State state;
   for(int n=0;n<240;++n){
    const double component=58*(fraction+.001*std::sin(n*.39));
    const double forward=std::sqrt(std::pow(58*.96,2)-component*component);
    in.wrist=in.shoulder+in.forward*forward+(direction==0?in.outward:direction==1?in.down*-1:in.down)*component;
    const auto previous=state;const auto r=Solve(in,&state);Geometry(in,r);
    if(previous.valid)EXPECT(AngleBetween(Arc(previous.axis,state.axis).Rotate(previous.pole),r.pole)<=360*kDegToRad/hz+1e-6);
   }
  }
}
int main(){ForwardAndHanging();InwardHands();RecordedLowHandGeometry();RequestedReachesRemainPossible();WristMotionDoesNotRelocateFreeElbows();AuthoredSupportCanRelieveWrist();BodyRotationAndContinuity();PreferenceBoundaryJitter();
 std::printf("elbow rest posture: %u checks, %u failures\n",checks,failures);return failures?1:0;}
