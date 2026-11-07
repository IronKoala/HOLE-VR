#include "ue/names.h"

#include <cstring>
#include <vector>

#include "core/log.h"
#include "core/memory.h"

namespace holevr::ue::names {
namespace {

// FNamePool layout (UE 5.x, 64-bit):
//   0x00 FRWLock      Lock            (8 bytes on Windows: SRWLOCK)
//   0x08 uint32       CurrentBlock
//   0x0C uint32       CurrentByteCursor
//   0x10 uint8*       Blocks[8192]
constexpr uintptr_t kBlocksOffset = 0x10;
constexpr uint32_t kMaxBlocks = 8192;
constexpr uint32_t kStride = 2;  // alignof(FNameEntry)

uintptr_t g_pool = 0;

// FNameEntryHeader (no case preserving): bIsWide:1, LowercaseProbeHash:5, Len:10
struct Header {
    bool wide;
    uint16_t len;
};
inline Header Decode(uint16_t raw) { return {static_cast<bool>(raw & 1), static_cast<uint16_t>(raw >> 6)}; }

bool BlockLooksLikeFirst(uintptr_t block) {
    if (!mem::LooksLikePointer(block) || mem::Game().Contains(block)) return false;
    unsigned char buf[20];
    if (!mem::IsReadable(block, sizeof(buf))) return false;
    std::memcpy(buf, reinterpret_cast<const void*>(block), sizeof(buf));
    uint16_t h0, h1;
    std::memcpy(&h0, buf, 2);
    std::memcpy(&h1, buf + 6, 2);
    Header a = Decode(h0), b = Decode(h1);
    return !a.wide && a.len == 4 && std::memcmp(buf + 2, "None", 4) == 0 &&
           !b.wide && b.len == 12 && std::memcmp(buf + 8, "ByteProperty", 12) == 0;
}

bool ScanSection(const mem::Section& s) {
    uintptr_t end = s.start + s.size - sizeof(uintptr_t);
    for (uintptr_t p = (s.start + 7) & ~uintptr_t(7); p < end; p += 8) {
        uintptr_t block = *reinterpret_cast<const uintptr_t*>(p);
        if (!BlockLooksLikeFirst(block)) continue;
        uintptr_t pool = p - kBlocksOffset;
        uint32_t current_block = mem::ReadOr<uint32_t>(pool + 0x08, ~0u);
        if (current_block >= kMaxBlocks) continue;
        g_pool = pool;
        log::Info("FNamePool found at exe+0x%llx (current block %u)",
                  static_cast<unsigned long long>(pool - mem::Game().base), current_block);
        return true;
    }
    return false;
}

}  // namespace

bool Init() {
    if (g_pool) return true;
    for (const auto& s : mem::Game().sections) {
        if (s.name == ".data" && ScanSection(s)) return true;
    }
    return false;
}

bool Ready() { return g_pool != 0; }
uintptr_t PoolAddress() { return g_pool; }

std::string EntryToString(uint32_t comparison_index) {
    if (!g_pool) return {};
    uint32_t block = comparison_index >> 16;
    uint32_t offset = comparison_index & 0xFFFF;
    if (block >= kMaxBlocks) return {};
    uintptr_t block_ptr = mem::ReadOr<uintptr_t>(g_pool + kBlocksOffset + block * 8ull);
    if (!block_ptr) return {};
    uintptr_t entry = block_ptr + offset * kStride;
    uint16_t raw;
    if (!mem::Read(entry, raw)) return {};
    Header h = Decode(raw);
    if (h.len == 0 || h.len > 1024) return {};
    if (!h.wide) {
        if (!mem::IsReadable(entry + 2, h.len)) return {};
        return std::string(reinterpret_cast<const char*>(entry + 2), h.len);
    }
    if (!mem::IsReadable(entry + 2, h.len * 2ull)) return {};
    // Wide names are rare (localized asset names). Keep ASCII, replace the rest.
    std::string out;
    out.reserve(h.len);
    auto* w = reinterpret_cast<const char16_t*>(entry + 2);
    for (uint16_t i = 0; i < h.len; ++i) out.push_back(w[i] < 0x80 ? static_cast<char>(w[i]) : '?');
    return out;
}

std::string ToString(const FName& name) {
    std::string s = EntryToString(name.comparison_index);
    if (name.number > 0) s += "_" + std::to_string(name.number - 1);
    return s;
}

bool Find(const std::string& text, FName* out) {
    if (!g_pool || text.empty() || text.size() > 1024) return false;
    constexpr uint32_t kBlockBytes = 0x10000u * kStride;
    uint32_t current_block = mem::ReadOr<uint32_t>(g_pool + 0x08, 0);
    uint32_t current_cursor = mem::ReadOr<uint32_t>(g_pool + 0x0C, 0);
    if (current_block >= kMaxBlocks) return false;
    std::vector<unsigned char> buf;
    for (uint32_t block = 0; block <= current_block; ++block) {
        uintptr_t base = mem::ReadOr<uintptr_t>(g_pool + kBlocksOffset + block * 8ull);
        if (!base) continue;
        uint32_t limit = block == current_block ? current_cursor : kBlockBytes;
        if (limit > kBlockBytes || !mem::IsReadable(base, limit)) continue;
        buf.resize(limit);
        std::memcpy(buf.data(), reinterpret_cast<const void*>(base), limit);
        uint32_t off = 0;
        while (off + 2 <= limit) {
            uint16_t raw;
            std::memcpy(&raw, buf.data() + off, 2);
            Header h = Decode(raw);
            if (h.len == 0) break;
            uint32_t bytes = h.wide ? h.len * 2u : h.len;
            if (off + 2 + bytes > limit) break;
            if (!h.wide && h.len == text.size() && std::memcmp(buf.data() + off + 2, text.data(), h.len) == 0) {
                out->comparison_index = (block << 16) | (off / kStride);
                out->number = 0;
                return true;
            }
            off += (2 + bytes + kStride - 1) & ~(kStride - 1);
        }
    }
    return false;
}

}  // namespace holevr::ue::names
