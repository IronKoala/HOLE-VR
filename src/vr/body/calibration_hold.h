#pragma once
// Button admission and feedback only. The Session's actual capture fraction
// owns progress; elapsed button time must never fake a completed measurement.
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace holevr::vr::calibration_hold {
struct Buttons {
    bool active=false, valid=false, left=false, right=false, interrupted=false;
    bool can_start=true; // a new capture starts on a fresh tracked observation
    std::uint64_t step=0; // phase + attempt; changes invalidate old admission
};
struct Admission { bool held=false, start=false, stop=false; };
struct Feedback { bool ramp=false, completed=false, stop=false; float amplitude=0; };
class Hold {
public:
    void Reset() { mode_=Mode::Release; have_step_=false; }
    bool NeedsRelease() const { return mode_==Mode::Release; }
    bool Holding() const { return mode_==Mode::Holding; }
    Admission Begin(const Buttons& b) {
        Admission out;
        if(!b.active||!b.valid||b.interrupted) {
            out.stop=mode_==Mode::Holding; mode_=Mode::Release;
            step_=b.step; have_step_=true; return out;
        }
        if(!have_step_||step_!=b.step) {
            out.stop=mode_==Mode::Holding; mode_=Mode::Release; step_=b.step; have_step_=true;
        }
        if(mode_==Mode::Holding && (!b.left||!b.right)) {
            out.stop=true; mode_=Mode::Release;
        }
        if(mode_==Mode::Release && !b.left&&!b.right) mode_=Mode::Ready;
        if(mode_==Mode::Ready && b.left&&b.right&&b.can_start) { mode_=Mode::Holding; out.start=true; }
        out.held=mode_==Mode::Holding;
        return out;
    }
    Feedback Observe(bool collecting,double fraction,bool accepted) {
        Feedback out;
        if(mode_!=Mode::Holding) return out;
        if(accepted) {
            mode_=Mode::Release; out.completed=true; out.amplitude=1; return out;
        }
        if(!collecting||!std::isfinite(fraction)) {
            mode_=Mode::Release; out.stop=true; return out;
        }
        out.ramp=true; out.amplitude=float(.9*std::clamp(fraction,0.0,1.0));
        return out;
    }
private:
    enum class Mode { Release,Ready,Holding };
    Mode mode_=Mode::Release;
    std::uint64_t step_=0;
    bool have_step_=false;
};
} // namespace holevr::vr::calibration_hold
