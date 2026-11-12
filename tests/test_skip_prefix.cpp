#include "../gpu/vk/skip_prefix.h"

#include <cstdio>

#define CHECK(name, condition) do { if (!(condition)) { std::fprintf(stderr, "FAIL %s at line %d\n", name, __LINE__); return 1; } } while (0)

int main() {
    using mkwvk::skip_prefix::parse;
    using mkwvk::skip_prefix::matches;
    const uint64_t key = 0x53509044401d78d2ull;   // (a program of OoT's, as vk_draw.cpp names it in RX_VK_SKIP_PROG)

    // (an empty list matches no key: with no env and no calibration, nothing is left out)
    CHECK("an empty list matches nothing", !matches(key, parse(nullptr)) && !matches(key, parse("")));
    CHECK("empty entries are skipped", !matches(key, parse(",,,")));

    // (a full key, and a prefix of it; the list is comma separated, either case)
    CHECK("a full key", matches(key, parse("53509044401d78d2")));
    CHECK("a prefix", matches(key, parse("535090")) && matches(key, parse("5")));
    CHECK("upper case", matches(key, parse("53509044401D78D2")) && matches(key, parse("535090")));
    CHECK("one of a list", matches(key, parse("deadbeef,535090")) && matches(key, parse("5350,deadbeef")));
    CHECK("no match", !matches(key, parse("deadbeef")) && !matches(key, parse("63509044401d78d2")));
    CHECK("a prefix only matches at the key's start", !matches(key, parse("44401d")));

    // (the longest prefix wins nothing special: any entry that matches, matches)
    CHECK("a longer list entry that does not match", !matches(key, parse("535091,53509045")));
    CHECK("a shorter one that does", matches(key, parse("535091,535090")));

    // (an entry with no hex digit at all matches nothing — a typo names no program, not every key that starts with 0)
    CHECK("a non-hex entry is not the zero prefix", !matches(key, parse("xyz")) && !matches(0x0ffu, parse("xyz")));
    CHECK("an entry's non-hex tail is ignored", matches(key, parse("535090zz")));

    // (more than 16 digits: the first 16 are the prefix, as a whole key is 16)
    CHECK("a longer entry is cut at 16 digits", matches(key, parse("53509044401d78d2ff")));
    CHECK("a 16-digit entry of another key", !matches(0x5350ull, parse("5350000000000000")));

    std::puts("skip_prefix: all checks passed");
    return 0;
}
