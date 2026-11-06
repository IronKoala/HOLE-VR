// Windows native integration test: actual input.cpp with a fake OpenXR runtime.
// Checks measurement freshness/validity boundaries without touching a headset.
// clang++ -std=c++20 -DNOMINMAX -DWIN32_LEAN_AND_MEAN -D_CRT_SECURE_NO_WARNINGS -Isrc -Ithird_party/openxr/include tests/test_calibration_tracking.cpp src/vr/input.cpp -o test_calibration_tracking
#include <cstdio>
#include <cstring>
#include <map>
#include <string>

#include "vr/input.h"
#include "vr/openxr.h"
#include "core/perf.h"

using namespace holevr::vr;
static int failures=0;
#define EXPECT(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n",__LINE__,#c); ++failures; } } while(0)
static xr::Listener* listener=nullptr;
static constexpr XrSpaceLocationFlags tracked=
    XR_SPACE_LOCATION_POSITION_VALID_BIT|XR_SPACE_LOCATION_ORIENTATION_VALID_BIT|
    XR_SPACE_LOCATION_POSITION_TRACKED_BIT|XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
static constexpr XrViewStateFlags views_tracked=
    XR_VIEW_STATE_POSITION_VALID_BIT|XR_VIEW_STATE_ORIENTATION_VALID_BIT|
    XR_VIEW_STATE_POSITION_TRACKED_BIT|XR_VIEW_STATE_ORIENTATION_TRACKED_BIT;
static XrSpaceLocationFlags hand_flags[2]={tracked,tracked};
static XrViewStateFlags view_flags=views_tracked;
static bool hand_active[2]={true,true};
static XrResult sync_result=XR_SUCCESS, head_result=XR_SUCCESS, hand_result=XR_SUCCESS;
static unsigned head_calls=0;
static uint32_t view_count=2;
static XrTime frame_time=0, head_time=0;
static int toggle_during_head=0;
static std::uintptr_t next_handle=10;
static XrAction grip_action=XR_NULL_HANDLE;
struct Space { int side; bool grip; };
static std::map<XrSpace,Space> spaces;

static XrResult XRAPI_CALL CreateSet(XrInstance,const XrActionSetCreateInfo*,XrActionSet* out) {
    *out=reinterpret_cast<XrActionSet>(++next_handle); return XR_SUCCESS;
}
static XrResult XRAPI_CALL StringPath(XrInstance,const char* path,XrPath* out) {
    *out=std::strcmp(path,"/user/hand/left")==0?1:std::strcmp(path,"/user/hand/right")==0?2:100;
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL CreateAction(XrActionSet,const XrActionCreateInfo* info,XrAction* out) {
    *out=reinterpret_cast<XrAction>(++next_handle);
    if(std::strcmp(info->actionName,"grip_pose")==0) grip_action=*out;
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL Suggest(XrInstance,const XrInteractionProfileSuggestedBinding*) { return XR_SUCCESS; }
static XrResult XRAPI_CALL CreateSpace(XrSession,const XrActionSpaceCreateInfo* info,XrSpace* out) {
    *out=reinterpret_cast<XrSpace>(++next_handle);
    spaces[*out]={info->subactionPath==1?0:1,info->action==grip_action}; return XR_SUCCESS;
}
static XrResult XRAPI_CALL Sync(XrSession,const XrActionsSyncInfo*) { return sync_result; }
static XrResult XRAPI_CALL Float(XrSession,const XrActionStateGetInfo*,XrActionStateFloat* out) {
    out->isActive=XR_TRUE; out->currentState=0; return XR_SUCCESS;
}
static XrResult XRAPI_CALL Bool(XrSession,const XrActionStateGetInfo*,XrActionStateBoolean* out) {
    out->isActive=XR_TRUE; out->currentState=XR_FALSE; return XR_SUCCESS;
}
static XrResult XRAPI_CALL Vector(XrSession,const XrActionStateGetInfo*,XrActionStateVector2f* out) {
    out->isActive=XR_TRUE; out->currentState={0,0}; return XR_SUCCESS;
}
static XrResult XRAPI_CALL Pose(XrSession,const XrActionStateGetInfo* info,XrActionStatePose* out) {
    out->isActive=hand_active[info->subactionPath==1?0:1]?XR_TRUE:XR_FALSE; return XR_SUCCESS;
}
static XrResult XRAPI_CALL Locate(XrSpace space,XrSpace,XrTime time,XrSpaceLocation* out) {
    if(hand_result!=XR_SUCCESS) return hand_result;
    const int side=spaces[space].side;
    out->locationFlags=hand_flags[side]; out->pose.orientation={0,0,0,1};
    out->pose.position={side==0?-.3f:.3f,1.5f,float(double(time-frame_time)/1e9)};
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL Views(XrSession,const XrViewLocateInfo* info,XrViewState* state,
                                 uint32_t,uint32_t* count,XrView* out) {
    ++head_calls; head_time=info->displayTime;
    if(toggle_during_head) {
        input::SetCalibrationCapture(false);
        if(toggle_during_head==2) input::SetCalibrationCapture(true);
        toggle_during_head=0;
    }
    if(head_result!=XR_SUCCESS) return head_result;
    *count=view_count; state->viewStateFlags=view_flags;
    for(int i=0;i<2;++i) { out[i].pose.orientation={0,0,0,1}; out[i].pose.position={i==0?-.032f:.032f,1.7f,0}; }
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL Haptic(XrSession,const XrHapticActionInfo*,const XrHapticBaseHeader*) { return XR_SUCCESS; }
namespace holevr::vr::xr {
void RegisterListener(Listener* l) { listener=l; }
PFN_xrVoidFunction GetProc(const char* name) {
#define PROC(n,f) if(std::strcmp(name,n)==0) return reinterpret_cast<PFN_xrVoidFunction>(f)
    PROC("xrCreateActionSet",CreateSet); PROC("xrStringToPath",StringPath); PROC("xrCreateAction",CreateAction);
    PROC("xrSuggestInteractionProfileBindings",Suggest); PROC("xrCreateActionSpace",CreateSpace); PROC("xrSyncActions",Sync);
    PROC("xrGetActionStateFloat",Float); PROC("xrGetActionStateBoolean",Bool); PROC("xrGetActionStateVector2f",Vector);
    PROC("xrGetActionStatePose",Pose); PROC("xrLocateSpace",Locate); PROC("xrLocateViews",Views); PROC("xrApplyHapticFeedback",Haptic);
#undef PROC
    return nullptr;
}
XrDuration PredictAhead() { return 20000000; }
void PictureFrames(int* lo,int* hi,XrDuration* period) { *lo=0; *hi=0; *period=11111111; }
XrTime NewPictureShownTime() { return frame_time; }
}
namespace holevr::log {
void Info(const char*,...) {} void Warn(const char*,...) {} void Error(const char*,...) {}
}
namespace holevr::perf { void AddReportLine(ReportFn) {} }
static void Frame() {
    frame_time+=11111111;
    listener->OnFrame(reinterpret_cast<XrSession>(2),reinterpret_cast<XrSpace>(3),frame_time);
}
int main() {
    input::Init(); EXPECT(listener!=nullptr);
    listener->OnInstanceCreated(reinterpret_cast<XrInstance>(1));
    std::vector<XrActionSet> sets;
    listener->OnSessionCreated(reinterpret_cast<XrSession>(2),sets);
    Frame(); EXPECT(head_calls==0); EXPECT(!input::CalibrationSample().valid);
    EXPECT(!input::CalibrationSample().head_valid);

    input::SetCalibrationCapture(true); EXPECT(!input::CalibrationSample().valid);
    EXPECT(!input::CalibrationSample().head_valid);
    Frame(); auto sample=input::CalibrationSample();
    EXPECT(sample.valid && sample.head_valid); EXPECT(head_calls==1);
    EXPECT(sample.time==frame_time && head_time==frame_time);
    EXPECT(sample.head.position.x==0 && sample.head.position.y==1.7f);
    EXPECT(sample.grip[0].position.z==0 && sample.grip[1].position.z==0); // not +20ms game prediction

    // Valid-but-untracked hands remain usable by gameplay, never arm fitting.
    // Head-only height measurement continues at the current base timestamp.
    hand_flags[0]=tracked&~XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
    Frame(); sample=input::CalibrationSample();
    EXPECT(!sample.valid && sample.head_valid); EXPECT(head_calls==2);
    EXPECT(sample.time==frame_time && head_time==frame_time);
    EXPECT(input::Peek().hand[0].grip_valid);
    hand_flags[0]=tracked;
    hand_flags[1]=tracked&~XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
    Frame(); EXPECT(!input::CalibrationSample().valid && input::CalibrationSample().head_valid);
    EXPECT(input::Peek().hand[1].grip_valid);
    hand_flags[1]=tracked;
    hand_active[0]=false;
    Frame(); EXPECT(!input::CalibrationSample().valid && input::CalibrationSample().head_valid);
    hand_active[1]=false; hand_flags[0]=hand_flags[1]=0;
    Frame(); sample=input::CalibrationSample();
    EXPECT(!sample.valid && sample.head_valid); // both controllers resting/occluded
    EXPECT(sample.time==frame_time && head_time==frame_time);
    EXPECT(sample.head.position.y==1.7f);
    hand_active[0]=hand_active[1]=true; hand_flags[0]=hand_flags[1]=tracked;

    view_flags=views_tracked&~XR_VIEW_STATE_POSITION_TRACKED_BIT;
    Frame(); EXPECT(!input::CalibrationSample().valid && !input::CalibrationSample().head_valid);
    view_flags=views_tracked&~XR_VIEW_STATE_POSITION_VALID_BIT;
    Frame(); EXPECT(!input::CalibrationSample().valid && !input::CalibrationSample().head_valid);
    view_flags=views_tracked; view_count=1;
    Frame(); EXPECT(!input::CalibrationSample().valid && !input::CalibrationSample().head_valid); view_count=2;
    head_result=XR_ERROR_RUNTIME_FAILURE;
    Frame(); EXPECT(!input::CalibrationSample().valid && !input::CalibrationSample().head_valid); head_result=XR_SUCCESS;
    Frame(); EXPECT(input::CalibrationSample().valid && input::CalibrationSample().head_valid);

    sync_result=XR_SESSION_NOT_FOCUSED;
    const auto before=head_calls; Frame();
    EXPECT(!input::CalibrationSample().valid && !input::CalibrationSample().head_valid); EXPECT(head_calls==before);
    sync_result=XR_ERROR_RUNTIME_FAILURE;
    Frame(); EXPECT(!input::CalibrationSample().valid && !input::CalibrationSample().head_valid);
    sync_result=XR_SUCCESS; hand_result=XR_ERROR_RUNTIME_FAILURE;
    Frame(); EXPECT(!input::CalibrationSample().valid && input::CalibrationSample().head_valid);
    hand_result=XR_SUCCESS;

    // A frame that started before disable/re-enable must not publish into the new capture.
    toggle_during_head=2; Frame(); EXPECT(!input::CalibrationSample().valid && !input::CalibrationSample().head_valid);
    Frame(); EXPECT(input::CalibrationSample().valid && input::CalibrationSample().head_valid);
    input::SetCalibrationCapture(false); const auto stopped=head_calls;
    EXPECT(!input::CalibrationSample().valid && !input::CalibrationSample().head_valid);
    Frame(); EXPECT(head_calls==stopped);
    input::SetCalibrationCapture(true); Frame(); EXPECT(input::CalibrationSample().valid && input::CalibrationSample().head_valid);
    listener->OnSessionEnding(); EXPECT(!input::CalibrationSample().valid && !input::CalibrationSample().head_valid);
    const auto ended=head_calls; Frame(); EXPECT(head_calls==ended);
    if(failures) return 1;
    std::puts("calibration_tracking: all checks passed"); return 0;
}
