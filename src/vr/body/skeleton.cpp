#include "vr/body/skeleton.h"

#include <cmath>

namespace holevr::vr::skel {

Xf Compose(const Xf& c, const Xf& p) {
    Xf r;
    r.rotation = ik::Normalized(p.rotation * c.rotation);
    r.scale = FVector{c.scale.X * p.scale.X, c.scale.Y * p.scale.Y, c.scale.Z * p.scale.Z};
    r.translation = Apply(p, c.translation);
    return r;
}

static double SafeDiv(double a, double b) { return std::fabs(b) > 1e-9 ? a / b : a; }

Xf Relative(const Xf& t, const Xf& p) {
    Xf r;
    FQuat inv = p.rotation.Inverse();
    r.rotation = ik::Normalized(inv * t.rotation);
    r.scale = FVector{SafeDiv(t.scale.X, p.scale.X), SafeDiv(t.scale.Y, p.scale.Y), SafeDiv(t.scale.Z, p.scale.Z)};
    FVector d = inv.Rotate(t.translation - p.translation);
    r.translation = FVector{SafeDiv(d.X, p.scale.X), SafeDiv(d.Y, p.scale.Y), SafeDiv(d.Z, p.scale.Z)};
    return r;
}

bool Pose::Init(const std::vector<int>& parent) {
    const int n = static_cast<int>(parent.size());
    for (int i = 0; i < n; ++i)
        if (parent[i] >= i || parent[i] < -1) return false;
    parent_ = parent;
    desc_.assign(n, {});
    for (int i = 0; i < n; ++i)
        for (int a = parent[i]; a >= 0; a = parent[a]) desc_[a].push_back(i);  // i ascending: parents first
    local_.assign(n, Xf{});
    comp_.assign(n, Xf{});
    changed_.assign(n, 0);
    return true;
}

void Pose::SetAllLocal(const Xf* local, int count) {
    for (int i = 0; i < Count() && i < count; ++i) local_[i] = local[i];
    for (int i = 0; i < Count(); ++i) comp_[i] = parent_[i] < 0 ? local_[i] : Compose(local_[i], comp_[parent_[i]]);
}

void Pose::Refresh(int i) {
    for (int d : desc_[i]) comp_[d] = Compose(local_[d], comp_[parent_[d]]);
}

void Pose::SetC(int i, const Xf& t) {
    comp_[i] = t;
    local_[i] = parent_[i] < 0 ? t : Relative(t, comp_[parent_[i]]);
    changed_[i] = 1;
    Refresh(i);
}

void Pose::SetL(int i, const Xf& t) {
    local_[i] = t;
    comp_[i] = parent_[i] < 0 ? t : Compose(t, comp_[parent_[i]]);
    changed_[i] = 1;
    Refresh(i);
}

void Pose::RotateC(int i, const FQuat& q) {
    Xf t = comp_[i];
    t.rotation = ik::Normalized(q * t.rotation);
    SetC(i, t);
}

void Pose::SetRotationC(int i, const FQuat& q) {
    Xf t = comp_[i];
    t.rotation = q;
    SetC(i, t);
}

void Pose::SetTranslationC(int i, const FVector& p) {
    Xf t = comp_[i];
    t.translation = p;
    SetC(i, t);
}

void Pose::ClearChanged() {
    for (auto& c : changed_) c = 0;
}

}  // namespace holevr::vr::skel
