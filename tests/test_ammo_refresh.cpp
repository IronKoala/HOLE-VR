#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <string>
#include "game/ammo_refresh.h"
#include "game/attachment_transaction.h"
#include "vr/workbench_rules.h"
using namespace holevr;
namespace refresh = game::custom::ammo_refresh;
using ue::reflection::PropertyInfo;
static int errors = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL %d: %s\n", __LINE__, #x); ++errors; } } while (0)

struct Object {
    std::map<std::string, PropertyInfo> fields;
    std::array<unsigned char, 0x500> bytes{};
    PropertyInfo Find(const char* name) const {
        auto i = fields.find(name);
        return i == fields.end() ? PropertyInfo{} : i->second;
    }
    void Field(const char* name, const char* type, int offset, int size) {
        PropertyInfo p; p.name = name; p.type = type; p.offset = offset;
        p.element_size = size; p.array_dim = 1; fields[name] = p;
    }
    PropertyInfo Muzzle() const { return refresh::MuzzleProperty([&](const char* name) { return Find(name); }); }
    bool Retarget(uintptr_t pointer) {
        auto p = Muzzle(); if (!p.valid()) return false;
        std::memcpy(bytes.data() + p.offset, &pointer, sizeof(pointer)); return true;
    }
    uintptr_t Pointer(int at) const {
        uintptr_t value = 0; std::memcpy(&value, bytes.data() + at, sizeof(value)); return value;
    }
};

int main() {
    CHECK(refresh::MagazineLayoutValid("BP_HG_C", 0));
    CHECK(refresh::MagazineLayoutValid("BP_AR_C", 0));
    CHECK(refresh::MagazineLayoutValid("BP_SG_C", 1));
    CHECK(refresh::MagazineLayoutValid("BP_SR_C", 1));
    CHECK(!refresh::MagazineLayoutValid("BP_SR_C", 0));
    CHECK(!refresh::MagazineLayoutValid("BP_UNKNOWN_C", 0));
    CHECK(!refresh::MagazineLayoutValid("BP_HG_C", 2));
    // Recorded from HOLE's actual game_classes.txt, not an assumed common
    // schema. The magazine and round spell the property differently.
    Object magazine, round, bullet;
    magazine.Field("MuzzlePoint", "ObjectProperty", 0x3a8, 8);
    round.Field("Muzzle Point", "ObjectProperty", 0x428, 8);
    bullet.Field("MuzzlePoint", "ObjectProperty", 0x348, 8);
    CHECK(!round.Find("MuzzlePoint").valid()); // reproduces the old rejection
    CHECK(magazine.Muzzle().offset == 0x3a8);
    CHECK(round.Muzzle().offset == 0x428);
    CHECK(bullet.Muzzle().offset == 0x348);
    // Preserve a spent-state sentinel, magazine-map bytes and nearby fields.
    round.bytes[0x468] = 1; magazine.bytes[0x338] = 19;
    const auto original_round = round.bytes, original_mag = magazine.bytes;
    CHECK(magazine.Retarget(0x12345678)); CHECK(round.Retarget(0x12345678)); CHECK(bullet.Retarget(0x12345678));
    CHECK(magazine.Pointer(0x3a8) == 0x12345678 && round.Pointer(0x428) == 0x12345678 && bullet.Pointer(0x348) == 0x12345678);
    for (size_t i = 0; i < round.bytes.size(); ++i) {
        if (i < 0x428 || i >= 0x430) CHECK(round.bytes[i] == original_round[i]);
        if (i < 0x3a8 || i >= 0x3b0) CHECK(magazine.bytes[i] == original_mag[i]);
    }

    // Wrong reflected layouts still fail closed instead of writing a guess.
    for (int bad = 0; bad < 5; ++bad) {
        Object o = round; auto& p = o.fields["Muzzle Point"];
        if (bad == 0) p.type = "StructProperty";
        if (bad == 1) p.element_size = 4;
        if (bad == 2) p.array_dim = 2;
        if (bad == 3) p.offset = -1;
        if (bad == 4) o.fields.clear();
        const auto before = o.bytes;
        CHECK(!o.Muzzle().valid()); CHECK(!o.Retarget(0xabcdef)); CHECK(o.bytes == before);
    }

    // The reported path: a removable light released away from its slot.
    // This validates the real release, property-resolution and transaction
    // helpers together. Engine dispatch/save persistence still need VR QA.
    vr::workbench::rules::LetGoFacts facts;
    facts.from_gun = facts.have_gun = facts.slot_can_be_empty = true;
    facts.from_slot = 7; facts.snapped = -1;
    CHECK(vr::workbench::rules::DecideLetGo(facts) == vr::workbench::rules::LetGo::kEmptySlot);
    std::string saved_light = "LIGHT_HG_LIGHT", fitted_light = saved_light;
    CHECK(magazine.Muzzle().valid() && round.Muzzle().valid() && bullet.Muzzle().valid());
    const auto result = game::custom::transaction::Run(
        [&] { saved_light = "Empty"; return true; },
        [&] { fitted_light.clear(); return magazine.Retarget(0x9876) && round.Retarget(0x9876) && bullet.Retarget(0x9876); },
        [&] { fitted_light = "LIGHT_HG_LIGHT"; return true; },
        [&] { saved_light = "LIGHT_HG_LIGHT"; return true; });
    CHECK(result == game::custom::transaction::Result::kApplied);
    CHECK(saved_light == "Empty" && fitted_light.empty());
    CHECK(round.bytes[0x468] == 1 && magazine.bytes[0x338] == 19);

    // Cached projectile values use each round's own ammo contributions, so
    // mixed shells keep their differences after fitting/removing a muzzle.
    refresh::Values weapon{3,4,5,6,7,8,2.4,0.75};
    for (const refresh::Values ammo : {refresh::Values{20,10,1,2,3,4,1.5,1.25},
                                       refresh::Values{8,9,2,3,4,5,2.8,3.25}}) {
        refresh::Values actual{}; CHECK(refresh::ProjectileValues(weapon, ammo, &actual));
        for (size_t i = 0; i < actual.size(); ++i)
            CHECK(actual[i] == (i == refresh::kPenetration ? std::trunc(weapon[i] + ammo[i]) : weapon[i] + ammo[i]));
    }
    refresh::Values invalid{}, untouched{}; untouched.fill(99);
    for (double bad : {std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN(), 1e40}) {
        invalid[refresh::kPenetration] = bad; auto result_values = untouched;
        CHECK(!refresh::ProjectileValues(weapon, invalid, &result_values)); CHECK(result_values == untouched);
    }
    std::printf("HOLE retained-ammo schema and light-removal regression: %d failures\n", errors);
    return errors ? 1 : 0;
}
