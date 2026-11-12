/* The program-key prefix lists (vk_draw.cpp): RX_VK_SKIP_PROG's and the first person head hide's
   HOS_FP_HEAD_HIDE_KEYS — a comma-separated list of hex prefixes, a program key matching the prefixes it starts with. */
#pragma once

#include <cstdint>
#include <utility>
#include <vector>

namespace mkwvk::skip_prefix {

/// One parsed prefix: a key matches it when (key & mask) == value — a full 16-digit key's mask is all ones
using Entry = std::pair<uint64_t, uint64_t>;

/// `list` parsed into prefixes: each comma-separated entry's leading hex digits (either case, at most 16, the rest of
/// the entry ignored) shifted to the key's most significant end. An empty list matches no key, and an entry with no
/// hex digit at all matches none either (a typo names nothing rather than every key that starts with 0)
inline std::vector<Entry> parse(const char* list) {
    std::vector<Entry> v;
    for (const char* q = list ? list : ""; *q;) {
        const char* e = q;
        while (*e && *e != ',') e++;
        uint64_t value = 0;
        int n = 0;
        for (const char* p = q; p < e && n < 16; p++) {
            const char c = *p;
            const int digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
                                                 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (digit < 0) break;
            value = (value << 4) | (uint64_t)digit;
            n++;
        }
        if (n) v.push_back({value << (4 * (16 - n)), n == 16 ? ~0ull : ~(~0ull >> (4 * n))});
        q = *e ? e + 1 : e;
    }
    return v;
}

/// Whether `key` starts with one of `prefixes`' prefixes
inline bool matches(uint64_t key, const std::vector<Entry>& prefixes) {
    for (const auto& [value, mask] : prefixes)
        if ((key & mask) == value) return true;
    return false;
}

}  // namespace mkwvk::skip_prefix
