// C++ model of the order ID cache (DESIGN.md §2, §4).
// Used by the simulator (src/cachesim.cpp) and the tests (test/).
// Header, so std:: is written out (no `using namespace` in headers).
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace itch {

// What the cache is asked to do, in order.
enum OpKind : uint8_t {
    ADD,     // new order: allocate a slot
    TOUCH,   // partial execute/cancel: look up, update shares, keep
    REMOVE,  // delete / full execute / old half of a replace: look up, free slot
};

struct Op {
    OpKind kind;
    uint64_t ref;
    bool after_replace;  // ADD that is the new half of a replace (needs the old one's hit)
};

enum Policy { LRU, PLRU, RANDOM };
inline const char* policy_name[] = {"LRU", "pLRU", "random"};

struct Config {
    int capacity;   // total entries (power of 2)
    int ways;       // entries per set (power of 2)
    int tag_bits;   // stored tag width; 64 = full, no aliasing possible
    Policy policy;
};

struct Result {
    uint64_t adds = 0;
    uint64_t evictions = 0;      // a valid order thrown out to make room: permanently lost
    uint64_t misses = 0;         // lookup found nothing (the order was evicted earlier)
    uint64_t aliases = 0;        // lookup "hit" a DIFFERENT order with the same tag: wrong data
    uint64_t lost_replaces = 0;  // replace whose old order had gone, so the new one can't be added
};

struct Entry {
    bool valid = false;
    uint64_t tag = 0;
    uint64_t true_ref = 0;    // full ID, kept only so the simulator can detect aliasing
    uint64_t last_used = 0;   // for true LRU
};

class Cache {
public:
    explicit Cache(const Config& c)  // explicit: no accidental conversion from Config
        : cfg(c), sets(c.capacity / c.ways),
          entries(std::size_t(c.capacity)), plru_bits(std::size_t(c.capacity), 0) {
        index_bits = 0;
        while ((1 << index_bits) < sets) ++index_bits;  // log2(sets)
    }

    void run(const std::vector<Op>& ops, Result& r) {
        bool last_remove_hit = true;
        for (const Op& op : ops) {
            ++tick;
            if (op.kind == ADD) {
                if (op.after_replace && !last_remove_hit) { ++r.lost_replaces; continue; }
                ++r.adds;
                insert(op.ref, r);
            } else {
                int way = lookup(op.ref, r);
                last_remove_hit = way >= 0;
                if (way < 0) continue;
                touch(set_of(op.ref), way);
                if (op.kind == REMOVE) slot(set_of(op.ref), way).valid = false;
            }
        }
    }

    // For tests: is this exact order in the cache right now?
    bool holds(uint64_t ref) const {
        int set = set_of(ref);
        for (int w = 0; w < cfg.ways; ++w) {
            const Entry& e = entries[std::size_t(set) * std::size_t(cfg.ways) + std::size_t(w)];
            if (e.valid && e.true_ref == ref) return true;
        }
        return false;
    }

    // Index = low bits of the order ID; tag = the bits above, cut to tag_bits.
    int set_of(uint64_t ref) const { return int(ref & uint64_t(sets - 1)); }
    uint64_t tag_of(uint64_t ref) const {
        uint64_t t = ref >> index_bits;
        return cfg.tag_bits >= 64 ? t : t & ((uint64_t(1) << cfg.tag_bits) - 1);
    }

private:
    Config cfg;
    int sets;
    int index_bits;
    std::vector<Entry> entries;       // sets * ways, set-major
    std::vector<uint8_t> plru_bits;   // ways-1 tree bits per set (stored with stride `ways`)
    uint64_t tick = 0;
    uint32_t lfsr = 0xACE1u;          // random-policy state; fixed seed = repeatable runs

    Entry& slot(int set, int way) {
        return entries[std::size_t(set) * std::size_t(cfg.ways) + std::size_t(way)];
    }

    // Compare all ways' tags (the hardware does this in parallel). -1 = miss.
    int lookup(uint64_t ref, Result& r) {
        int set = set_of(ref);
        uint64_t tag = tag_of(ref);
        for (int w = 0; w < cfg.ways; ++w) {
            Entry& e = slot(set, w);
            if (e.valid && e.tag == tag) {
                if (e.true_ref != ref) ++r.aliases;  // hardware can't tell; we can
                return w;
            }
        }
        ++r.misses;
        return -1;
    }

    void insert(uint64_t ref, Result& r) {
        int set = set_of(ref);
        int way = -1;
        for (int w = 0; w < cfg.ways; ++w)       // invalid way first (§4.2)
            if (!slot(set, w).valid) { way = w; break; }
        if (way < 0) {                           // set full: pick a victim
            way = victim(set);
            ++r.evictions;
        }
        Entry& e = slot(set, way);
        e.valid = true;
        e.tag = tag_of(ref);
        e.true_ref = ref;
        touch(set, way);
    }

    int victim(int set) {
        switch (cfg.policy) {
            case LRU: {  // the way used longest ago
                int best = 0;
                for (int w = 1; w < cfg.ways; ++w)
                    if (slot(set, w).last_used < slot(set, best).last_used) best = w;
                return best;
            }
            case PLRU: {  // walk the tree: each bit points toward the staler half (§4.3)
                uint8_t* bits = &plru_bits[std::size_t(set) * std::size_t(cfg.ways)];
                int node = 0;
                while (node < cfg.ways - 1) node = 2 * node + 1 + bits[node];
                return node - (cfg.ways - 1);  // leaf position -> way number
            }
            case RANDOM: {  // 16-bit Galois LFSR: the one-register hardware version
                lfsr = (lfsr >> 1) ^ (-(lfsr & 1u) & 0xB400u);
                return int(lfsr % uint32_t(cfg.ways));
            }
        }
        return 0;
    }

    void touch(int set, int way) {
        slot(set, way).last_used = tick;
        // pLRU: flip the bits on this way's path to point AWAY from it.
        uint8_t* bits = &plru_bits[std::size_t(set) * std::size_t(cfg.ways)];
        int node = way + cfg.ways - 1;           // leaf
        while (node > 0) {
            int parent = (node - 1) / 2;
            bool came_from_right = node == 2 * parent + 2;
            bits[parent] = came_from_right ? 0 : 1;  // 0 = victim on the left
            node = parent;
        }
    }
};

inline Result simulate(const std::vector<Op>& ops, const Config& c) {
    Result r;
    Cache cache(c);
    cache.run(ops, r);
    return r;
}

}  // namespace itch
