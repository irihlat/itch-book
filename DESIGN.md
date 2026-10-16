# Order Book Engine — Design and Build Document

A cache-inspired, N-way set-associative order book engine for Nasdaq TotalView-ITCH 5.0,
targeting the Digilent Arty A7-100T.

This document exists to be read before writing code, and re-read while writing it. It
specifies **what** each block does and **why** it is shaped that way. It deliberately does
not contain RTL. The code is the cheap part; the understanding is the deliverable.

---

## 0. Rules for myself

1. Do not commit anything I could not redraw on a whiteboard from memory.
2. After finishing a module, close everything and rewrite it from scratch. If I can't, I
   haven't learnt it yet — that's information, not failure.
3. AI is a tutor and a reviewer, not an author. "Why does this infer a latch?" and "review
   my LRU update" are fine. "Write me a set-associative cache" is not.
4. The golden model, testbench plumbing, build scripts and Vivado error decoding are fair
   game to get help with. The cache, the tag compare, the LRU, the hazard logic and the
   book insert network are not.
5. Every design decision in this document gets a recorded reason. If I can't state the
   reason, I haven't made the decision yet.

---

## 1. Problem domain

### 1.1 What an order book is

An order book is a sorted ladder of unmatched orders for one symbol.

- **Bids** — buy orders, sorted highest price first.
- **Asks** — sell orders, sorted lowest price first.
- **Best bid** — the highest price any waiting buyer will pay.
- **Best ask** — the lowest price any waiting seller will accept.
- **Spread** — the gap between them.

When a buyer's price reaches a seller's price, they match, a trade executes, and both
orders leave the book. Everything that hasn't matched sits and waits. That waiting pile,
grouped by price, is the book.

**Best bid and best ask are the product of this design.** Everything else exists to keep
those two numbers correct.

### 1.2 What the exchange actually sends

The exchange never sends the ladder. It sends a stream of edits, and the receiver builds
the ladder itself.

Critically, the field set is asymmetric:

| Message | order ID | symbol | side | price | shares |
|---|---|---|---|---|---|
| Add Order | yes | yes | yes | yes | yes |
| Order Cancel | yes | no | no | no | yes (amount cancelled) |
| Order Executed | yes | no | no | no | yes (amount executed) |
| Order Delete | yes | no | no | no | no |
| Order Replace | yes (+ new ID) | no | no | yes | yes |

**Price, symbol and side arrive exactly once, on the Add.** Every message afterwards
identifies the order by a 64-bit reference number and nothing else.

> **Amended — see §10.4.** This is correct for the 8-byte ticker *string* but wrong about
> symbol identity: every message carries a 2-byte stock locate code.

This single fact is what drives the entire architecture. To process a cancel, the design
must recover symbol, side and price from the ID alone — which means it must have stored
them when the Add arrived, and must be able to find them again in one or two cycles.

Other properties of the feed:

- It is **full depth** — every resting order from every participant, not a summary.
- It is **all symbols** interleaved in one stream.
- It is **anonymous** — no participant identity (ignore the MPID variant).
- It is **one-way** — receive only. This is a feed handler, not a trading system. No order
  entry, no risk checks, no strategy.
- Price is a 4-byte unsigned integer with **four implied decimal places**. 100.02 arrives
  as 1000200. No floating point anywhere in this design.
- Timestamps are 6 bytes (nanoseconds since midnight). Everything is big-endian.
- Symbols are 8 bytes of ASCII, right-padded with spaces.

---

## 2. Why this is a cache problem

Millions of orders are live on the exchange at any moment. The Arty A7-100T has roughly
600 KB of block RAM. Storing every live order is not possible.

So the design keeps a bounded subset and needs a rule for **which slot a given ID lives
in**. The rule must be arithmetic, not a choice, because the same rule has to run on write
and on read and land in the same place both times. There is no index at the front of the
cabinet — the ID *is* the address.

### 2.1 The structure

Split the 64-bit order ID:

- **index** — low bits, selects one of 256 sets.
- **tag** — the remaining bits, stored in the entry so a hit can be distinguished from a
  collision.

The index bits are never stored. An entry sitting in set 37 must have index bits of 37 —
that's how it got there. The address does half the remembering for free.

Each set holds **4 ways**. All four tags are compared in parallel in one cycle. Total
capacity: 256 × 4 = 1024 live orders.

### 2.2 Why 8 index bits and not 10

1024 slots would need 10 bits to address individually. Only 8 are spent, because the last
four slots are not picked by number — they are picked by comparing all four at once.
Naming a set costs 8 bits. The way is never named, so it is never paid for.

### 2.3 Why 4 ways

Associativity is bounded freedom in placement, and the cost of freedom is comparators.

| Ways | Freedom | Cost |
|---|---|---|
| 1 (direct mapped) | none — one drawer, take it or lose data | trivial |
| 4 | four choices per index | 4 comparators, easy timing |
| 1024 (fully associative) | total — never evict unnecessarily | 1024 comparators, will not close timing |

The reason direct-mapped is bad here despite having the same total capacity: orders live
for wildly different durations. A long-resting order gets evicted the first instant *any*
short-lived order maps to its slot, even with the rest of the cache empty. Four ways means
an order must be unlucky four times.

4 is the sweet spot for the same reason real L1 caches sit at 4 or 8 way.

**To measure, not assume:** actual conflict rate at 4 ways vs 8 vs direct-mapped, on real
captured data. This is a headline number for the writeup.

### 2.4 Where this design differs from a CPU cache — the important part

When an L1 evicts a line, nothing is lost. The data lives in L2 or DRAM. Eviction costs
time, not correctness.

**When this design evicts an order, the order is gone permanently.** There is no L2. The
ITCH message that carried its price went past once and will not be re-sent. That symbol's
ladder is now silently wrong and stays wrong.

So eviction here is a **correctness event, not a performance event**. Consequences for the
design:

- Count evictions in hardware; expose the counter as a status output.
- Raise a per-symbol "book may be stale" flag on eviction. A consumer would rather know its
  picture is untrustworthy than act on a bad one.
- Size the cache so this effectively never happens under the test load, and prove it with
  numbers rather than assertion.

This limitation is stated openly in the writeup. Understanding *what the design does wrong*
is worth more than the RTL.

---

## 3. Architecture

```
        Ingress            unwrap MoldUDP64, sequence gap detection
           |
        Decoder            byte offsets to fixed-width struct, big-endian
           |
        Symbol filter      8 chars to 4-bit index, drop everything else
           |
        Order ID cache     4-way set associative, pseudo-LRU
           |
        Book engine        top-N sorted price levels per symbol
           |
        BBO output         best bid and ask, emitted on change
```

### 3.1 Ingress

Unwraps MoldUDP64: session ID, sequence number, message count, then N length-prefixed ITCH
messages. Detects sequence gaps and flags them.

For simulation, stimulus can come from a file — but build the unwrapper anyway, gap
detection is a real feature and it is cheap.

Interface: AXI4-Stream, 64-bit data, `tvalid` / `tready` / `tlast`.

> **Amended — see §10.3.** The historical capture files are not MoldUDP64 framed.

### 3.2 Decoder

A state machine walking byte offsets, emitting one fixed-width `decoded_msg_t` per message.

Gotchas to handle:
- Big-endian throughout.
- Variable message length — the FSM must derive length from the first byte (message type).
- 6-byte timestamps.
- 8-byte space-padded symbol strings.

**Order Replace is cracked into two internal micro-ops here** — a delete of the old ID and
an add of the new — the same way a CPU decoder cracks a complex instruction. Two cache
operations from one message.

### 3.3 Symbol filter

Only 8–16 symbols are tracked, so well over 99% of the feed is dropped here. Dropping early
is the single biggest area saving in the design.

Implementation: a small CAM. 16 registers holding 64-bit symbols, all compared in parallel,
one-hot match encoded to a 4-bit index. Everything downstream uses the 4-bit index, never
the string.

> **Amended — see §10.4.** Compare 16-bit locate codes, not 64-bit strings, and filter
> every message type rather than Adds only.

### 3.4 Order ID cache

The core of the project. See section 4.

### 3.5 Book engine

Per symbol, maintain the top 8 bid levels and top 8 ask levels, held in flip-flops, always
sorted.

Three cases:

1. **Price matches an existing level** — add or subtract shares on that level. No shift.
   This is the common case by far.
2. **New price inside the held range** — compare against all 8 levels in parallel, work out
   the insert position, shift everything below it one place. The worst level falls off. All
   combinational, one cycle.
3. **Price worse than every held level** — discard. Trading decisions live at the top of the
   book; a bid far below the worst held level is irrelevant.

**Why top-N rather than a price-indexed array:** an array indexed by price tick is O(1) but
needs the price range bounded hard or the memory explodes. Top-N is smaller, and a
compare-and-shift network demonstrates thinking in parallel hardware rather than porting a
`std::map`.

**Known limitation:** if enough levels are cancelled away, the top-8 can empty from below
and would need levels already discarded. Real feed handlers keep more levels than they
publish, or flag the book incomplete. For this project: keep 8, note the limitation.

### 3.6 BBO output

Emits `{symbol_idx, bid_px, bid_qty, ask_px, ask_qty, valid}` whenever the top level
changes.

---

## 4. The order ID cache in detail

### 4.1 Entry layout

| Field | Bits | Purpose |
|---|---|---|
| valid | 1 | identity |
| tag | 24 | identity |
| symbol index | 4 | payload |
| side | 1 | payload |
| price | 32 | payload |
| shares | 32 | payload |

94 bits per entry, 1024 entries, roughly 12 KB.

Notes on each:

- **valid** — BRAM contains garbage at power-up. Without this, random junk with a matching
  tag looks like a real order. Cleared on reset. Omitting it is a classic first-project bug.
- **tag** — 24 rather than the full 40 leftover bits, because real exchange order IDs do not
  span the full 64-bit range. Fewer tag bits means a narrower compare and easier timing. It
  also permits theoretical aliasing, so this number is **measured against real data, not
  guessed**.
- **symbol index** — 4 bits, not the 8-byte string. The string was already collapsed at the
  filter stage; storing it would cost 64 bits per entry and buy nothing.
- **price** — 32-bit fixed point, four implied decimals.
- **shares** — the only field modified after the Add.

Storage split: data array in BRAM, tags and valid bits in distributed RAM or flops so all
four ways can be compared in a single cycle.

### 4.2 Operations

**Add** — pick a way (an invalid one first, otherwise the LRU victim), write the entry.

**Cancel / Execute** — read the set, compare all 4 tags in parallel, hit gives symbol, side
and price. **This is a read-modify-write:** read the current share count, subtract, then
either write the new count back or clear valid if it reached zero. The share count reaching
zero frees the slot, which is why the cache does not simply fill up and stay full.

**Delete** — read, hit, clear valid.

**Miss** — drop the message, increment the miss counter.

Every hit also updates the ladder: the entry says the order was (say) a buy at 100.02, so
that bid level loses the same number of shares. Two writes from one message.

### 4.3 Eviction: pseudo-LRU

Three bits per set forming a binary tree. Root bit selects the stale pair; the selected
pair's bit selects the way within it. Victim found in two bit tests.

Update on a touch: flip the bits along that way's path to point **away** from it. Two flips
per access. 3 bits × 256 sets = 768 flip-flops for the whole policy.

Compared with the alternatives:

| | True LRU | Pseudo-LRU | Random |
|---|---|---|---|
| Knows | full ranking, 1st to 4th | which half, then which of pair | nothing |
| Bits per set | 5 | 3 | 0 (one shared LFSR) |
| Update | reorder all four, every access | flip two bits | none |
| Victim | always the true oldest | a stale one, usually the oldest | anything |

Pseudo-LRU never picks the newest entry; it sometimes picks the second- or third-oldest
instead of the actual oldest. The miss rate difference is typically a fraction of a
percent, and true LRU's four-way shuffle is nasty combinational logic sitting on the
critical path — real clock frequency traded for a rounding error.

**Honest caveat specific to this problem:** LRU's assumption is weaker here than in a CPU
cache. In a CPU an untouched line is probably dead. In an order book an order can rest
untouched all morning and then be cancelled — never dead, just patient. LRU is a decent
heuristic here, not a correct one. **Worth measuring against random on real data rather than
assuming it wins.**

Build random first (one LFSR, almost no logic), then pseudo-LRU, and compare eviction
counts. "I implemented both and measured X% difference for Y% less logic" is a concrete,
checkable claim.

### 4.4 Pipeline hazards — the hard problem

Read, modify and write land in different pipeline stages. Meanwhile the next message is
already entering the pipe. If two messages concern the same order:

```
              cycle 1     cycle 2     cycle 3     cycle 4
Cancel 88     read        modify      write
              sees 500    500-200     stores 300
Execute 88                read        modify      write
                          sees 500    wrong       wrong
```

In cycle 2 the execute reads memory and gets 500, which is now a lie — the correct value is
300, which does not land until cycle 3. The book goes wrong and stays wrong.

This is a **read-after-write hazard**, and it is the same problem a load-store unit has with
a store buffer.

Two fixes:

- **Stall.** Detect that the incoming message targets an order already in flight, freeze it
  until the write lands. Simple, obviously correct, costs throughput.
- **Forward.** Take the value from the stage that computed it and hand it sideways to the
  read stage. A comparator on the in-flight order ID driving a mux. Not much logic, but it
  must be exactly right or the wrong order's shares get forwarded.

**Plan: build stall first.** Get it correct, get the scoreboard passing, then add forwarding
as a second version and measure the throughput improvement. "I built it the simple way,
measured it, then optimised it" is a better story than pretending it was right first time —
and it produces two numbers instead of zero.

### 4.5 Backpressure

Decide early, because retrofitting is painful: either skid buffers between stages, or a
fully non-stalling fixed-latency pipeline sized so it cannot stall.

**Two decisions to nail down before writing RTL:**
1. Target end-to-end fixed latency.
2. Whether the pipeline is permitted to stall at all.

Everything else is constrained by these two.

> **Answered — see §10.5.** Stalling is permitted; skid buffers between stages.

### 4.6 Timing closure

Expected critical path: the 4-way tag compare, plus the priority encode, plus the LRU
update, all in one cycle. Likely needs splitting into two pipeline stages — which then
makes the hazard problem worse.

That tension — latency vs frequency vs hazard complexity — is the interesting engineering,
and it is what gets written up.

---

## 5. Build order

Do not attempt the whole cache as the next piece of RTL. Each step below is a few dozen
lines plus a testbench, and each is made of the previous ones.

> **Amended — see §10.1.** Phase 0 (C++ model and parameter sweeps) comes before step 1.

### Step 1 — One entry, stored

A small memory with valid bits. Write port, read port, synchronous reset clearing all valid
bits.

*Testbench checks:* write then read returns the same data; reset clears valid; reading an
address never written returns valid = 0.

*Self-test:* why must the valid bits reset but the data need not?

### Step 2 — Tag compare

Given a stored tag and an incoming tag, produce a hit signal. Hit requires valid AND tag
match.

*Testbench checks:* matching tag with valid = 1 gives hit; matching tag with valid = 0 gives
no hit; non-matching tag gives no hit.

*Self-test:* what goes wrong if valid is left out of the hit condition?

### Step 3 — Four ways at once

Four tags compared in parallel, one-hot match, encoded to a way index. Plus a hit/miss
signal.

*Testbench checks:* hit in each way individually; miss when none match; hit and miss are
never simultaneously high; the one-hot vector never has two bits set.

*Self-test:* what should happen if two ways somehow hold the same tag? Is that reachable?

### Step 4 — Add eviction

Pseudo-LRU victim selection and the update-on-touch logic. Invalid ways are chosen before
any valid way.

*Testbench checks:* fill a set, verify the victim is a plausible LRU choice; verify invalid
ways are filled first; verify the touched way is never the immediate next victim.

*Self-test:* construct by hand a touch sequence where pseudo-LRU and true LRU disagree.

### Step 5 — Pipeline it

Split into read / modify / write stages, add hazard detection, stall on conflict.

*Testbench checks:* back-to-back messages for the same order produce the correct final
share count; back-to-back messages for different orders do not stall unnecessarily.

*Self-test:* what is the minimum cycle separation at which no stall is needed, and why?

**Only after step 5:** decoder, symbol filter, book engine, then integration. The board goes
in the order once the design works in simulation — not before.

---

## 6. Verification

```
                Message stream
                (recorded or random)
                   /          \
              Your RTL      C++ golden model
                   \          /
                   Scoreboard
              any mismatch fails
```

The golden model does the obvious thing — a hash map for orders, a sorted map for levels,
no cleverness. Same stimulus into both. Every BBO the RTL emits is checked against the
model.

**The trap:** the C++ model has unlimited memory and never evicts. The RTL does. The moment
a set fills, they disagree, and an evening disappears hunting a bug that isn't one. Either
model the cache geometry in C++ as well, or size the test so evictions cannot occur and
introduce them deliberately later. **Model it** — more work, but then the scoreboard
genuinely validates the cache rather than working around it.

Three layers, in this order:

**Directed tests.** Hand-written sequences, writable before the design is finished. Add then
cancel. Four orders into one set, then a fifth. Cancel more shares than the order holds.
Replace. Delete an ID that was never added. These catch the real bugs.

**Assertions (SVA).** Running on every cycle of every test, so ten good properties catch more
than a hundred directed tests:
- `tvalid` never drops before `tready`
- hit and miss are mutually exclusive
- the way-match vector is one-hot or zero
- no X propagates into the book
- best bid price is always strictly below best ask price
- the level array is always sorted
- shares never go negative

**Constrained random.** Legal ITCH streams with random IDs, prices and message types, run in
the millions. This is what finds the hazard bugs nobody thought of — two messages about the
same order landing exactly three cycles apart.

---

## 7. Numbers to produce

The writeup is half the project. Most candidates write none at all. Target these:

- Conflict/eviction rate at 4 ways vs 8 vs direct-mapped, on real captured data
- Eviction rate as a function of total capacity
- Miss rate under pseudo-LRU vs random
- Logic utilisation of pseudo-LRU vs true LRU
- Achieved clock frequency, and what the critical path turned out to be
- Throughput with stalling vs with forwarding
- End-to-end latency in cycles
- Tag width vs aliasing rate, to justify 24 bits

Four of these eight need no hardware and come out of the phase 0 C++ model: ways vs
eviction rate, capacity vs evictions, pLRU vs random, tag width vs aliasing. Two come from
RTL simulation (stall vs forwarding, end-to-end latency) and two from Vivado reports (Fmax
and critical path, pLRU vs true LRU area).

---

## 8. Open questions

- Target clock frequency? What does the ingress data rate actually demand?
- ~~Fixed-latency non-stalling pipeline, or stall-and-skid-buffer?~~ Answered, §10.5.
- ~~Where do real ITCH sample files come from for the golden data?~~ Answered, §10.2.
- Does the top-8 level depth ever empty out on real data? Measure before deciding it's fine.
- ~~Is 24 tag bits actually enough for the ID distribution in the sample data?~~ Answered,
  §10.8.3.
- MoldUDP framing: synthesise it, or cut the ingress block from scope? (§10.3)
- Cache capacity: 1024, 4096 or 8192? Benefit measured on BX in §10.8.5; decide once Nasdaq
  eviction rates and Vivado cost figures are in.

---

## 9. What this project is not

Not a trading system. No order entry, no risk checks, no strategy, no P&L. It receives,
it builds a picture, it publishes best bid and ask. That is the whole scope, and keeping
the scope closed is what makes it finishable.

---

## 10. Amendments (2026-10-14)

### 10.1 Build order — phase 0 added before RTL

Every question in §8 is a data question, and four of the eight numbers in §7 need no
hardware. Build the C++ model and run the parameter sweeps first, then write RTL against
justified constants rather than guesses.

Revised phase order:

| Phase | Work | Gate |
|---|---|---|
| 0 | Data acquisition, C++ golden model, parameter sweep | 4 ways / 24-bit tag / top-8 justified by numbers |
| 1 | Interface contract: stall policy, latency target, AXIS handshake | decided before any RTL |
| 2 | Cache core, unpipelined (§5 steps 1–4) | Fmax on the compare + encode path alone |
| 3 | Pipeline and hazards (§5 step 5): stall version, then forwarding version | scoreboard passing |
| 4 | Decoder, symbol filter, book engine, BBO | integration in simulation |
| 5 | Scoreboard, SVA, constrained random, §7 numbers | writeup data complete |
| 6 | Board bring-up | buy the Arty here, not before |

First concrete task in phase 0: a **message census** — walk the capture file, count messages
by type, print the table. The ratio of Adds to Cancels/Deletes/Executes is the cache
pressure, and no capacity reasoning is possible before seeing it.

### 10.2 Data source

`emi.nasdaq.com`, over HTTPS — the FTP endpoint referenced by most online guides is dead.

- `/ITCH/Nasdaq BX ITCH/` — BX files, roughly 0.4–1.6 GB compressed. Use these for
  developing and debugging the parser; same message format, far faster to iterate on.
- `/ITCH/Nasdaq ITCH/` — the full feed, 3.5–17 GB compressed. Use for headline numbers only.
- `/ITCH/Stock_Locate_Codes/` — relevant to §10.4.

**Avoid quiet sessions for reported figures.** File size is a proxy for message volume, and
the smallest files are quiet days (30 December, post-Thanksgiving half sessions). Measuring
eviction rate on one of those flatters the result, and an interviewer who knows the data
will ask which day. Report a quiet day and a busy day; the gap between them is itself a
result worth having.

### 10.3 File framing is not MoldUDP64

Historical daily capture files are a flat sequence of `[2-byte big-endian length][message]`.
No session ID, no sequence numbers, no packet framing. The first bytes of a file are
`00 0c 53` — length 12, message type `S`.

This means the §3.1 ingress block has no stimulus as specified.

**OPEN:** either synthesise MoldUDP packets in the C++ model to exercise the unwrapper, or
cut MoldUDP from scope and take the length-prefixed stream directly. Decide before phase 4.

### 10.4 Stock locate — amends §1.2

Every ITCH message carries a 2-byte stock locate code at bytes 1–2, not just Add Order.
§1.2 is correct that the 8-byte ticker *string* arrives once; it is wrong that symbol
identity does.

Add Order (`A`) layout, 36 bytes:

| Offset | Bytes | Field |
|---|---|---|
| 0 | 1 | message type `A` |
| 1 | 2 | stock locate |
| 3 | 2 | tracking number |
| 5 | 6 | timestamp |
| 11 | 8 | order reference number |
| 19 | 1 | buy/sell indicator |
| 20 | 4 | shares |
| 24 | 8 | stock symbol, space-padded |
| 32 | 4 | price |

Consequences:

- The symbol filter can drop non-tracked symbols for **all** message types before the cache,
  not just Adds. As currently specified, cancels and deletes for ~99% of symbols reach the
  cache and burn lookup bandwidth for nothing.
- The filter CAM compares 16 bits, not 64. Sixteen 16-bit comparators instead of sixteen
  64-bit ones.
- Requires consuming Stock Directory (`R`) messages at startup to build the
  locate → index table. This is a startup phase in hardware, not optional plumbing.

The cache premise is unaffected: price and side still arrive only on the Add, so recovering
them from the order ID is still the core problem.

**TO VERIFY** against the spec PDF before relying on any of it:
`nasdaqtrader.com/content/technicalsupport/specifications/dataproducts/NQTVITCHSpecification.pdf`
(Real data agrees with these offsets — see §10.8.1. The PDF check is still to do.)

### 10.5 §4.5 answered — the pipeline may stall

ITCH peak message rates against a 100 MHz clock leave tens of cycles per message.
Throughput is not the constraint.

Decision: **allow stalling, with skid buffers between stages.** Target fixed, predictable
end-to-end latency rather than maximum throughput.

The forwarding version (§4.4) still gets built, but to produce a measurement, not because
the design needs it. Say that plainly in the writeup rather than implying a throughput
bound that was never there.

### 10.6 Board demo is a replay

The Arty A7-100T has no 10G interface. On-board demonstration will be a BRAM-fed replay of
captured messages, not a line-rate feed. State this openly rather than discovering it in
phase 6.

### 10.7 Tooling boundary

`rtl/` is denied to Claude Code via `Edit(**/rtl/**)` in `.claude/settings.json`, enforcing
rule 3 of §0 structurally rather than by intention. Testbenches live in `bench/`, the C++
model in `src/`, `include/` and `test/`, all of which are fair game for assistance.

### 10.8 Phase 0 results — BX 2019-07-30 (2026-10-14)

All figures from `data/20190730.BX_ITCH_50` (837 MB, 28.7M messages, 03:06–19:05).
Reproduce with `runsim census`, `runsim orders`, `runsim`. The cache model is covered by 20
directed tests (`runtest`), each confirmed to catch a deliberately planted bug.

#### 10.8.1 The data

- **Framing (§10.3) confirmed.** Every byte walked; all 28.7M message lengths match the
  spec's per-type sizes.
- **Field offsets (§10.4) agree with the data.** Following every order through the day:
  0 references to unknown orders, 0 bad side bytes, 0 zero prices, 0 orders left at close.
  The spec PDF check is still outstanding, but a wrong offset could not produce this.
- **How orders leave the book:** delete 80.2%, replace 16.1%, full execution 3.7%.
  Order Cancel (`X`) never removed a whole order — full removals always come as `D`.
- **Order lifetime:** p10 1.1 ms, median 1.1 s, p90 61 s, p99 815 s. Most orders are gone
  within a second; a small tail rests for many minutes (the §4.3 "patient order").
- **Peak live orders, all symbols:** 67,283 at 15:59:34. Nothing like this reaches the
  cache — the symbol filter decides what does.

#### 10.8.2 Decision: tracked symbols

**The 16 busiest symbols by add count** on the development day (list in
`config/tracked_symbols.txt`): SPY IWM IWO SOXL AAPL QQQ XLK MSFT IWN UVXY XLV IWB GDX OILX
DRIP SMH.

Reason:
- Fills the 4-bit symbol index; more than 16 widens the index and adds a book per symbol
  for no new learning (§9).
- Combined peak **1,010 live orders at 15:59:01 vs 1,024 slots** — enough pressure that the
  §7 sweeps show real differences instead of zero everywhere.
- A reproducible rule, not a hand-picked list.

Stored as tickers, never locate codes: locates are reassigned daily and resolved from `R`
messages at startup (§10.4).

#### 10.8.3 Decision: tag width — 24 bits (§4.1, §8)

| Tag bits | 8 | 12 | 16 | 20 | 24 |
|---|---|---|---|---|---|
| Aliases (wrong order returned) | 2,596 | 11 | 0 | 0 | 0 |

16 is the measured minimum on BX. **Keep 24**: 8 bits of margin for the full Nasdaq feed,
where far more orders per day widen the spread of IDs.

#### 10.8.4 Decision: 4-way, pseudo-LRU (§2.3, §4.3)

Evictions as % of adds, 1024 entries:

| Ways | 1 | 2 | 4 | 8 | 16 |
|---|---|---|---|---|---|
| pLRU | 12.0% | 5.0% | **2.0%** | 0.78% | 0.30% |

At 4 ways: true LRU 1.92%, **pLRU 1.99%**, random 2.49% (and random has 50% more misses).

Reason:
- pLRU is within 4% of true LRU for a fraction of the logic — §4.3's claim, now measured.
- The §4.3 caveat ("LRU is weaker for patient orders") is answered: on this data LRU's
  guess still clearly beats random.
- Each doubling of ways cuts evictions ~2.5×, but every way is another comparator on the
  critical path (§4.6). More capacity buys the same improvement more cheaply (§10.8.5).

#### 10.8.5 OPEN: capacity

4-way pLRU, 24-bit tag:

| Entries | 512 | 1024 | 2048 | 4096 | 8192 |
|---|---|---|---|---|---|
| Evictions | 63,591 | 25,092 | 7,404 | 1,518 | 250 |
| % of adds | 5.1% | 2.0% | 0.59% | 0.12% | 0.02% |

**1024 as designed is not enough**: 25,092 lost orders a day, each one a stale book (§2.4).
Never zero at any size, so the eviction counter and stale flag stay essential.

Cost estimate (to confirm in Vivado, phase 5): order data is BRAM and cheap at any size; the
pressure is tag + valid storage for the parallel compare — ~25 kbit at 1024 (flip-flops),
~100 kbit at 4096 (distributed RAM), ~200 kbit at 8192 (large distributed RAM, or BRAM at
the cost of an extra cycle and a longer hazard window). Valid-bit reset also grows.

**Deliberately left open.** The benefit side is measured here; the cost side is only an
estimate. Decide once both are real: eviction rates on a full Nasdaq day, and actual
utilisation and Fmax from Vivado for the candidate sizes.

#### 10.8.6 Observation: set index is not uniform

Low 8 bits of order IDs across all 10.6M adds: busiest set gets ~17% more than the mean, and
only 42% of IDs are odd. Low-bit indexing is slightly lopsided. Worth one experiment: does
XOR-ing upper ID bits into the index reduce evictions?

#### 10.8.7 Remaining in phase 0

- Top-8 depth: does a book's top 8 ever empty from below (§3.5, §8)? Needs price levels in
  the C++ model.
- Re-run all of the above on a full Nasdaq day, busy and quiet (§10.2); then close §10.8.5.
- Decoder tests (`decode_basic`).
