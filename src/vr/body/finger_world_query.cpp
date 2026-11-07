#include "vr/body/finger_world_query.h"

#include <algorithm>
#include <cmath>

namespace holevr::vr::finger_world_query {
namespace {
using namespace ik;
bool Finite(const FVector& p) { return std::isfinite(p.X) && std::isfinite(p.Y) && std::isfinite(p.Z); }
double Setting(double value,double fallback,double lo,double hi) {
    return std::clamp(std::isfinite(value)?value:fallback,lo,hi);
}
bool Valid(const Geometry& g) {
    if (!Finite(g.wrist) || !std::isfinite(g.radius_cm) || g.radius_cm <= 0 || g.radius_cm > 3.0) return false;
    const auto& q = g.orientation;
    const double q2 = q.X*q.X + q.Y*q.Y + q.Z*q.Z + q.W*q.W;
    if (!std::isfinite(q2) || q2 < 0.5 || q2 > 1.5) return false;
    for (int f = 0; f < kFingers; ++f)
        if (g.valid[f])
            for (int n = 0; n < 4; ++n)
                if (!Finite(g.point[f][n]) || Length(g.point[f][n] - g.wrist) > 60) return false;
    return true;
}
double Motion(const Geometry& a, const Geometry& b) {
    double d = Length(a.wrist - b.wrist);
    for (int f = 0; f < kFingers; ++f) {
        if (a.valid[f] != b.valid[f]) return 1e9;
        if (a.valid[f]) for (int n = 0; n < 4; ++n) d = std::max(d, Length(a.point[f][n] - b.point[f][n]));
    }
    return d;
}
bool Discontinuity(const Geometry& a, const Geometry& b, double distance) {
    const auto& p = a.orientation;
    const auto& q = b.orientation;
    const double cos_half = std::fabs(p.X*q.X + p.Y*q.Y + p.Z*q.Z + p.W*q.W);
    return Motion(a, b) > distance || cos_half < 0.64 || std::fabs(a.radius_cm - b.radius_cm) > 1e-4;
}
double Envelope(const Geometry& g) {
    double radius = g.radius_cm;
    for (int f = 0; f < kFingers; ++f) if (g.valid[f])
        for (const auto& point : g.point[f]) radius = std::max(radius, Length(point - g.wrist) + g.radius_cm);
    return radius + 0.3;
}
double Fraction(const FVector& p, const FVector& a, const FVector& b) {
    const FVector d = b-a;
    const double len2 = Dot(d,d);
    return len2 > 1e-8 ? std::clamp(Dot(p-a,d)/len2, 0.0, 1.0) : 1.0;
}
bool Usable(const Hit& h) {
    const double n = Length(h.normal);
    return Finite(h.point) && Finite(h.center) && Finite(h.normal) && n > 0.5 && n < 1.5;
}
void Store(finger_contact::Contact& c, const Hit& h, int segment, double fraction, double radius,
           int source, bool recovered=false) {
    c={};
    if (!Usable(h) || h.initial_overlap) return;
    c.valid = true;
    c.segment = segment;
    c.fraction = std::clamp(fraction, 0.0, 1.0);
    c.point = h.point;
    c.normal = Normalize(h.normal);
    c.radius_cm = radius;
    c.source=static_cast<finger_contact::Contact::Source>(source);
    c.surface_id=h.surface_id;
    c.recovered_overlap=recovered;
    // Local tangent patch: finite in both tangent directions. A long segment
    // does not turn a small obstacle into an infinite wall.
    c.patch_radius_cm = std::clamp(2.0*radius + 0.5, 1.0, 2.5);
}
int Priority(const finger_contact::Contact& c) {
    using Source=finger_contact::Contact::Source;
    return c.source==Source::temporal ? 3 : c.source==Source::displayed ? 2 : 1;
}
bool Incompatible(const finger_contact::Contact& a,const finger_contact::Contact& b) {
    if(!a.surface_id || a.surface_id!=b.surface_id || a.segment!=b.segment ||
       Dot(a.normal,b.normal)>-0.85 || Length(a.point-b.point)>a.patch_radius_cm+b.patch_radius_cm) return false;
    // Opposing surfaces of a corridor can both be satisfied. Opposite sides
    // of the same solid object cannot: do not ask one phalanx to satisfy an
    // impossible pair of half-spaces just because its desired pose passed
    // through the object. Keep the surface reached from the displayed side.
    const FVector axis=Normalize(a.normal-b.normal);
    return Dot(b.point-a.point,axis)<a.radius_cm+b.radius_cm-0.02;
}
}  // namespace

Geometry MakeGeometry(const fingers::Chain chains[kFingers], const fingers::Angles poses[kFingers],
                      const skel::Xf& hand, double radius_cm) {
    Geometry g;
    g.wrist = hand.translation;
    g.orientation = hand.rotation;
    g.radius_cm = radius_cm;
    for (int f = 0; f < kFingers; ++f) {
        g.valid[f] = chains[f].valid;
        if (!g.valid[f]) continue;
        skel::Xf joint[3]; FVector tip;
        fingers::Fk(chains[f], poses[f], joint, &tip);
        for (int n = 0; n < 3; ++n) g.point[f][n] = skel::Apply(hand,joint[n].translation);
        g.point[f][3] = skel::Apply(hand,tip);
    }
    return g;
}

void Reset(State& s) { s = State{}; }
void Commit(State& s, const Geometry& g) {
    if (!Valid(g)) { Reset(s); return; }
    s.rendered = g;
    s.have_rendered = true;
}

Stats Update(State& s, const Geometry& requested, double dt, Backend& backend,
             finger_contact::Contacts output[kFingers], const Settings& settings) {
    Stats stats;
    for (int f = 0; f < kFingers; ++f) output[f] = {};
    if (!Valid(requested) || !std::isfinite(dt) || dt <= 0.0) {
        Reset(s); stats.reset = true; return stats;
    }
    const double refresh = Setting(settings.clear_refresh_seconds, 0.05, 0.005, 0.05);
    const double max_age = Setting(settings.max_contact_age_seconds, 0.05, 0.005, 0.05);
    const double motion_epsilon = Setting(settings.movement_threshold_cm, 0.15, 0.01, 0.5);
    const int budget = std::clamp(settings.max_casts, 16, 64);
    if (dt > 0.25 || (s.initialized && Discontinuity(requested, s.queried,
                                  Setting(settings.teleport_distance_cm, 30.0, 10.0, 100.0)))) {
        Reset(s); stats.reset = true;
    }
    const bool moved = !s.initialized || Motion(requested,s.queried) > motion_epsilon;
    const bool rendered_moved=s.have_rendered && Motion(s.rendered,s.clear_rendered)>motion_epsilon;
    for (auto& digit : s.segment) for (auto& cache : digit) for(auto& c:cache.contacts)
        if(c.valid) {
            c.age_seconds+=dt;
            if(c.age_seconds>=max_age) { ++stats.expired; c={}; }
        }
    s.clear_age += dt;
    if (s.initialized && s.broad_clear && !moved && !rendered_moved && s.clear_age < refresh) {
        stats.available = true;
        stats.reused = 1;
        return stats;
    }
    bool narrow_hit=false;
    auto sweep = [&](const FVector& from, const FVector& to, double radius, Hit& hit) {
        if (stats.casts >= budget) { stats.budget_limited = true; return QueryStatus::unavailable; }
        ++stats.casts;
        const auto result = backend.Sweep(from,to,radius,hit);
        if(stats.casts>1 && result==QueryStatus::hit) { narrow_hit=true; stats.nearby=true; }
        if (result == QueryStatus::unavailable) ++stats.failures;
        return result;
    };
    // The large, near-stationary sphere is only a proximity hint. Some mesh
    // query paths do not report an initial overlap, so a clear result cannot
    // veto the actual phalanx sweeps. Cache clear only after those complete.
    Hit broad;
    const FVector broad_from = s.have_rendered ? s.rendered.wrist : requested.wrist;
    const double broad_radius = std::max(Envelope(requested), s.have_rendered ? Envelope(s.rendered) : 0.0);
    const auto broad_status = sweep(broad_from, requested.wrist + FVector{0,0,0.001}, broad_radius, broad);
    stats.available = broad_status != QueryStatus::unavailable;
    s.queried = requested;
    s.initialized = true;
    s.clear_age = 0;
    s.broad_clear = false;
    if (broad_status == QueryStatus::unavailable) {
        for (auto& digit : s.segment) for (auto& c : digit) c = SegmentCache{};
        return stats;
    }
    stats.nearby = broad_status==QueryStatus::hit;
    struct Pending { QueryStatus status=QueryStatus::clear; Hit hit{}; };
    Pending pending[kFingers][kSegments];
    // Always cover every requested phalanx first. Sixteen casts suffice for
    // this complete set plus the coarse gate, even at the minimum budget.
    for (int k=0;k<kFingers*kSegments;++k) {
        const int index=(s.cursor+k)%(kFingers*kSegments),f=index/kSegments,j=index%kSegments;
        if(!requested.valid[f]) { s.segment[f][j]={}; continue; }
        auto& p=pending[f][j];
        p.status=sweep(requested.point[f][j],requested.point[f][j+1],requested.radius_cm,p.hit);
        stats.primary_hits+=p.status==QueryStatus::hit;
        auto& contact=s.segment[f][j].contacts[0];
        if(p.status!=QueryStatus::hit || !p.hit.initial_overlap) contact={};
        if(p.status==QueryStatus::hit && !p.hit.initial_overlap)
            Store(contact,p.hit,j,Fraction(p.hit.center,requested.point[f][j],requested.point[f][j+1]),requested.radius_cm,0);
    }
    stats.broad_false_clear=broad_status==QueryStatus::clear && stats.primary_hits>0;
    // Share remaining casts fairly between overlap recovery, the displayed
    // capsules, and motion sweeps. Full displayed capsules matter when an
    // object approaches a bent phalanx while the requested pose is elsewhere.
    // A persistent cursor prevents recovery or continuous wrist motion from
    // starving the same finger/category. With no recovery and all segments
    // moving, the default budget covers both categories within two frames.
    constexpr int work_count=kFingers*kSegments*3;
    auto recover=[&](const Hit& initial,int f,int j,int kind,const FVector& a,const FVector& b) {
        auto& contact=s.segment[f][j].contacts[kind];
        if(!Usable(initial)) { contact={}; return; }
        if(stats.casts>=budget) {
            stats.budget_limited=true;
            stats.reused+=contact.valid;
            return;
        }
        FVector from=initial.center+Normalize(initial.normal)*(requested.radius_cm+0.05);
        const finger_contact::Contact* prior=nullptr;
        for(const auto& candidate:s.segment[f][j].contacts)
            if(candidate.valid && initial.surface_id && candidate.surface_id==initial.surface_id &&
               (!prior || Priority(candidate)>Priority(*prior))) prior=&candidate;
        // Overlap depenetration normals can flip when a requested pose crosses
        // an object's center. Start from the last verified outside surface
        // when available; the new trace must still supply a real impact.
        if(prior) from=prior->point+prior->normal*(requested.radius_cm+0.2);
        const auto previous=contact;
        contact={};
        Hit hit;
        auto status=sweep(from,initial.center,requested.radius_cm,hit);
        // Start close: the old four-centimeter recast could begin inside the
        // opposite wall of a narrow gap. Only an unresolved initial overlap
        // justifies trying farther out, and that cast shares the same budget.
        if(status==QueryStatus::hit && hit.initial_overlap && stats.casts<budget) {
            from=initial.center+Normalize(initial.normal)*(2.0*requested.radius_cm+3.0);
            status=sweep(from,initial.center,requested.radius_cm,hit);
        }
        if(status==QueryStatus::hit && !hit.initial_overlap) {
            Store(contact,hit,j,Fraction(initial.center,a,b),requested.radius_cm,kind,true);
            if(contact.valid) ++stats.recovered_overlaps;
            else ++stats.failed_recovery;
        } else {
            ++stats.failed_recovery;
            if(status==QueryStatus::hit && hit.initial_overlap && stats.casts>=budget) {
                stats.budget_limited=true;
                contact=previous; // preserve its age; no new surface was measured
                stats.reused+=contact.valid;
            }
        }
    };
    const int start=s.work_cursor;
    bool stopped=false;
    for(int k=0;k<work_count;++k) {
        const int work=(start+k)%work_count,index=work/3,kind=work%3;
        const int f=index/kSegments,j=index%kSegments;
        if(!requested.valid[f]) continue;
        const bool rendered=s.have_rendered && s.rendered.valid[f];
        const bool endpoint_moved=rendered && Length(requested.point[f][j+1]-s.rendered.point[f][j+1])>motion_epsilon;
        const bool segment_moved=endpoint_moved || (rendered && Length(requested.point[f][j]-s.rendered.point[f][j])>motion_epsilon);
        const auto& p=pending[f][j];
        const bool needed=kind==0 ? p.status==QueryStatus::hit && p.hit.initial_overlap
                                 : kind==1 ? segment_moved : endpoint_moved;
        auto& contact=s.segment[f][j].contacts[kind];
        if(!needed) { if(kind!=0) contact={}; continue; }
        if(stats.casts>=budget) {
            stats.budget_limited=true;
            stats.reused+=contact.valid;
            if(!stopped) { s.work_cursor=work; stopped=true; }
            continue;
        }
        if(kind==0) {
            // An overlap ImpactPoint is a sweep center, not a surface. Obtain
            // a new hit from outside; never manufacture a tangent plane.
            recover(p.hit,f,j,0,requested.point[f][j],requested.point[f][j+1]);
        } else if(kind==1) {
            Hit hit;
            const auto status=sweep(s.rendered.point[f][j],s.rendered.point[f][j+1],requested.radius_cm,hit);
            ++stats.displayed_casts;
            if(status!=QueryStatus::hit || !hit.initial_overlap) contact={};
            if(status==QueryStatus::hit) {
                if(hit.initial_overlap) recover(hit,f,j,1,s.rendered.point[f][j],s.rendered.point[f][j+1]);
                else Store(contact,hit,j,Fraction(hit.center,s.rendered.point[f][j],s.rendered.point[f][j+1]),requested.radius_cm,1);
            }
        } else {
            Hit hit;
            const auto status=sweep(s.rendered.point[f][j+1],requested.point[f][j+1],requested.radius_cm,hit);
            ++stats.temporal_casts;
            contact={};
            if(status==QueryStatus::hit && !hit.initial_overlap)
                Store(contact,hit,j,1.0,requested.radius_cm,2);
        }
        if(!stopped) s.work_cursor=(work+1)%work_count;
    }
    s.cursor=(s.cursor+1)%(kFingers*kSegments);
    stats.broad_false_clear=broad_status==QueryStatus::clear && narrow_hit;
    s.broad_clear=!narrow_hit && !stats.failures && !stats.budget_limited;
    if(s.broad_clear) s.clear_rendered=s.have_rendered ? s.rendered : requested;
    for (int f=0; f<kFingers; ++f) for (int j=0; j<kSegments; ++j) {
        const auto& cache=s.segment[f][j];
        for (const auto& c:cache.contacts) {
            if(!c.valid || c.age_seconds>=max_age || output[f].count>=finger_contact::kMaxContacts) continue;
            bool conflict=false;
            for(const auto& other:cache.contacts)
                if(other.valid && other.age_seconds<max_age && Priority(other)>Priority(c) && Incompatible(c,other)) {
                    conflict=true; ++stats.conflicting_contacts; break;
                }
            if(conflict) continue;
            output[f].items[output[f].count++]=c;
        }
    }
    return stats;
}
}  // namespace holevr::vr::finger_world_query
