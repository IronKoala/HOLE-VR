// Native unit tests for core/gpu_share_math.h: Windows' "GPU Engine" counter
// names are read right, and the per-program summary adds up.
//   g++ -std=c++20 -Isrc tests/test_gpu_share.cpp -o /tmp/test_gpu_share && /tmp/test_gpu_share
#include <cstdio>
#include <string>

#include "core/gpu_share_math.h"

using namespace holevr::gpu_share;

static int failures = 0;
#define EXPECT(c)                                                     \
    do {                                                              \
        if (!(c)) {                                                   \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);  \
            ++failures;                                               \
        }                                                             \
    } while (0)

int main() {
    Instance in;
    EXPECT(ParseInstance("pid_1234_luid_0x00000000_0x0000C3B0_phys_0_eng_0_engtype_3D", &in));
    EXPECT(in.pid == 1234);
    EXPECT(in.luid == "0x00000000_0x0000C3B0");
    EXPECT(in.engine == Engine::k3D);
    EXPECT(ParseInstance("pid_8_luid_0x00000000_0x0000C3B0_phys_0_eng_1_engtype_Compute_0", &in));
    EXPECT(in.engine == Engine::kCompute && in.pid == 8);
    EXPECT(ParseInstance("pid_8_luid_0x00000000_0x0000C3B0_phys_0_eng_9_engtype_VideoEncode", &in));
    EXPECT(in.engine == Engine::kVideoEncode);
    EXPECT(ParseInstance("pid_8_luid_0x00000000_0x0000C3B0_phys_0_eng_4_engtype_Copy", &in));
    EXPECT(in.engine == Engine::kCopy);
    EXPECT(ParseInstance("pid_8_luid_0x00000000_0x0000C3B0_phys_0_eng_5_engtype_Vr", &in));
    EXPECT(in.engine == Engine::kOther);
    EXPECT(!ParseInstance("_Total", &in));
    EXPECT(!ParseInstance("pid_x_luid_0x0_0x1_phys_0_eng_0_engtype_3D", &in));
    EXPECT(!ParseInstance("pid_12_luid_0x0_0x1_eng_0_engtype_3D", &in));

    Share s;
    Instance game, comp, other;
    ParseInstance("pid_100_luid_a_b_phys_0_eng_0_engtype_3D", &game);
    ParseInstance("pid_200_luid_a_b_phys_0_eng_0_engtype_3D", &comp);
    ParseInstance("pid_300_luid_a_b_phys_0_eng_0_engtype_3D", &other);
    s.Add(game, 78.0);
    s.Add(comp, 11.2);
    s.Add(other, 0.2);
    s.Add(other, 0.0);
    EXPECT(s.total[0] > 89.3 && s.total[0] < 89.5);
    auto name = [](uint32_t pid) -> std::string {
        return pid == 100 ? "the game" : pid == 200 ? "vrcompositor.exe" : "pid " + std::to_string(pid);
    };
    const std::string top = Top(s.by_pid[0], name);
    EXPECT(top == "the game 78%, vrcompositor.exe 11%");  // 0.2% is under the half-percent shown
    Share many;
    for (uint32_t pid = 1; pid <= 8; ++pid) {
        Instance i;
        ParseInstance("pid_" + std::to_string(pid) + "_luid_a_b_phys_0_eng_0_engtype_3D", &i);
        many.Add(i, 10.0);
    }
    const std::string t2 = Top(many.by_pid[0], name, 5);
    EXPECT(t2.find("others 30%") != std::string::npos);
    EXPECT(Top(Share{}.by_pid[0], name) == "nothing");

    if (failures) return 1;
    std::printf("test_gpu_share: all passed\n");
    return 0;
}
