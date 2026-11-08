// Native unit tests for the order of headset image calls (no headset, no
// Windows): a fake OpenXR swapchain that enforces the spec's rules, and the
// frame loop's use of vr/swapchain_hold.h against it, with waits that time
// out in either eye and copies that are skipped.
//   g++ -std=c++20 -O2 -Isrc tests/test_swapchain_hold.cpp -o /tmp/test_swapchain_hold && /tmp/test_swapchain_hold
#include <cstdio>
#include <deque>
#include <vector>

#include "vr/swapchain_hold.h"

using namespace holevr::vr;

static int failures = 0;
#define EXPECT(c)                                                     \
    do {                                                              \
        if (!(c)) {                                                   \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);  \
            ++failures;                                               \
        }                                                             \
    } while (0)

// One swapchain as the OpenXR spec describes it. Breaking a rule counts in
// `order_errors` (a real runtime returns XR_ERROR_CALL_ORDER_INVALID, or the
// GPU writes into an image the compositor may still be reading).
struct FakeSwapchain {
    int count = 3;
    int next = 0;                 // acquire hands out images round-robin
    std::deque<int> acquired;     // acquired, not yet waited (oldest first)
    std::deque<int> waited;       // waited, not yet released
    std::vector<int> content;     // frame number written into each image
    int shown = -1;               // last released image: what the headset shows
    int timeouts_left = 0;        // the next N waits time out
    int last_wait_target = -1;
    int order_errors = 0;
    int waits_on_same_after_timeout = 0;
    bool last_wait_timed_out = false;

    FakeSwapchain() : content(3, -1) {}

    bool Acquire(uint32_t* index) {
        if (static_cast<int>(acquired.size() + waited.size()) >= count) {
            ++order_errors;
            return false;
        }
        acquired.push_back(next);
        *index = static_cast<uint32_t>(next);
        next = (next + 1) % count;
        return true;
    }
    WaitResult Wait() {
        if (acquired.empty()) {
            ++order_errors;
            return WaitResult::kError;
        }
        if (!waited.empty()) ++order_errors;  // must release before waiting on the next image
        const int target = acquired.front();
        if (last_wait_timed_out && target == last_wait_target) ++waits_on_same_after_timeout;
        last_wait_target = target;
        if (timeouts_left > 0) {
            --timeouts_left;
            last_wait_timed_out = true;
            return WaitResult::kTimeout;
        }
        last_wait_timed_out = false;
        acquired.pop_front();
        waited.push_back(target);
        return WaitResult::kReady;
    }
    void Write(uint32_t index, int frame) {
        bool ok = false;
        for (int w : waited) ok |= w == static_cast<int>(index);
        if (!ok) ++order_errors;  // writing an image that was not waited on
        content[index] = frame;
    }
    void Release() {
        if (waited.empty()) {
            ++order_errors;
            return;
        }
        shown = waited.front();
        waited.pop_front();
    }
    int ShownFrame() const { return shown < 0 ? -1 : content[shown]; }
};

// The frame loop as RunFrame does it: ready both eyes, copy only if both are
// ready and the GPU allows, release only after the copy.
struct Loop {
    FakeSwapchain sc[2];
    HeldImage held[2];
    bool gpu_busy = false;  // CopyEyes skipped this frame
    int copied = 0;

    bool Frame(int frame) {
        for (int e = 0; e < 2; ++e) {
            const ImageStep step = ReadyImage(
                held[e], [&](uint32_t* i) { return sc[e].Acquire(i); }, [&] { return sc[e].Wait(); });
            if (step != ImageStep::kReady) return false;
        }
        if (gpu_busy) return false;
        for (int e = 0; e < 2; ++e) sc[e].Write(held[e].index, frame);
        ++copied;
        for (int e = 0; e < 2; ++e) ReleaseWritten(held[e], [&] { sc[e].Release(); });
        return true;
    }
    int Errors() const { return sc[0].order_errors + sc[1].order_errors; }
};

static void TestSteadyFrames() {
    Loop l;
    for (int f = 0; f < 20; ++f) {
        EXPECT(l.Frame(f));
        EXPECT(l.sc[0].ShownFrame() == f && l.sc[1].ShownFrame() == f);
    }
    EXPECT(l.Errors() == 0);
}

// A wait that times out keeps its image and is retried on the same image;
// nothing unwritten is released, so the headset keeps the last good picture.
static void TestTimeoutInEye(int eye) {
    Loop l;
    EXPECT(l.Frame(0));
    l.sc[eye].timeouts_left = 2;
    EXPECT(!l.Frame(1));
    EXPECT(!l.Frame(2));
    EXPECT(l.held[eye].acquired && !l.held[eye].waited);
    EXPECT(l.sc[0].ShownFrame() == 0 && l.sc[1].ShownFrame() == 0);  // still the last good picture
    EXPECT(l.sc[eye].waits_on_same_after_timeout == 1);
    EXPECT(l.Frame(3));  // recovers on a later wait, still the same image
    EXPECT(l.sc[eye].waits_on_same_after_timeout == 2);
    EXPECT(l.sc[0].ShownFrame() == 3 && l.sc[1].ShownFrame() == 3);
    for (int f = 4; f < 12; ++f) EXPECT(l.Frame(f));
    EXPECT(l.sc[0].ShownFrame() == 11 && l.sc[1].ShownFrame() == 11);
    EXPECT(l.Errors() == 0);
    // Both eyes stay on matching images (no permanent one-frame shift).
    EXPECT(l.sc[0].shown == l.sc[1].shown);
}

// Both eyes ready but the copy cannot run (GPU still busy): both stay held,
// the next frame copies into the same images without acquiring again.
static void TestCopySkipped() {
    Loop l;
    EXPECT(l.Frame(0));
    l.gpu_busy = true;
    EXPECT(!l.Frame(1));
    const uint32_t i0 = l.held[0].index, i1 = l.held[1].index;
    EXPECT(l.held[0].waited && l.held[1].waited);
    EXPECT(!l.Frame(2));
    EXPECT(l.held[0].index == i0 && l.held[1].index == i1);
    EXPECT(l.sc[0].ShownFrame() == 0 && l.sc[1].ShownFrame() == 0);
    l.gpu_busy = false;
    EXPECT(l.Frame(3));
    EXPECT(l.sc[0].ShownFrame() == 3 && l.sc[1].ShownFrame() == 3);
    EXPECT(l.Errors() == 0);
}

// Timeouts in both eyes on alternate frames, mixed with skipped copies.
static void TestMixed() {
    Loop l;
    int last_good = -1;
    for (int f = 0; f < 200; ++f) {
        if (f % 7 == 3) l.sc[f % 2].timeouts_left = 1 + f % 3;
        l.gpu_busy = f % 11 == 5;
        if (l.Frame(f)) last_good = f;
        EXPECT(l.sc[0].ShownFrame() == last_good && l.sc[1].ShownFrame() == last_good);
    }
    // Once the timeouts stop it recovers within a few frames.
    l.gpu_busy = false;
    for (int f = 200; f < 205; ++f)
        if (l.Frame(f)) last_good = f;
    EXPECT(last_good == 204);
    EXPECT(l.sc[0].ShownFrame() == 204 && l.sc[1].ShownFrame() == 204);
    EXPECT(l.Errors() == 0);
}

// What the old loop did (release every acquired image even after a timeout)
// breaks the rules in this fake, so the fake does catch the defect.
static void TestOldLoopIsCaught() {
    FakeSwapchain sc;
    uint32_t i = 0;
    sc.Acquire(&i);
    sc.timeouts_left = 1;
    EXPECT(sc.Wait() == WaitResult::kTimeout);
    sc.Release();  // old code: released regardless
    EXPECT(sc.order_errors == 1);
}

int main() {
    TestSteadyFrames();
    TestTimeoutInEye(0);
    TestTimeoutInEye(1);
    TestCopySkipped();
    TestMixed();
    TestOldLoopIsCaught();
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("swapchain hold tests passed\n");
    return 0;
}
