#include "vr/body/calibration_hold.h"
#include "vr/body/body_calibration.h"
#include "vr/haptic_queue.h"
#include <cmath>
#include <cstdio>
#include <limits>
using namespace holevr::vr;
namespace hold=calibration_hold;
namespace bc=body_calibration;
static unsigned checks=0,failures=0;
#define EXPECT(x) do { ++checks; if(!(x)) { ++failures; std::printf("FAIL %d: %s\n",__LINE__,#x); } } while(0)
static hold::Buttons Buttons(bool left,bool right,bool valid=true,std::uint64_t step=1) {
    hold::Buttons b;b.active=true;b.valid=valid;b.left=left;b.right=right;b.step=step;return b;
}
static void AdmissionAndCompletion() {
    hold::Hold h;
    EXPECT(!h.Begin(Buttons(true,true)).held); // held before setup is not a new gesture
    EXPECT(!h.Begin(Buttons(false,true)).held);
    EXPECT(!h.Begin(Buttons(true,false)).held); // alternating releases never both-up
    EXPECT(!h.Begin(Buttons(false,false)).held && !h.NeedsRelease());
    auto b=Buttons(true,true);b.can_start=false;
    EXPECT(!h.Begin(b).start);b.can_start=true;
    auto a=h.Begin(b);EXPECT(a.start&&a.held);
    auto feedback=h.Observe(true,0,false);EXPECT(feedback.ramp&&feedback.amplitude==0&&!feedback.completed);
    for(double progress:{.1,.5,.75,1.0,2.0}) {
        feedback=h.Observe(true,progress,false);
        EXPECT(feedback.ramp && feedback.amplitude<=.9f && !feedback.completed);
    }
    feedback=h.Observe(false,0,true);EXPECT(feedback.completed&&feedback.amplitude==1&&h.NeedsRelease());
    EXPECT(!h.Observe(false,1,true).completed); // stale acceptance cannot double-pulse
    EXPECT(!h.Begin(Buttons(true,true,true,2)).held); // next phase held through
    EXPECT(!h.Begin(Buttons(false,true,true,2)).held);
    EXPECT(!h.Begin(Buttons(true,true,true,2)).held);
    h.Begin(Buttons(false,false,true,2));EXPECT(h.Begin(Buttons(true,true,true,2)).start);
    EXPECT(h.Begin(Buttons(false,true,true,2)).stop);
    EXPECT(!h.Observe(false,1,true).completed); // accepted flag after release loses admission
}
static void InterruptionsAndQueue() {
    for(int mode=0;mode<4;++mode) {
        hold::Hold h;h.Begin(Buttons(false,false));h.Begin(Buttons(true,true));h.Observe(true,.6,false);
        auto b=Buttons(true,true);
        if(mode==0)b.valid=false;
        if(mode==1)b.active=false;
        if(mode==2)b.interrupted=true; // coalesced release/repress, final raw levels still down
        if(mode==3)b.step=2; // retry changes attempt/phase; no old admission
        EXPECT(h.Begin(b).stop && h.NeedsRelease());
        EXPECT(!h.Begin(Buttons(true,true,true,b.step)).start);
        h.Begin(Buttons(false,false,true,b.step));EXPECT(h.Begin(Buttons(true,true,true,b.step)).start);
    }
    hold::Hold failed;failed.Begin(Buttons(false,false));failed.Begin(Buttons(true,true));
    EXPECT(failed.Observe(false,1,false).stop); // rejected fit or unsuccessful save
    EXPECT(!failed.Begin(Buttons(true,true)).held);
    hold::Hold invalid;invalid.Begin(Buttons(false,false));invalid.Begin(Buttons(true,true));
    EXPECT(invalid.Observe(true,std::numeric_limits<double>::quiet_NaN(),false).stop);
    std::vector<input::HapticRequest> queue={{0,.3f,.2f,0,false},{1,.7f,.1f,45,false},{0,.8f,.2f,0,false}};
    input::QueueHapticStop(queue,0);
    EXPECT(queue.size()==2 && queue[0].hand==1 && queue[0].amplitude==.7f && queue[0].frequency==45);
    EXPECT(queue[1].hand==0 && queue[1].stop);
    input::QueueHapticStop(queue,0);EXPECT(queue.size()==2 && queue[0].hand==1);
    queue.assign(16,{1,.5f,.1f,0,false});input::QueueHapticStop(queue,0);
    EXPECT(queue.size()==17 && queue.back().stop); // cancellation is never budget-dropped
    for(unsigned i=0;i<16;++i)EXPECT(queue[i].hand==1&&!queue[i].stop);
    input::QueueHapticStop(queue,1);EXPECT(queue.size()==2 && queue[0].hand==0&&queue[1].hand==1);
}
struct Driver {
    bc::Session session;hold::Hold gate;double time=0;unsigned pulses=0;
    hold::Feedback Tick(double seconds,bool left,bool right,bool valid=true,bool skip=false,bool saved=true,bool interrupted=false) {
        time=seconds;const auto before=session.GetStatus();auto b=Buttons(left,right,valid,(std::uint64_t(before.attempt)<<8)|unsigned(before.phase));
        b.interrupted=interrupted;auto admission=gate.Begin(b);
        bc::Sample s;s.seconds=seconds;s.valid=s.head_valid=s.grip_orientation_valid=valid;
        s.head={0,0,170};s.head_rotation={};s.wrist[0]={54,-20,145};s.wrist[1]={61,21,146};
        s.floor_known=true;s.height_mode=bc::HeightMode::Real;s.height_posture=bc::HeightPosture::Standing;
        s.confirm_pose=admission.start;s.capture_held=admission.held;s.skip_arms=skip;
        auto status=session.Update(s);
        auto feedback=gate.Observe(status.pose_confirmed,status.hold_fraction,status.capture_accepted&&saved);
        if(feedback.completed)++pulses;
        EXPECT(!feedback.ramp || feedback.amplitude<=.9f);
        return feedback;
    }
};
static void ActualSessionContract() {
    for(double hz:{5.,7.,45.,90.}) {
        Driver d;const double dt=1/hz;d.Tick(0,false,false);auto f=d.Tick(dt,true,true);
        EXPECT(f.ramp&&f.amplitude==0);const double start=d.time;
        while(d.session.GetStatus().phase==bc::Phase::MeasureHeight && d.time-start<3) {
            f=d.Tick(d.time+dt,true,true);
            if(d.time-start<2-1e-8)EXPECT(!f.completed);
        }
        EXPECT(d.pulses==1 && d.session.GetStatus().phase==bc::Phase::ArmsForward);
        EXPECT(d.time-start>=2-1e-8); // no1.5s completion before2s capture ends
        for(int i=0;i<30;++i)d.Tick(d.time+dt,true,true);
        EXPECT(d.pulses==1 && !d.session.GetStatus().pose_confirmed);
        d.Tick(d.time+dt,false,false);d.Tick(d.time+dt,true,true);
        EXPECT(d.session.GetStatus().pose_confirmed);
        const double same=d.time;d.Tick(same,false,true); // release on duplicate tracking timestamp
        EXPECT(!d.session.GetStatus().pose_confirmed && d.session.GetStatus().hold_fraction==0);
        d.Tick(same+dt,true,true);EXPECT(!d.session.GetStatus().pose_confirmed);
        d.Tick(same+2*dt,false,false);d.Tick(same+3*dt,true,true);
        EXPECT(d.session.GetStatus().pose_confirmed);
        d.Tick(d.time+dt,true,true,false);d.Tick(d.time+dt,true,true,true);
        EXPECT(!d.session.GetStatus().pose_confirmed && d.pulses==1);
    }
    for(bool save:{false,true}) {
        Driver d;d.Tick(0,false,false,true,true); // X chooses height only, does not confirm it
        EXPECT(!d.session.GetStatus().pose_confirmed && d.pulses==0);
        d.Tick(.1,false,false);d.Tick(.2,true,true);
        for(int i=0;i<22 && d.session.GetStatus().phase!=bc::Phase::Complete;++i)d.Tick(d.time+.1,true,true,true,false,save);
        EXPECT(d.session.GetStatus().phase==bc::Phase::Complete);
        EXPECT(d.pulses==(save?1u:0u)); // final acknowledgment requires persisted result
    }
}
int main() {
    AdmissionAndCompletion();InterruptionsAndQueue();ActualSessionContract();
    std::printf("calibration hold: %u checks, %u failures\n",checks,failures);return failures?1:0;
}
