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

#include "itch/cache_model.hpp"
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
