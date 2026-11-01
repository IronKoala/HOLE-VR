#pragma once
#include <cstdint>
#include <string>

namespace holevr::ue {

// FName as stored in UE 5.5 shipping builds (no case-preserving display index).
struct FName {
    uint32_t comparison_index = 0;
    uint32_t number = 0;
};

namespace names {
// Locates the engine's global FNamePool by scanning the exe's data sections
// for a block whose first entries are "None" and "ByteProperty". Those are
// always the first two names the engine registers, in every UE5 build, so
// this does not depend on code signatures that change between game patches.
bool Init();
bool Ready();
uintptr_t PoolAddress();

// Plain string for a name entry, without the _N number suffix.
std::string EntryToString(uint32_t comparison_index);
// Full display string, e.g. "Bullet_3".
std::string ToString(const FName& name);
}  // namespace names

}  // namespace holevr::ue
