#pragma once
// Stored hand-on-object grips (Into the Radius 1's format): where the hand
// sits relative to the object, plus a target point for each fingertip, all
// in the object's frame. Fingers are solved to the targets once
// (finger_solve.h) and then held; only the trigger finger is live, blending
// between three stored states (off the trigger along the frame, resting on
// the trigger, pulled).
//
// Records are plain text files in hole-vr/grips/, written by the in-headset
// grip editor, so a grip is tuned by eye and kept. A gun with no file uses
// the built-in pistol record below.
//
// Object frames: gun = X barrel, Y right, Z up, origin the game's
// MainHandPosition (cm). The hand is described anatomically (middle
// knuckle, wrist->knuckle direction, pinky->index direction), so a record
// does not depend on the skeleton's bone axes.
//
// Pure logic, unit tested in tests/test_body.cpp.

#include <string>
#include <vector>

#include "vr/body/ik_math.h"

namespace holevr::vr::grip_record {

enum Tip : int { kThumb = 0, kIndexOff, kIndexRest, kIndexPull, kMiddle, kRing, kPinky, kTipCount };
const char* TipName(int t);

// A box of the object's outline, for the clip check: centre, half sizes,
// pitched about the object's Y axis (degrees, + = top tilts forward).
struct OutlineBox {
    std::string name;
    FVector centre, half;
    double pitch_deg = 0;
};

struct Record {
    std::string name;
    bool right_hand = true;
    FVector knuckle;      // middle finger knuckle
    FVector fingers;      // wrist -> middle knuckle direction
    FVector across;       // pinky knuckle -> index knuckle direction
    FVector tip[kTipCount];
    std::vector<OutlineBox> outline;
};

// The built-in right-hand pistol grip, from the owner's reference photos
// (high grip, thumb forward along the left side of the frame, index straight
// along the right side above the trigger guard) and the measured Glock-style
// pistol (scan 2026-11-01: grip 5.6 cm deep, 3.1 wide, trigger guard bottom
// at 7.8 cm, slant 13 degrees).
Record DefaultPistol();
// The built-in pistol hand placement (knuckle, fingers, across).
void PistolHand(Record* r);
// True for a saved grip that still has the first built-in hand (before
// 2026-11-02), so the loader can move it to the new one and keep its tips.
bool HasOldPistolHand(const Record& r);

std::string Serialize(const Record& r);
// Fills *r from text; keys missing from the text keep their current values.
bool Parse(const std::string& text, Record* r);

// The same grip for the other hand (mirror across the object's X/Z plane).
Record Mirror(const Record& r);

// The hand's anatomy in hand-bone space.
struct HandGeo {
    FVector knuckle;  // middle knuckle from the wrist
    FVector fingers;  // wrist -> middle knuckle direction
    FVector across;   // pinky -> index direction
};
// Hand bone transform (object frame) for a record.
void HandInObject(const Record& r, const HandGeo& g, FQuat* rot, FVector* pos);
// The reverse: store a hand bone transform (object frame) in the record.
void StoreHand(Record* r, const HandGeo& g, const FQuat& rot, const FVector& pos);

// How deep a point (object frame) is inside the outline, cm (0 = outside).
double Depth(const Record& r, const FVector& p, std::string* box = nullptr);

}  // namespace holevr::vr::grip_record
