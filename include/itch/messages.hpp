// Decoders for the ITCH 5.0 messages that touch the order book.
// Byte offsets are from the start of the message (the type byte), per DESIGN.md §10.4.
// Each decode_* takes a pointer to the type byte and returns a plain struct.
#pragma once

#include "itch/endian.hpp"

#include <cstdint>
#include <cstring>  // memcpy

namespace itch {

// Common header, present in every message: type(1) locate(2) tracking(2) timestamp(6).
struct Header {
    char type;
    uint16_t locate;      // per-day stock code, announced by a Stock Directory 'R'
    uint64_t timestamp;   // ns since midnight
};

inline Header decode_header(const uint8_t* m) {
    return {char(m[0]), load_be16(m + 1), load_be48(m + 5)};
}

// 'R' Stock Directory: maps a locate code to its ticker.
struct StockDirectory {
    uint16_t locate;
    char symbol[9];       // 8 chars, space-padded, plus a '\0' so it prints as a C string
};

inline StockDirectory decode_stock_directory(const uint8_t* m) {
    StockDirectory r{};                // {} zeroes it, so symbol[8] is already '\0'
    r.locate = load_be16(m + 1);
    memcpy(r.symbol, m + 11, 8);       // copy 8 raw bytes
    return r;
}

// 'A' Add Order and 'F' Add Order with MPID (F = A + 4-byte attribution we ignore).
struct AddOrder {
    uint16_t locate;
    uint64_t ref;         // order reference number: the order's ID for the rest of the day
    char side;            // 'B' buy or 'S' sell
    uint32_t shares;
    uint32_t price;       // 4 implied decimals: 1000200 = 100.02
};

inline AddOrder decode_add(const uint8_t* m) {
    return {load_be16(m + 1), load_be64(m + 11), char(m[19]),
            load_be32(m + 20), load_be32(m + 32)};
}

// 'E' Executed, 'C' Executed with Price, 'X' Cancel: all reduce an order's shares.
// The shares field sits at the same offset (19) in all three.
struct Reduce {
    uint64_t ref;
    uint32_t shares;      // how many shares to remove
};

inline Reduce decode_reduce(const uint8_t* m) {
    return {load_be64(m + 11), load_be32(m + 19)};
}

// 'D' Delete: remove the whole order.
inline uint64_t decode_delete_ref(const uint8_t* m) {
    return load_be64(m + 11);
}

// 'U' Replace: old order disappears, new one appears with the same side and stock.
struct Replace {
    uint64_t old_ref;
    uint64_t new_ref;
    uint32_t shares;
    uint32_t price;
};

inline Replace decode_replace(const uint8_t* m) {
    return {load_be64(m + 11), load_be64(m + 19), load_be32(m + 27), load_be32(m + 31)};
}

}  // namespace itch
