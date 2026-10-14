// Cache simulator: replays the tracked symbols' orders through a model of the
// order ID cache (DESIGN.md §2, §4) and counts what goes wrong. Produces the
// phase 0 numbers in §7: ways, capacity, replacement policy, tag width.
//
// Two passes:
//   1. Parse the file once. Keep only tracked symbols. An unlimited "golden"
//      map turns each message into a cache operation: ADD, TOUCH, REMOVE.
//   2. Replay those operations through each cache design. Fast: no parsing.
//
// Usage: ./build/cachesim data/20190730.BX_ITCH_50 SPY IWM ... (tickers to track)

#include "itch/endian.hpp"
#include "itch/messages.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

using namespace std;
using namespace itch;

namespace {

// ---------------------------------------------------------------------------
// Pass 1 output: what the cache is asked to do, in order.
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// The cache model.
// ---------------------------------------------------------------------------

enum Policy { LRU, PLRU, RANDOM };
const char* policy_name[] = {"LRU", "pLRU", "random"};

struct Config {
    int capacity;   // total entries
    int ways;       // entries per set (power of 2)
    int tag_bits;   // stored tag width; 64 = full, no aliasing possible
    Policy policy;
};

struct Result {
    uint64_t adds = 0;
    uint64_t evictions = 0;   // a valid order thrown out to make room: permanently lost
    uint64_t misses = 0;      // lookup found nothing (the order was evicted earlier)
    uint64_t aliases = 0;     // lookup "hit" a DIFFERENT order with the same tag: wrong data
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
          entries(size_t(c.capacity)), plru_bits(size_t(sets) * size_t(c.ways), 0) {
        index_bits = 0;
        while ((1 << index_bits) < sets) ++index_bits;  // log2(sets)
    }

    void run(const vector<Op>& ops, Result& r) {
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

private:
    Config cfg;
    int sets;
    int index_bits;
    vector<Entry> entries;          // sets * ways, set-major
    vector<uint8_t> plru_bits;      // ways-1 tree bits per set (stored with stride `ways`)
    uint64_t tick = 0;
    uint32_t lfsr = 0xACE1u;        // random-policy state; fixed seed = repeatable runs

    // Index = low bits of the order ID; tag = the bits above, cut to tag_bits.
    int set_of(uint64_t ref) const { return int(ref & uint64_t(sets - 1)); }
    uint64_t tag_of(uint64_t ref) const {
        uint64_t t = ref >> index_bits;
        return cfg.tag_bits >= 64 ? t : t & ((uint64_t(1) << cfg.tag_bits) - 1);
    }
    Entry& slot(int set, int way) { return entries[size_t(set) * size_t(cfg.ways) + size_t(way)]; }

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
                uint8_t* bits = &plru_bits[size_t(set) * size_t(cfg.ways)];
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
        uint8_t* bits = &plru_bits[size_t(set) * size_t(cfg.ways)];
        int node = way + cfg.ways - 1;           // leaf
        while (node > 0) {
            int parent = (node - 1) / 2;
            bool came_from_right = node == 2 * parent + 2;
            bits[parent] = came_from_right ? 0 : 1;  // 0 = victim on the left
            node = parent;
        }
    }
};

Result simulate(const vector<Op>& ops, const Config& c) {
    Result r;
    Cache cache(c);
    cache.run(ops, r);
    return r;
}

void print_header() {
    printf("  %8s %5s %5s %7s %10s %10s %8s %9s %10s\n", "capacity", "ways", "tag",
           "policy", "evictions", "% of adds", "misses", "aliases", "lost repl");
}

void print_row(const Config& c, const Result& r) {
    char tag[12];
    if (c.tag_bits >= 64) snprintf(tag, sizeof tag, "full");
    else snprintf(tag, sizeof tag, "%d", c.tag_bits);
    printf("  %8d %5d %5s %7s %10llu %9.3f%% %8llu %9llu %10llu\n", c.capacity, c.ways, tag,
           policy_name[c.policy], (unsigned long long)r.evictions,
           100.0 * double(r.evictions) / double(r.adds), (unsigned long long)r.misses,
           (unsigned long long)r.aliases, (unsigned long long)r.lost_replaces);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <capture-file> TICKER [TICKER ...]\n", argv[0]);
        return 2;
    }
    int fd = open(argv[1], O_RDONLY);
    if (fd < 0) { perror(argv[1]); return 1; }
    struct stat st;
    if (fstat(fd, &st) < 0) { perror("fstat"); return 1; }
    size_t size = size_t(st.st_size);
    auto* data = static_cast<const uint8_t*>(
        mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0));
    if (data == MAP_FAILED) { perror("mmap"); return 1; }
    madvise(const_cast<uint8_t*>(data), size, MADV_SEQUENTIAL);

    // ---- Pass 1: file -> list of cache operations for tracked symbols ----
    vector<string> wanted(argv + 2, argv + argc);
    vector<bool> tracked(65536, false);
    int found = 0;
    unordered_map<uint64_t, uint32_t> golden;  // ref -> shares left; never evicts
    vector<Op> ops;
    ops.reserve(4'000'000);

    size_t pos = 0;
    while (pos + 2 <= size) {
        uint16_t len = load_be16(data + pos);
        if (pos + 2 + len > size) break;
        const uint8_t* m = data + pos + 2;
        Header h = decode_header(m);
        pos += 2 + len;

        if (h.type == 'R') {
            StockDirectory r = decode_stock_directory(m);
            string sym = r.symbol;
            sym.erase(sym.find_last_not_of(' ') + 1);
            if (find(wanted.begin(), wanted.end(), sym) != wanted.end()) {
                tracked[r.locate] = true;
                ++found;
            }
            continue;
        }
        if (!tracked[h.locate]) continue;  // the symbol filter (§3.3, §10.4): every type has a locate

        switch (h.type) {
            case 'A':
            case 'F': {
                AddOrder a = decode_add(m);
                golden[a.ref] = a.shares;
                ops.push_back({ADD, a.ref, false});
                break;
            }
            case 'E':
            case 'C':
            case 'X': {
                Reduce r = decode_reduce(m);
                auto it = golden.find(r.ref);
                if (it == golden.end()) break;
                if (r.shares >= it->second) {
                    golden.erase(it);
                    ops.push_back({REMOVE, r.ref, false});
                } else {
                    it->second -= r.shares;
                    ops.push_back({TOUCH, r.ref, false});
                }
                break;
            }
            case 'D': {
                uint64_t ref = decode_delete_ref(m);
                if (golden.erase(ref)) ops.push_back({REMOVE, ref, false});
                break;
            }
            case 'U': {  // §3.2: cracked into remove-old + add-new
                Replace u = decode_replace(m);
                if (!golden.erase(u.old_ref)) break;
                golden[u.new_ref] = u.shares;
                ops.push_back({REMOVE, u.old_ref, false});
                ops.push_back({ADD, u.new_ref, true});
                break;
            }
            default:
                break;
        }
    }
    munmap(const_cast<uint8_t*>(data), size);
    close(fd);

    printf("tracked %d of %zu tickers, %zu cache operations\n\n", found, wanted.size(), ops.size());

    // ---- Pass 2: the sweeps ----
    // Baseline is the DESIGN.md cache: 1024 entries, 4-way, 24-bit tag, pLRU.
    const Config base{1024, 4, 24, PLRU};

    printf("baseline (DESIGN.md §2, §4.1):\n");
    print_header();
    print_row(base, simulate(ops, base));

    printf("\nways, at 1024 entries (§2.3):\n");
    print_header();
    for (int w : {1, 2, 4, 8, 16})
        for (Policy p : {LRU, PLRU, RANDOM}) {
            if (w == 1 && p != LRU) continue;  // one way: no choice to make
            Config c{1024, w, 24, p};
            print_row(c, simulate(ops, c));
        }

    printf("\ncapacity, at 4-way pLRU:\n");
    print_header();
    for (int cap : {512, 1024, 2048, 4096, 8192}) {
        Config c{cap, 4, 24, PLRU};
        print_row(c, simulate(ops, c));
    }

    printf("\ntag width, at 1024 x 4-way pLRU (§4.1: justify 24):\n");
    print_header();
    for (int t : {8, 12, 16, 20, 24, 64}) {
        Config c{1024, 4, t, PLRU};
        print_row(c, simulate(ops, c));
    }
    return 0;
}
