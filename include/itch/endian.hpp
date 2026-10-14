// Big-endian loads. ITCH stores the most significant byte first; x86 expects it
// last, so bytes 00 27 must be assembled by hand to get 39 (not 9984).
// No `using namespace std;` here: headers are pasted into every includer.
#pragma once  // ignore repeat #includes of this file

#include <cstdint>  // uint8_t..uint64_t: exact-width integer types

namespace itch {  // call as itch::load_be16(...)

// inline: allowed to be defined in a header included by many .cpp files.
// p[0] is the first byte, p[1] the next.
inline uint16_t load_be16(const uint8_t* p) {
    return uint16_t(p[0]) << 8 | uint16_t(p[1]);  // 0xAB<<8 = 0xAB00; | 0xCD = 0xABCD
}

inline uint32_t load_be32(const uint8_t* p) {
    return uint32_t(load_be16(p)) << 16 | load_be16(p + 2);
}

// 6 bytes: ITCH timestamps (ns since midnight). No 48-bit type, so uint64_t.
inline uint64_t load_be48(const uint8_t* p) {
    return uint64_t(load_be16(p)) << 32 | load_be32(p + 2);
}

// 8 bytes: ITCH order reference numbers.
inline uint64_t load_be64(const uint8_t* p) {
    return uint64_t(load_be32(p)) << 32 | load_be32(p + 4);
}

}  // namespace itch
