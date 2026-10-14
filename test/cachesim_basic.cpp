// Directed tests for the cache model (DESIGN.md §6: "these catch the real bugs").
// Every expected value is worked out by hand. Most tests use a tiny cache:
//   capacity 8, 4 ways -> 2 sets. Set = lowest bit of the ID:
//   even IDs (2, 4, 6, ...) go to set 0, odd IDs to set 1.
// Run: ./runtest -test cachesim_basic

#include "check.hpp"
#include "itch/cache_model.hpp"

#include <cstdint>
#include <vector>

using namespace std;
using namespace itch;

namespace {

const Config small_lru{8, 4, 64, LRU};
const Config small_plru{8, 4, 64, PLRU};

Op add(uint64_t ref) { return {ADD, ref, false}; }
Op touch(uint64_t ref) { return {TOUCH, ref, false}; }
Op remove(uint64_t ref) { return {REMOVE, ref, false}; }

// Run ops on a fresh cache; hand back both the cache (to inspect) and the counts.
struct Run {
    Cache cache;
    Result r;
};
Run run(const Config& c, const vector<Op>& ops) {
    Run x{Cache(c), {}};
    x.cache.run(ops, x.r);
    return x;
}

// ---- Basic operations (§4.2) ----

void add_then_hit() {
    auto x = run(small_lru, {add(2), touch(2)});
    CHECK(x.r.misses == 0);
    CHECK(x.cache.holds(2));
}

void remove_then_miss() {
    auto x = run(small_lru, {add(2), remove(2), touch(2)});
    CHECK(x.r.misses == 1);
    CHECK(!x.cache.holds(2));
}

void partial_reduce_keeps_order() {
    auto x = run(small_lru, {add(2), touch(2), touch(2)});
    CHECK(x.r.misses == 0);
    CHECK(x.cache.holds(2));
}

void miss_on_never_added() {
    auto x = run(small_lru, {remove(42)});
    CHECK(x.r.misses == 1);
}

// ---- Placement ----

void four_fit_in_one_set() {
    auto x = run(small_lru, {add(2), add(4), add(6), add(8)});  // all set 0
    CHECK(x.r.evictions == 0);
    for (uint64_t ref : {2, 4, 6, 8}) CHECK(x.cache.holds(ref));
}

void fifth_in_set_evicts() {
    auto x = run(small_lru, {add(2), add(4), add(6), add(8), add(10)});
    CHECK(x.r.evictions == 1);
    CHECK(x.cache.holds(10));
}

void sets_are_independent() {
    // Set 0 full; an odd ID goes to set 1, which is empty: no eviction.
    auto x = run(small_lru, {add(2), add(4), add(6), add(8), add(1)});
    CHECK(x.r.evictions == 0);
    CHECK(x.cache.holds(1));
}

void invalid_way_used_before_evicting() {
    // Free a slot by removing 4; the next add must reuse it, not evict.
    auto x = run(small_lru, {add(2), add(4), add(6), add(8), remove(4), add(10)});
    CHECK(x.r.evictions == 0);
    for (uint64_t ref : {2, 6, 8, 10}) CHECK(x.cache.holds(ref));
}

void direct_mapped_conflict() {
    // 4 entries, 1 way -> 4 sets of one. 0 and 4 share set 0: second evicts first.
    auto x = run({4, 1, 64, LRU}, {add(0), add(4)});
    CHECK(x.r.evictions == 1);
    CHECK(!x.cache.holds(0));
    CHECK(x.cache.holds(4));
}

// ---- Replacement policy (§4.3) ----

void lru_evicts_least_recent() {
    // Touch 2 after filling: 4 becomes the oldest, so 4 is evicted.
    auto x = run(small_lru, {add(2), add(4), add(6), add(8), touch(2), add(10)});
    CHECK(x.r.evictions == 1);
    CHECK(!x.cache.holds(4));
    for (uint64_t ref : {2, 6, 8, 10}) CHECK(x.cache.holds(ref));
}

void plru_fill_in_order_evicts_first() {
    // Filling ways 0,1,2,3 in order leaves every tree bit pointing at way 0.
    auto x = run(small_plru, {add(2), add(4), add(6), add(8), add(10)});
    CHECK(x.r.evictions == 1);
    CHECK(!x.cache.holds(2));
}

void plru_touched_is_never_next_victim() {
    // §5 step 4 check. For each order in the full set: touch it, add a new one,
    // and the touched order must survive.
    for (uint64_t t : {2, 4, 6, 8}) {
        auto x = run(small_plru, {add(2), add(4), add(6), add(8), touch(t), add(10)});
        CHECK(x.r.evictions == 1);
        CHECK(x.cache.holds(t));
    }
}

void plru_equals_lru_at_two_ways() {
    // With 2 ways, one pLRU bit IS exact LRU, so results must match exactly.
    vector<Op> ops;
    uint32_t s = 12345;
    for (int i = 0; i < 20000; ++i) {  // pseudo-random mix of adds/touches/removes
        s = s * 1103515245u + 12345u;
        uint64_t ref = (s >> 8) % 64;
        int k = int((s >> 20) % 3);
        ops.push_back({OpKind(k), ref, false});
    }
    Result a = simulate(ops, {16, 2, 64, LRU});
    Result b = simulate(ops, {16, 2, 64, PLRU});
    CHECK(a.evictions == b.evictions);
    CHECK(a.misses == b.misses);
    CHECK(a.evictions > 0);  // make sure the test actually exercised eviction
}

void random_is_repeatable() {
    vector<Op> ops;
    for (uint64_t r = 0; r < 200; r += 2) ops.push_back(add(r));  // all set 0, lots of evictions
    Result a = simulate(ops, {8, 4, 64, RANDOM});
    Result b = simulate(ops, {8, 4, 64, RANDOM});
    CHECK(a.evictions == 96);  // 100 adds into a 4-way set: 4 fit, 96 evict
    CHECK(a.evictions == b.evictions);
}

void random_spreads_victims() {
    // Fill set 0 with 2,4,6,8, then 20 more adds. A real random choice hits
    // ways 1-3 sometimes, so 4, 6 and 8 can't ALL survive. (A broken "always
    // way 0" would keep them forever.) Fixed seed, so the outcome is fixed.
    vector<Op> ops = {add(2), add(4), add(6), add(8)};
    for (uint64_t r = 10; r < 50; r += 2) ops.push_back(add(r));
    auto x = run({8, 4, 64, RANDOM}, ops);
    CHECK(!(x.cache.holds(4) && x.cache.holds(6) && x.cache.holds(8)));
}

// ---- Tags and aliasing (§4.1) ----

void index_and_tag_split() {
    Cache c({8, 4, 64, LRU});     // 2 sets -> 1 index bit
    CHECK(c.set_of(6) == 0);      // 6 = 0b110: low bit 0
    CHECK(c.set_of(7) == 1);
    CHECK(c.tag_of(6) == 3);      // 0b110 >> 1 = 0b11
}

void narrow_tag_aliases() {
    // 1-bit tag. 2 = 0b010 -> tag 1. 6 = 0b110 -> tag 0b11, cut to 1 bit = 1.
    // Same set, same stored tag: looking up 6 "finds" order 2. Wrong order.
    auto x = run({8, 4, 1, LRU}, {add(2), touch(6)});
    CHECK(x.r.aliases == 1);
    CHECK(x.r.misses == 0);
}

void full_tag_never_aliases() {
    auto x = run({8, 4, 64, LRU}, {add(2), touch(6)});
    CHECK(x.r.aliases == 0);
    CHECK(x.r.misses == 1);
}

// ---- Replace (§3.2: cracked into remove + add) ----

void replace_of_evicted_order_is_lost() {
    // 4 entries, 4 ways: a single set. Adding 5 evicts 1 (LRU). Then a replace of 1:
    // the remove misses, so the new order 9 can't be added (side unknown).
    auto x = run({4, 4, 64, LRU},
                 {add(1), add(2), add(3), add(4), add(5), remove(1), {ADD, 9, true}});
    CHECK(x.r.evictions == 1);
    CHECK(x.r.lost_replaces == 1);
    CHECK(!x.cache.holds(9));
}

void replace_of_live_order_works() {
    auto x = run(small_lru, {add(2), remove(2), {ADD, 4, true}});
    CHECK(x.r.lost_replaces == 0);
    CHECK(x.cache.holds(4));
}

}  // namespace

int main() {
    const check::Test tests[] = {
        {"add_then_hit", "an added order can be found again", add_then_hit},
        {"remove_then_miss", "a removed order is gone", remove_then_miss},
        {"partial_reduce_keeps_order", "a partial cancel/execute keeps the order", partial_reduce_keeps_order},
        {"miss_on_never_added", "looking up an unknown order is a miss", miss_on_never_added},
        {"four_fit_in_one_set", "4 orders fit in one 4-way set, no eviction", four_fit_in_one_set},
        {"fifth_in_set_evicts", "a 5th order in a full set evicts one", fifth_in_set_evicts},
        {"sets_are_independent", "a full set doesn't affect other sets", sets_are_independent},
        {"invalid_way_used_before_evicting", "an empty slot is reused before evicting", invalid_way_used_before_evicting},
        {"direct_mapped_conflict", "1-way: two orders in the same set clash", direct_mapped_conflict},
        {"lru_evicts_least_recent", "LRU evicts the order untouched longest", lru_evicts_least_recent},
        {"plru_fill_in_order_evicts_first", "pLRU, filled in order, evicts the first", plru_fill_in_order_evicts_first},
        {"plru_touched_is_never_next_victim", "pLRU never evicts the order just touched", plru_touched_is_never_next_victim},
        {"plru_equals_lru_at_two_ways", "at 2 ways, pLRU and LRU behave identically", plru_equals_lru_at_two_ways},
        {"random_is_repeatable", "random policy gives the same result every run", random_is_repeatable},
        {"random_spreads_victims", "random policy doesn't always evict the same slot", random_spreads_victims},
        {"index_and_tag_split", "ID splits correctly into set index and tag", index_and_tag_split},
        {"narrow_tag_aliases", "too-short tag returns the WRONG order", narrow_tag_aliases},
        {"full_tag_never_aliases", "full tag never confuses two orders", full_tag_never_aliases},
        {"replace_of_evicted_order_is_lost", "replacing an evicted order loses the new one", replace_of_evicted_order_is_lost},
        {"replace_of_live_order_works", "replacing a live order adds the new one", replace_of_live_order_works},
    };
    return check::run_tests(tests);
}
