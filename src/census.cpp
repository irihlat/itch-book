// Message census (DESIGN.md §10.1): count messages by type in an ITCH capture.
// File layout (§10.3): [2-byte big-endian length][message], repeated.
// The first byte of each message is its type letter.
// Usage: ./build/census data/20190730.BX_ITCH_50

#include "itch/endian.hpp"  // "" = our project; <> = system/library

#include <fcntl.h>     // open
#include <sys/mman.h>  // mmap, munmap, madvise
#include <sys/stat.h>  // fstat
#include <unistd.h>    // close

#include <cstdint>
#include <cstdio>      // printf, fprintf, perror

using namespace std;

namespace {  // unnamed: names here are private to this file

struct MsgInfo {
    uint16_t length;   // spec length, excluding the 2-byte prefix
    const char* name;
};

// Spec lengths are a self-check only: the loop trusts the file's prefix. If the
// parser lost its place, garbage "types" would almost never match these.
MsgInfo info_for(uint8_t type) {
    switch (type) {
        case 'S': return {12, "System Event"};
        case 'R': return {39, "Stock Directory"};
        case 'H': return {25, "Stock Trading Action"};
        case 'Y': return {20, "Reg SHO Restriction"};
        case 'L': return {26, "Market Participant Position"};
        case 'V': return {35, "MWCB Decline Level"};
        case 'W': return {12, "MWCB Status"};
        case 'K': return {28, "IPO Quoting Period"};
        case 'J': return {35, "LULD Auction Collar"};
        case 'h': return {21, "Operational Halt"};
        case 'A': return {36, "Add Order"};
        case 'F': return {40, "Add Order with MPID"};
        case 'E': return {31, "Order Executed"};
        case 'C': return {36, "Order Executed with Price"};
        case 'X': return {23, "Order Cancel"};
        case 'D': return {19, "Order Delete"};
        case 'U': return {35, "Order Replace"};
        case 'P': return {44, "Trade (non-cross)"};
        case 'Q': return {40, "Cross Trade"};
        case 'B': return {19, "Broken Trade"};
        case 'I': return {50, "NOII"};
        case 'N': return {20, "RPII"};
        default:  return {0, "UNKNOWN"};
    }
}

// ns since midnight -> HH:MM:SS.mmm. %02llu = unsigned long long, 2 digits, zero-padded.
void print_time(uint64_t ns) {
    uint64_t s = ns / 1'000'000'000;  // ' is a digit separator
    printf("%02llu:%02llu:%02llu.%03llu",
           (unsigned long long)(s / 3600), (unsigned long long)(s / 60 % 60),
           (unsigned long long)(s % 60), (unsigned long long)(ns / 1'000'000 % 1000));
}

}  // namespace

// argc = number of command-line words, argv = the words. Return 0 = success.
int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <capture-file>\n", argv[0]);  // stderr = error output
        return 2;
    }

    int fd = open(argv[1], O_RDONLY);  // fd: OS handle for the open file, -1 on failure
    if (fd < 0) { perror(argv[1]); return 1; }  // perror adds the OS's reason
    struct stat st;
    if (fstat(fd, &st) < 0) { perror("fstat"); return 1; }  // &st = address of st
    size_t size = size_t(st.st_size);

    // mmap: the whole file becomes one array, data[0..size-1]; the OS loads it
    // from disk as we touch it. No chunks, so no message split across a boundary.
    auto* data = static_cast<const uint8_t*>(
        mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0));
    if (data == MAP_FAILED) { perror("mmap"); return 1; }
    madvise(const_cast<uint8_t*>(data), size, MADV_SEQUENTIAL);  // hint: read ahead

    // Indexed by type byte: count['A'] counts Add Orders. = {} zeroes them.
    uint64_t count[256] = {};
    uint64_t bad_length[256] = {};
    uint64_t total = 0;
    uint64_t first_ts = 0, last_ts = 0;

    size_t pos = 0;  // offset of the current length prefix
    while (pos + 2 <= size) {
        uint16_t len = itch::load_be16(data + pos);
        if (pos + 2 + len > size) {  // message runs past end of file
            fprintf(stderr, "truncated message at offset %zu\n", pos);
            break;
        }
        const uint8_t* msg = data + pos + 2;
        uint8_t type = msg[0];

        ++count[type];
        if (len != info_for(type).length) ++bad_length[type];

        // Common 11-byte header: type(1) locate(2) tracking(2) timestamp(6) -> ts at +5.
        if (len >= 11) {
            last_ts = itch::load_be48(msg + 5);
            if (total == 0) first_ts = last_ts;
        }
        ++total;
        pos += 2 + len;  // skip prefix + message
    }

    // %-6s = text left-aligned in 6 cols, %14llu = number right-aligned in 14,
    // %7.3f = decimal with 3 places, %% = literal %, %zu = size_t.
    printf("%-6s %-30s %14s %8s\n", "type", "name", "count", "%");
    for (int t = 0; t < 256; ++t) {
        if (count[t] == 0) continue;
        printf("%-6c %-30s %14llu %7.3f%%", t, info_for(uint8_t(t)).name,
               (unsigned long long)count[t], 100.0 * double(count[t]) / double(total));
        if (bad_length[t])
            printf("  (%llu with unexpected length)", (unsigned long long)bad_length[t]);
        printf("\n");
    }
    printf("%-37s %14llu\n", "total", (unsigned long long)total);
    printf("bytes walked: %zu of %zu\n", pos, size);  // should equal: ended exactly at EOF
    printf("time span: ");
    print_time(first_ts);
    printf(" to ");
    print_time(last_ts);
    printf("\n");

    munmap(const_cast<uint8_t*>(data), size);
    close(fd);
    return 0;
}
