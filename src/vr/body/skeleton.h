#pragma once
// A skeleton's pose held in our own memory: local (parent-relative) and
// component-space transforms for every bone, with parent links. All body IK
// runs on this, and the engine is only touched twice a frame: one read of the
// animated pose and one write of the result (body.cpp). Each engine bone
// get/set used to rebuild the whole pose (~40 us a call, ~300 calls a frame).
// Pure math, unit tested in tests/test_body.cpp.

#include <cstdint>
#include <vector>

#include "vr/body/ik_math.h"

namespace holevr::vr::skel {

// FTransform (double precision), laid out like the engine's.
struct alignas(16) Xf {
    FQuat rotation;
    FVector translation;
    double pad0 = 0;
    FVector scale{1, 1, 1};
    double pad1 = 0;
};
static_assert(sizeof(Xf) == 0x60, "FTransform layout");

// child (in parent's space) placed in the parent's space's parent: what UE
// writes as child * parent.
Xf Compose(const Xf& child, const Xf& parent);
// t expressed relative to parent (inverse of Compose).
Xf Relative(const Xf& t, const Xf& parent);
inline FVector Apply(const Xf& t, const FVector& p) {
    return t.translation + t.rotation.Rotate(FVector{p.X * t.scale.X, p.Y * t.scale.Y, p.Z * t.scale.Z});
}

class Pose {
public:
    // parent[i] < i for every bone but the root (-1), as in UE skeletons.
    bool Init(const std::vector<int>& parent);
    int Count() const { return static_cast<int>(parent_.size()); }
    int Parent(int i) const { return parent_[i]; }

    // Replace all local transforms, then rebuild component space.
    void SetAllLocal(const Xf* local, int count);
    const std::vector<Xf>& Local() const { return local_; }
    const Xf& L(int i) const { return local_[i]; }
    const Xf& C(int i) const { return comp_[i]; }

    // Component space set: the bone's local is recomputed from its parent,
    // its descendants keep their locals and move with it.
    void SetC(int i, const Xf& t);
    void SetL(int i, const Xf& t);
    // Pre-multiplies a component-space rotation, keeping the bone's position.
    void RotateC(int i, const FQuat& q);
    void SetRotationC(int i, const FQuat& q);
    void SetTranslationC(int i, const FVector& p);

    // Bones whose local transform was changed since the last ClearChanged().
    bool Changed(int i) const { return changed_[i] != 0; }
    void ClearChanged();

private:
    void Refresh(int i);  // recompute comp of i's descendants
    std::vector<int> parent_;
    std::vector<std::vector<int>> desc_;  // descendants of each bone, parents first
    std::vector<Xf> local_, comp_;
    std::vector<uint8_t> changed_;
};

}  // namespace holevr::vr::skel
