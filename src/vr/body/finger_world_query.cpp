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
double Radius(const Geometry& g,int finger,int segment) {
    return g.segment_radius_cm[finger][segment]>0 ? g.segment_radius_cm[finger][segment] : g.radius_cm;
}
bool Valid(const Geometry& g) {
    if (!Finite(g.wrist) || !std::isfinite(g.radius_cm) || g.radius_cm <= 0 || g.radius_cm > 3.0) return false;
    const auto& q = g.orientation;
    const double q2 = q.X*q.X + q.Y*q.Y + q.Z*q.Z + q.W*q.W;
    if (!std::isfinite(q2) || q2 < 0.5 || q2 > 1.5) return false;
    for (int f = 0; f < kFingers; ++f)
        if (g.valid[f]) {
            for (int n = 0; n < 4; ++n)
                if (!Finite(g.point[f][n]) || Length(g.point[f][n] - g.wrist) > 60) return false;
            for(int j=0;j<kSegments;++j)
                if(!std::isfinite(g.segment_radius_cm[f][j]) || g.segment_radius_cm[f][j]<0 || Radius(g,f,j)>8) return false;
        }
    return true;
}
double Motion(const Geometry& a, const Geometry& b) {
    double d = Length(a.wrist - b.wrist);
    for (int f = 0; f < kFingers; ++f) {
        if (a.valid[f] != b.valid[f]) return 1e9;
        if(a.valid[f]) for(int j=0;j<kSegments;++j) if(std::fabs(Radius(a,f,j)-Radius(b,f,j))>1e-5) return 1e9;
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
bool ValidFrame(const skel::Xf& frame) {
    const auto& q=frame.rotation;
    const double q2=q.X*q.X+q.Y*q.Y+q.Z*q.Z+q.W*q.W;
    return Finite(frame.translation) && Finite(frame.scale) && std::isfinite(q2) &&
        q2>.99 && q2<1.01 && std::fabs(frame.scale.X)>1e-5 &&
        std::fabs(frame.scale.Y)>1e-5 && std::fabs(frame.scale.Z)>1e-5;
}
void MoveFeature(finger_contact::Contact& c,const skel::Xf& old,const skel::Xf& now) {
    auto point=old.rotation.Inverse().Rotate(c.point-old.translation);
    point={point.X/old.scale.X,point.Y/old.scale.Y,point.Z/old.scale.Z};
    auto normal=old.rotation.Inverse().Rotate(c.normal);
    normal={normal.X*old.scale.X,normal.Y*old.scale.Y,normal.Z*old.scale.Z};
    c.point=skel::Apply(now,point);
    c.normal=Normalize(now.rotation.Rotate({normal.X/now.scale.X,normal.Y/now.scale.Y,normal.Z/now.scale.Z}));
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
    using Source=finger_contact::Contact::Source;
    c.source=source==0 ? Source::desired : source==1 ? Source::displayed :
             source==2 ? Source::temporal : Source::endpoint;
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
        for(int j=0;j<kSegments;++j)
            g.segment_radius_cm[f][j]=hand_collision_shape::Transform(chains[f].collision[j],joint[j],hand,radius_cm).radius_cm;
        for (int n = 0; n < 3; ++n) g.point[f][n] = skel::Apply(hand,joint[n].translation);
        g.point[f][3] = skel::Apply(hand,tip);
    }
    return g;
}

void Reset(State& s) { s = State{}; }
void Commit(State& s, const Geometry& g) {
    if (!Valid(g)) { Reset(s); return; }
    for(int f=0;f<kFingers;++f) if(g.valid[f]) for(int j=0;j<kSegments;++j) {
        auto& cache=s.segment[f][j];
        const auto& measured=cache.observation[0];
        if(measured.valid && measured.geometry_clear &&
           std::fabs(measured.radius_cm-Radius(g,f,j))<1e-6 &&
           Length(measured.from-g.point[f][j])<=.001 &&
           Length(measured.to-g.point[f][j+1])<=.001) {
            // This exact full-radius capsule was already checked before it
            // was displayed. Its next-frame displayed check can use that
            // same observation, without renewing either geometry or contact
            // age. Motion sweeps and the50ms moving-world audit still run.
            cache.observation[1]=measured;
            cache.contacts[1]=cache.contacts[0];
            cache.contacts[1].source=finger_contact::Contact::Source::displayed;
        }
    }
    s.rendered = g;
    s.have_rendered = true;
}
void InvalidateGeometry(State& s) {
    for(auto& digit:s.segment) for(auto& cache:digit)
        for(auto& observation:cache.observation) observation.valid=false;
    s.broad_clear=false;
}
double MaxDelta(const Geometry& a,const Geometry& b) {
    return Valid(a) && Valid(b) && std::fabs(a.radius_cm-b.radius_cm)<1e-6 ? Motion(a,b) : 1e9;
}
Verification VerifyGeometry(State& s,const Geometry& actual,Backend& backend,const Settings& settings) {
    Verification result;
    if(!s.initialized || !Valid(actual)) return result;
    const int budget=std::clamp(settings.max_casts,16,64);
    const int initial_calls=backend.InvocationCount();
    int hits=0;
    auto finish=[&]() {
        const int calls=backend.InvocationCount();
        result.engine_calls=calls>=0 && initial_calls>=0 ? calls-initial_calls : result.casts+hits;
        return result;
    };
    // Two-tenths of a millimeter tolerates an already seated boundary. The
    // remaining finger volume is tested against actual engine geometry.
    for(int f=0;f<kFingers;++f) if(actual.valid[f]) for(int j=0;j<kSegments;++j) {
        const double fitted_radius=Radius(actual,f,j);
        const double radius=std::max(fitted_radius*.9,fitted_radius-.02);
        if(s.frame_casts>=budget) { result.budget_limited=true; return finish(); }
        ++s.frame_casts; ++result.casts;
        Hit hit;
        const auto status=backend.Sweep(actual.point[f][j],actual.point[f][j+1],radius,hit);
        if(status==QueryStatus::unavailable) return finish();
        if(status==QueryStatus::hit) {
            ++hits; result.available=true; result.blocked_mask|=1u<<f; return finish();
        }
    }
    result.available=true; result.clear=true;
    return finish();
}

Stats Update(State& s, const Geometry& requested, double dt, Backend& backend,
             finger_contact::Contacts output[kFingers], const Settings& settings) {
    Stats stats;
    const int calls_before=backend.InvocationCount();
    auto finish=[&]() {
        const int actual=backend.InvocationCount();
        stats.engine_calls=actual>=0 && calls_before>=0 ? actual-calls_before :
            stats.casts+stats.hit_results+stats.surface_queries;
        s.frame_casts=stats.casts;
        return stats;
    };
    for(int f=0;f<kFingers;++f) output[f]={};
    if(!Valid(requested) || !std::isfinite(dt) || dt<=0) {
        Reset(s); stats.reset=true; return finish();
    }
    const double refresh=Setting(settings.clear_refresh_seconds,.05,.005,.05);
    const double max_age=Setting(settings.max_contact_age_seconds,.05,.005,.05);
    const double epsilon=Setting(settings.movement_threshold_cm,.15,.01,.5);
    const int budget=std::clamp(settings.max_casts,16,64);
    if(dt>.25 || (s.initialized && Discontinuity(requested,s.queried,
                                  Setting(settings.teleport_distance_cm,30,10,100)))) {
        Reset(s); stats.reset=true;
    }
    for(auto& digit:s.segment) for(auto& cache:digit) for(int kind=0;kind<4;++kind) {
        cache.observation[kind].age_seconds+=dt;
        if(cache.contacts[kind].valid) cache.contacts[kind].age_seconds+=dt;
    }
    s.queried=requested; s.initialized=true; s.broad_clear=false;
    stats.available=true;
    bool feature_checked[kFingers][kSegments][4]{};
    struct Surface { std::uint64_t id=0; bool valid=false; skel::Xf frame{}; };
    std::array<Surface,4> surfaces{};
    int surface_count=0;
    auto frame_for=[&](std::uint64_t id,skel::Xf& frame) {
        if(!id) return false;
        for(int i=0;i<surface_count;++i) if(surfaces[i].id==id) {
            frame=surfaces[i].frame; return surfaces[i].valid;
        }
        if(surface_count==static_cast<int>(surfaces.size())) return false;
        auto& result=surfaces[surface_count++]; result.id=id;
        ++stats.surface_queries;
        result.valid=backend.SurfaceTransform(id,result.frame) && ValidFrame(result.frame);
        frame=result.frame;
        return result.valid;
    };
    auto unresolved=[&](int f,int j) {
        stats.unresolved_mask|=1u<<f;
        stats.unresolved_segments|=1u<<(f*kSegments+j);
    };
    auto sweep=[&](const FVector& from,const FVector& to,double radius,Hit& hit) {
        if(stats.casts>=budget) { stats.budget_limited=true; return QueryStatus::unavailable; }
        ++stats.casts;
        const auto status=backend.Sweep(from,to,radius,hit);
        if(status==QueryStatus::unavailable) { ++stats.failures; stats.available=false; }
        if(status==QueryStatus::hit) { ++stats.hit_results; stats.nearby=true; }
        return status;
    };
    auto reuse=[&](int f,int j,int kind,const FVector& from,const FVector& to) {
        auto& cache=s.segment[f][j];
        auto& observation=cache.observation[kind];
        auto& c=cache.contacts[kind];
        if(!observation.valid || observation.age_seconds>=(observation.clear ? refresh : max_age) ||
           Length(from-observation.from)>epsilon || Length(to-observation.to)>epsilon ||
           std::fabs(observation.radius_cm-Radius(requested,f,j))>1e-6) return false;
        if(!observation.clear) {
            if(!c.valid) return false;
            skel::Xf frame;
            const bool tracked=observation.have_frame && frame_for(c.surface_id,frame);
            auto updated=c;
            if(tracked) MoveFeature(updated,observation.frame,frame);
            if(tracked && (Length(updated.point-c.point)>1e-5 || Dot(updated.normal,c.normal)<.999999))
                observation.geometry_clear=false;
            const auto nearest=from+(to-from)*Fraction(updated.point,from,to);
            const auto delta=nearest-updated.point;
            // A feature which leaves the finite neighborhood no longer
            // certifies this query. Reacquire actual geometry immediately.
            if(Length(Reject(delta,updated.normal))>updated.patch_radius_cm ||
               std::fabs(Dot(delta,updated.normal))>Radius(requested,f,j)+1.0 ||
               Length(updated.point-c.point)>30) return false;
            if(!tracked) {
                // Unsupported/deforming features cannot be moved from a
                // component transform. Measure their actual surface from
                // the verified outside side, one cast instead of another
                // initial overlap plus recovery. This refreshes the contact
                // only: the full-segment audit deadline remains unchanged.
                if(stats.casts>=budget) return false;
                Hit hit;
                ++stats.feature_casts;
                feature_checked[f][j][kind]=true;
                const auto target=from+(to-from)*c.fraction-c.normal*.08;
                const auto status=sweep(c.point+c.normal*(Radius(requested,f,j)+.2),target,
                                        Radius(requested,f,j),hit);
                if(status!=QueryStatus::hit || hit.initial_overlap) return false;
                Store(updated,hit,j,c.fraction,Radius(requested,f,j),kind);
                if(!updated.valid) return false;
                if(Length(updated.point-c.point)>1e-5 || Dot(updated.normal,c.normal)<.999999)
                    observation.geometry_clear=false;
                observation.have_frame=false;
            }
            c=updated;
            if(tracked) observation.frame=frame;
            stats.nearby=true;
        }
        ++stats.cache_hits; ++stats.reused;
        return true;
    };
    auto observe=[&](int f,int j,int kind,const FVector& from,const FVector& to,bool clear,bool geometry_clear=false) {
        auto& cache=s.segment[f][j];
        auto& observation=cache.observation[kind];
        observation={}; observation.valid=true; observation.clear=clear;
        observation.geometry_clear=clear || geometry_clear;
        observation.from=from; observation.to=to; observation.radius_cm=Radius(requested,f,j);
        observation.age_seconds=0;
        auto& c=cache.contacts[kind];
        if(!clear && c.valid)
            observation.have_frame=frame_for(c.surface_id,observation.frame);
    };
    auto query=[&](int f,int j,int kind,const FVector& from,const FVector& to) {
        auto& cache=s.segment[f][j];
        auto& c=cache.contacts[kind];
        if(reuse(f,j,kind,from,to)) return true;
        if(!stats.available) {
            c={}; cache.observation[kind].valid=false; unresolved(f,j); return false;
        }
        if(stats.casts>=budget) { stats.budget_limited=true; unresolved(f,j); return false; }
        // Keep the old real feature as a direction hint until the new
        // observation is complete, even when it is too old for the solver.
        const auto previous=c;
        Hit hit;
        FVector trace_from=from;
        if(kind==3) {
            const auto& support=cache.contacts[0].valid ? cache.contacts[0] : previous;
            if(!support.valid) { unresolved(f,j); return false; }
            const double distance=Dot(to-support.point,support.normal);
            trace_from=to+support.normal*std::max(.1,Radius(requested,f,j)+.2-distance);
        }
        const auto status=sweep(trace_from,to,Radius(requested,f,j),hit);
        if(kind==0) stats.primary_hits+=status==QueryStatus::hit;
        else if(kind==1) ++stats.displayed_casts;
        else if(kind==2) ++stats.temporal_casts;
        else ++stats.feature_casts;
        if(status==QueryStatus::unavailable) {
            c={}; cache.observation[kind].valid=false; unresolved(f,j); return false;
        }
        if(status==QueryStatus::clear) {
            if(previous.valid && kind!=2 && !feature_checked[f][j][kind] && stats.casts<budget) {
                auto prior=previous;
                skel::Xf frame;
                const auto& old_observation=cache.observation[kind];
                if(old_observation.have_frame && frame_for(prior.surface_id,frame))
                    MoveFeature(prior,old_observation.frame,frame);
                const double fraction=Fraction(prior.point,from,to);
                const auto center=from+(to-from)*fraction;
                const auto delta=center-prior.point;
                if(std::fabs(Dot(delta,prior.normal)-Radius(requested,f,j))<=.10 &&
                   Length(Reject(delta,prior.normal))<=prior.patch_radius_cm) {
                    // A solver's small skin gap is still supported by the
                    // nearby real wall. Reprobe that feature past the gap;
                    // never keep it merely because an old plane existed.
                    Hit support;
                    ++stats.feature_casts;
                    const auto near=sweep(prior.point+prior.normal*(Radius(requested,f,j)+.2),
                                          center-prior.normal*.08,Radius(requested,f,j),support);
                    if(near==QueryStatus::hit && !support.initial_overlap &&
                       (!prior.surface_id || !support.surface_id || prior.surface_id==support.surface_id)) {
                        Store(c,support,j,fraction,Radius(requested,f,j),kind);
                        if(c.valid) { observe(f,j,kind,from,to,false,true); return true; }
                    }
                    if(near==QueryStatus::unavailable) {
                        c={}; cache.observation[kind].valid=false; unresolved(f,j); return false;
                    }
                }
            }
            c={}; observe(f,j,kind,from,to,true); return true;
        }
        const double fraction=kind>=2 ? 1.0 : Fraction(hit.center,from,to);
        if(!hit.initial_overlap) {
            Store(c,hit,j,fraction,Radius(requested,f,j),kind);
            if(c.valid) { observe(f,j,kind,from,to,false); return true; }
            cache.observation[kind].valid=false; unresolved(f,j); return false;
        }
        if(!Usable(hit)) {
            c={}; cache.observation[kind].valid=false; ++stats.failed_recovery;
            unresolved(f,j); return false;
        }
        // Recover before moving to the next digit. At the normal32cast cap
        // all15 overlapping phalanges can each acquire a real surface in
        // this frame, instead of waiting behind three competing work lists.
        if(stats.casts>=budget) {
            stats.budget_limited=true; unresolved(f,j); return false;
        }
        FVector outside=hit.center+Normalize(hit.normal)*.05;
        const finger_contact::Contact* prior=nullptr;
        for(const auto& candidate:cache.contacts)
            if(candidate.valid && hit.surface_id && candidate.surface_id==hit.surface_id &&
               (!prior || Priority(candidate)>Priority(*prior))) prior=&candidate;
        if(prior) {
            auto current=*prior;
            // A fresh transform is a side hint, not a substitute for impact.
            auto& old_observation=cache.observation[static_cast<size_t>(prior-cache.contacts.data())];
            skel::Xf frame;
            if(old_observation.have_frame && frame_for(current.surface_id,frame))
                MoveFeature(current,old_observation.frame,frame);
            outside=current.point+current.normal*(Radius(requested,f,j)+.2);
        } else if(kind==0 && s.have_rendered && s.rendered.valid[f]) {
            const auto old=s.rendered.point[f][j]+
                (s.rendered.point[f][j+1]-s.rendered.point[f][j])*fraction;
            // Starting from the displayed side prevents a thin solid's
            // far-side depenetration normal from reversing the contact.
            if(Length(old-hit.center)>.01) outside=old;
        }
        Hit recovered;
        auto recovery=sweep(outside,hit.center,Radius(requested,f,j),recovered);
        // Preserve enough budget for every remaining primary segment's
        // first cast/recovery. A deep/ambiguous overlap is explicitly
        // unresolved rather than borrowing an unbounded recovery budget.
        int primary_left=0;
        if(kind==0) {
            bool past=false;
            for(int index=0;index<kFingers*kSegments;++index) {
                const int other=(s.cursor+index)%(kFingers*kSegments);
                if(past && requested.valid[other/kSegments]) ++primary_left;
                if(other==f*kSegments+j) past=true;
            }
        }
        if(recovery==QueryStatus::hit && recovered.initial_overlap &&
           stats.casts+2*primary_left<budget) {
            outside=hit.center+Normalize(hit.normal)*(Radius(requested,f,j)+.05);
            recovery=sweep(outside,hit.center,Radius(requested,f,j),recovered);
        }
        if(recovery==QueryStatus::hit && recovered.initial_overlap &&
           stats.casts+2*primary_left<budget) {
            outside=hit.center+Normalize(hit.normal)*(2*Radius(requested,f,j)+3);
            recovery=sweep(outside,hit.center,Radius(requested,f,j),recovered);
        }
        c={};
        if(recovery==QueryStatus::hit && !recovered.initial_overlap &&
           (!hit.surface_id || !recovered.surface_id || hit.surface_id==recovered.surface_id)) {
            Store(c,recovered,j,fraction,Radius(requested,f,j),kind,true);
            if(c.valid) {
                ++stats.recovered_overlaps; observe(f,j,kind,from,to,false); return true;
            }
        }
        ++stats.failed_recovery;
        cache.observation[kind].valid=false;
        // A budget-starved fresh contact may bridge this frame, but its
        // original age remains authoritative; it is never rejuvenated.
        if(stats.casts>=budget && previous.valid && previous.age_seconds<max_age) c=previous;
        unresolved(f,j);
        return false;
    };
    bool primary_ok[kFingers][kSegments]{};
    for(int k=0;k<kFingers*kSegments;++k) {
        const int index=(s.cursor+k)%(kFingers*kSegments),f=index/kSegments,j=index%kSegments;
        if(!requested.valid[f]) { s.segment[f][j]={}; continue; }
        primary_ok[f][j]=query(f,j,0,requested.point[f][j],requested.point[f][j+1]);
        if(!stats.available) break;
    }
    // Displayed geometry is a separate bounded observation. Temporal sweeps
    // are unnecessary when a fresh spatial plane already stops the proposed
    // motion from the displayed side. This avoids three copies of one wall.
    constexpr int work_count=kFingers*kSegments*3;
    const int work_start=s.work_cursor;
    bool stopped=false;
    if(stats.available) for(int k=0;k<work_count;++k) {
        const int work=(work_start+k)%work_count,index=work/3,kind=work%3==0 ? 3 : work%3==1 ? 2 : 1;
        const int f=index/kSegments,j=index%kSegments;
        auto& cache=s.segment[f][j];
        if(!requested.valid[f] || (kind!=3 && (!s.have_rendered || !s.rendered.valid[f]))) {
            cache.contacts[kind]={}; cache.observation[kind]={}; continue;
        }
        const auto& old_a=s.rendered.point[f][j];
        const auto& old_b=s.rendered.point[f][j+1];
        const auto& new_a=requested.point[f][j];
        const auto& new_b=requested.point[f][j+1];
        bool needed=kind==1 ? std::max(Length(old_a-new_a),Length(old_b-new_b))>epsilon :
                             Length(old_b-new_b)>epsilon;
        const auto& primary=cache.contacts[0];
        if(kind==3) {
            needed=false;
            if(primary_ok[f][j] && primary.valid && primary.age_seconds<max_age) {
                const auto seed=new_a+(new_b-new_a)*primary.fraction;
                const auto delta=new_b-primary.point;
                const double endpoint_depth=Radius(requested,f,j)-Dot(delta,primary.normal);
                const double seed_depth=Radius(requested,f,j)-Dot(seed-primary.point,primary.normal);
                // A local first-touch plane may miss a substantially deeper
                // distal end beyond its finite tangent patch. Measure that
                // endpoint against real geometry; never widen the old patch.
                needed=endpoint_depth>.02 && endpoint_depth>seed_depth+.02 &&
                       Length(Reject(delta,primary.normal))>primary.patch_radius_cm;
            }
            const auto& end=cache.contacts[3];
            if(!needed && end.valid) {
                const auto delta=new_b-end.point;
                needed=std::fabs(Dot(delta,end.normal)-Radius(requested,f,j))<=.10 &&
                       Length(Reject(delta,end.normal))<=end.patch_radius_cm;
            }
        }
        if(kind==2 && primary_ok[f][j] && primary.valid && primary.age_seconds<max_age) {
            const auto old=old_a+(old_b-old_a)*primary.fraction;
            const auto delta=old-primary.point;
            if(Dot(delta,primary.normal)>=Radius(requested,f,j)-.05 &&
               Length(Reject(delta,primary.normal))<=primary.patch_radius_cm) needed=false;
        }
        if(!needed) { cache.contacts[kind]={}; cache.observation[kind]={}; continue; }
        const bool ok=kind==3 ? query(f,j,kind,new_a,new_b) :
                      kind==1 ? query(f,j,kind,old_a,old_b) : query(f,j,kind,old_b,new_b);
        if(!ok && !stopped) { s.work_cursor=work; stopped=true; }
        if(!stopped) s.work_cursor=(work+1)%work_count;
        if(!stats.available) break;
    }
    s.cursor=(s.cursor+1)%(kFingers*kSegments);
    if(!stats.available) {
        for(auto& digit:s.segment) for(auto& cache:digit) cache={};
        for(int f=0;f<kFingers;++f) if(requested.valid[f]) {
            stats.unresolved_mask|=1u<<f; stats.unresolved_segments|=7u<<(f*kSegments);
        }
        return finish();
    }
    stats.geometry_clear=true;
    for(int f=0;f<kFingers;++f) if(requested.valid[f]) for(int j=0;j<kSegments;++j) {
        const auto& observed=s.segment[f][j].observation[0];
        // The looser movement threshold permits constraint reuse, not a
        // certificate for a different final capsule. Endpoint verification
        // can spend the saved casts on the actual geometry in that case.
        stats.geometry_clear=stats.geometry_clear && primary_ok[f][j] && observed.geometry_clear &&
            Length(requested.point[f][j]-observed.from)<=.001 &&
            Length(requested.point[f][j+1]-observed.to)<=.001;
    }
    bool have_contact=false;
    for(int f=0;f<kFingers;++f) for(int j=0;j<kSegments;++j) {
        auto& cache=s.segment[f][j];
        for(auto& c:cache.contacts) {
            if(!c.valid) continue;
            if(c.age_seconds>=max_age) { ++stats.expired; c={}; continue; }
            bool conflict=false;
            for(const auto& other:cache.contacts)
                if(other.valid && other.age_seconds<max_age && Priority(other)>Priority(c) && Incompatible(c,other)) {
                    conflict=true; ++stats.conflicting_contacts; break;
                }
            if(conflict || output[f].count>=finger_contact::kMaxContacts) continue;
            output[f].items[output[f].count++]=c; have_contact=true;
        }
    }
    s.broad_clear=!have_contact && !stats.unresolved_mask;
    return finish();
}
}  // namespace holevr::vr::finger_world_query
