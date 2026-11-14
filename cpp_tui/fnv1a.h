// fnv1a.h
//
// Self-contained 64-bit FNV-1a hasher for change fingerprints, cache keys and
// dedup keys.
//
//     Fnv1a64 h;
//     h.fold(someWord);
//     h.foldBytes(someString);
//     std::uint64_t digest = h.value();
//
// `foldBytes` folds the string's bytes and then a separator byte, so callers
// folding several strings cannot collide across a shared prefix/suffix pair
// ("ab" then "c" vs "a" then "bc"). Not cryptographic - a fast, stable hash
// for local fingerprints and keys.
#pragma once

#include <cstdint>
#include <string_view>

namespace llmfun::tui {

/// 64-bit FNV-1a. Folding a word is xor then multiply by the prime, so each
/// folded word mixes into the whole state.
struct Fnv1a64 {
    static constexpr std::uint64_t kOffsetBasis = 1469598103934665603ull;
    static constexpr std::uint64_t kPrime = 1099511628211ull;

    void fold(std::uint64_t v) {
        h ^= v;
        h *= kPrime;
    }

    // The string's bytes, then a separator: without it, folding "ab" then "c"
    // and folding "a" then "bc" would land on the same value.
    void foldBytes(std::string_view s) {
        for (char c : s)
            fold(static_cast<unsigned char>(c));
        fold(0xff);
    }

    std::uint64_t value() const { return h; }

private:
    std::uint64_t h = kOffsetBasis;
};

} // namespace llmfun::tui
