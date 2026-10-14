// Order tracker: follows every order through the day with unlimited memory.
// The seed of the golden model (DESIGN.md §6). Reports:
//   - peak live orders           (vs the cache's 1024 slots)
//   - how orders end             (delete / execute / cancel / replace)
//   - order lifetimes            (relevant to LRU, §4.3)
//   - busiest symbols            (for choosing what to track, §3.3)
//   - unknown references         (should be 0 if the §10.4 offsets are right)
//   - combined peak of a tracked set, if tickers are given (what the cache must hold)
// Usage: ./build/orders data/20190730.BX_ITCH_50 [TICKER ...]

#include "itch/endian.hpp"
#include "itch/messages.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>      // sort, nth_element
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>  // hash table: key -> value, ~constant-time lookup
#include <vector>         // growable array

using namespace std;
using namespace itch;

namespace {

// What we remember about a live order.
struct Order {
    uint16_t locate;
    char side;
    uint32_t price;
    uint32_t shares;
    uint64_t added_ts;    // when it was added, for lifetime
};

// How an order left the book.
enum End { BY_DELETE, BY_EXECUTE, BY_CANCEL, BY_REPLACE, END_KINDS };
const char* end_name[END_KINDS] = {"delete", "fully executed", "fully cancelled", "replaced"};

// ns -> human-readable duration.
string fmt_duration(uint64_t ns) {
    char buf[32];
    if (ns < 1'000)              snprintf(buf, sizeof buf, "%llu ns", (unsigned long long)ns);
    else if (ns < 1'000'000)     snprintf(buf, sizeof buf, "%.1f us", ns / 1e3);
    else if (ns < 1'000'000'000) snprintf(buf, sizeof buf, "%.1f ms", ns / 1e6);
    else                         snprintf(buf, sizeof buf, "%.1f s", ns / 1e9);
    return buf;  // char array converts to string
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <capture-file> [TICKER ...]\n", argv[0]);
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

    unordered_map<uint64_t, Order> live;  // ref -> order: every order currently on the book
    live.reserve(2'000'000);              // pre-size to avoid repeated regrowth

    vector<string> ticker(65536);         // locate -> symbol; locate is 16 bits, so 65536 slots
    vector<uint64_t> adds_per_locate(65536, 0);
    vector<uint32_t> live_per_locate(65536, 0);  // orders on the book right now, per symbol
    vector<uint32_t> peak_per_locate(65536, 0);  // most it ever held at once
    vector<uint64_t> peak_ts_per_locate(65536, 0);
    // Tracked set: tickers from the command line (argv[2] onward).
    vector<string> wanted(argv + 2, argv + argc);
    vector<bool> tracked(65536, false);   // locate -> is it in the tracked set?
    vector<uint16_t> tracked_locates;     // the locates we found for those tickers
    size_t tracked_live = 0, tracked_peak = 0;
    uint64_t tracked_peak_ts = 0;
    vector<uint32_t> at_peak;             // per-symbol live counts at the combined peak

    vector<uint64_t> lifetimes;           // ns, one per order that left the book
    lifetimes.reserve(11'000'000);

    uint64_t ended[END_KINDS] = {};
    uint64_t unknown_ref = 0;             // reference to an order we never saw added
    uint64_t bad_side = 0, zero_price = 0;
    size_t peak_live = 0;
    uint64_t peak_ts = 0;

    // Remove an order, recording why and how long it lived.
    auto remove = [&](unordered_map<uint64_t, Order>::iterator it, End why, uint64_t now) {
        // [&] = this lambda (inline function) can use the variables around it.
        ++ended[why];
        lifetimes.push_back(now - it->second.added_ts);  // it->second = the Order
        --live_per_locate[it->second.locate];
        if (tracked[it->second.locate]) --tracked_live;
        live.erase(it);
    };

    auto add = [&](uint64_t ref, Order o) {
        live[ref] = o;
        ++adds_per_locate[o.locate];
        if (++live_per_locate[o.locate] > peak_per_locate[o.locate]) {
            peak_per_locate[o.locate] = live_per_locate[o.locate];
            peak_ts_per_locate[o.locate] = o.added_ts;
        }
        if (o.side != 'B' && o.side != 'S') ++bad_side;
        if (o.price == 0) ++zero_price;
        if (live.size() > peak_live) { peak_live = live.size(); peak_ts = o.added_ts; }
        if (tracked[o.locate] && ++tracked_live > tracked_peak) {
            tracked_peak = tracked_live;
            tracked_peak_ts = o.added_ts;
            at_peak.clear();              // snapshot who made up this peak
            for (uint16_t l : tracked_locates) at_peak.push_back(live_per_locate[l]);
        }
    };

    size_t pos = 0;
    while (pos + 2 <= size) {
        uint16_t len = load_be16(data + pos);
        if (pos + 2 + len > size) break;
        const uint8_t* m = data + pos + 2;
        Header h = decode_header(m);

        switch (h.type) {
            case 'R': {  // braces give each case its own scope for local variables
                StockDirectory r = decode_stock_directory(m);
                string sym = r.symbol;
                sym.erase(sym.find_last_not_of(' ') + 1);  // trim the space padding
                ticker[r.locate] = sym;
                if (find(wanted.begin(), wanted.end(), sym) != wanted.end()) {
                    tracked[r.locate] = true;
                    tracked_locates.push_back(r.locate);
                }
                break;
            }
            case 'A':
            case 'F': {  // F decodes the same; it just has extra bytes on the end
                AddOrder a = decode_add(m);
                add(a.ref, {a.locate, a.side, a.price, a.shares, h.timestamp});
                break;
            }
            case 'E':
            case 'C':
            case 'X': {
                Reduce r = decode_reduce(m);
                auto it = live.find(r.ref);  // find returns live.end() if missing
                if (it == live.end()) { ++unknown_ref; break; }
                Order& o = it->second;       // & = reference: o IS the stored order, not a copy
                if (r.shares >= o.shares)
                    remove(it, h.type == 'X' ? BY_CANCEL : BY_EXECUTE, h.timestamp);
                else
                    o.shares -= r.shares;
                break;
            }
            case 'D': {
                auto it = live.find(decode_delete_ref(m));
                if (it == live.end()) { ++unknown_ref; break; }
                remove(it, BY_DELETE, h.timestamp);
                break;
            }
            case 'U': {
                Replace u = decode_replace(m);
                auto it = live.find(u.old_ref);
                if (it == live.end()) { ++unknown_ref; break; }
                Order next = it->second;    // copy: keeps locate and side
                next.price = u.price;
                next.shares = u.shares;
                next.added_ts = h.timestamp;
                remove(it, BY_REPLACE, h.timestamp);
                add(u.new_ref, next);
                break;
            }
            default:
                break;  // everything else doesn't touch the book
        }
        pos += 2 + len;
    }

    // ---- Report ----
    printf("peak live orders:   %zu (at ", peak_live);
    uint64_t s = peak_ts / 1'000'000'000;
    printf("%02llu:%02llu:%02llu)\n", (unsigned long long)(s / 3600),
           (unsigned long long)(s / 60 % 60), (unsigned long long)(s % 60));
    printf("live at end of day: %zu\n", live.size());
    printf("unknown references: %llu   bad side: %llu   zero price: %llu\n",
           (unsigned long long)unknown_ref, (unsigned long long)bad_side,
           (unsigned long long)zero_price);

    uint64_t total_ended = 0;
    for (uint64_t n : ended) total_ended += n;  // range-for: each element in turn
    printf("\nhow orders ended (%llu):\n", (unsigned long long)total_ended);
    for (int k = 0; k < END_KINDS; ++k)
        printf("  %-16s %10llu  %5.1f%%\n", end_name[k], (unsigned long long)ended[k],
               100.0 * double(ended[k]) / double(total_ended));

    // Percentiles: sort once, then pick positions. p50 = median.
    sort(lifetimes.begin(), lifetimes.end());
    printf("\norder lifetime:\n");
    for (double p : {0.10, 0.25, 0.50, 0.75, 0.90, 0.99}) {
        size_t i = size_t(p * double(lifetimes.size() - 1));
        printf("  p%-3.0f %s\n", p * 100, fmt_duration(lifetimes[i]).c_str());  // c_str: string -> C string for printf
    }

    // Busiest symbols by Add count: sort locate codes, biggest first.
    // Peak live = most orders that symbol held at once: what it costs the cache.
    vector<uint16_t> order(65536);
    for (int i = 0; i < 65536; ++i) order[i] = uint16_t(i);
    sort(order.begin(), order.end(),
         [&](uint16_t a, uint16_t b) { return adds_per_locate[a] > adds_per_locate[b]; });
    printf("\nbusiest symbols:\n");
    printf("  %-8s %6s %9s %10s %9s\n", "symbol", "locate", "adds", "peak live", "peak at");
    for (int i = 0; i < 30; ++i) {
        uint16_t l = order[i];
        uint64_t ps = peak_ts_per_locate[l] / 1'000'000'000;
        printf("  %-8s %6u %9llu %10u  %02llu:%02llu:%02llu\n", ticker[l].c_str(), l,
               (unsigned long long)adds_per_locate[l], peak_per_locate[l],
               (unsigned long long)(ps / 3600), (unsigned long long)(ps / 60 % 60),
               (unsigned long long)(ps % 60));
    }

    if (!wanted.empty()) {
        printf("\ntracked set: %zu of %zu tickers found\n", tracked_locates.size(), wanted.size());
        for (const string& w : wanted)  // const& = look at it without copying
            if (find(ticker.begin(), ticker.end(), w) == ticker.end())
                printf("  not in this file: %s\n", w.c_str());

        uint64_t sum_peaks = 0;
        for (uint16_t l : tracked_locates) sum_peaks += peak_per_locate[l];
        uint64_t ps = tracked_peak_ts / 1'000'000'000;
        printf("combined peak:        %zu  (at %02llu:%02llu:%02llu)\n", tracked_peak,
               (unsigned long long)(ps / 3600), (unsigned long long)(ps / 60 % 60),
               (unsigned long long)(ps % 60));
        printf("sum of separate peaks: %llu  (upper bound: peaks at different times)\n",
               (unsigned long long)sum_peaks);
        printf("cache capacity:       1024  -> combined peak is %.0f%% of it\n",
               100.0 * double(tracked_peak) / 1024.0);

        printf("\nat the combined peak:\n");
        printf("  %-8s %6s %12s\n", "symbol", "live", "own peak");
        for (size_t i = 0; i < tracked_locates.size(); ++i) {
            uint16_t l = tracked_locates[i];
            printf("  %-8s %6u %12u\n", ticker[l].c_str(), at_peak.empty() ? 0 : at_peak[i],
                   peak_per_locate[l]);
        }
    }

    munmap(const_cast<uint8_t*>(data), size);
    close(fd);
    return 0;
}
