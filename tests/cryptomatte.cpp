#include "cryptomatte.h"
#include <cstdio>
// Cryptomatte ids (MurmurHash3_x86_32, seed 0; reference values from the mmh3 package) and ranked coverage.
int main() {
    int failures = 0;
    const auto expect = [&](bool ok, const char* what) {
        if (!ok) {
            std::fprintf(stderr, "cryptomatte: %s\n", what);
            ++failures;
        }
    };
    expect(HdUECryptomatteHash("hello") == 0x248bfa47u, "hello");
    expect(HdUECryptomatteHash("Hello, world!") == 0xc0363e43u, "Hello, world! (a 1-byte tail)");
    expect(HdUECryptomatteHash("The quick brown fox jumps over the lazy dog") == 0x2e4ff723u, "fox (a 3-byte tail)");
    expect(HdUECryptomatteHash("default") == 0x42c9679fu, "default (the specification's example manifest)");
    expect(HdUECryptomatteHash("/A") == 0x8edd078fu, "/A (a 2-byte tail)");
    expect(HdUECryptomatteHash("") == 0x00800000u, "the empty name hashes to 0: its exponent bit is flipped");

    expect(HdUECryptomatteLayer("CryptoObject00") == std::pair{0, 0}, "CryptoObject00");
    expect(HdUECryptomatteLayer("CryptoMaterial02") == std::pair{1, 2}, "CryptoMaterial02");
    expect(HdUECryptomatteLayer("CryptoObject") == std::pair{-1, -1}, "a layer needs its two digits");
    expect(HdUECryptomatteLayer("CryptoObjectA0") == std::pair{-1, -1}, "digits only");

    // 4 samples: A, A, B, nothing -> (A, 0.5), (B, 0.25), then zeros.
    const uint32_t a = HdUECryptomatteHash("/A"), b = HdUECryptomatteHash("/B");
    const uint32_t ids[] = {a, b, 0, a};
    float ranks[6];
    HdUECryptomatteRanks(ids, 4, ranks, 3);
    expect(ranks[0] == HdUECryptomatteFloat(a) && ranks[1] == .5f, "rank 0 is the most covering id");
    expect(ranks[2] == HdUECryptomatteFloat(b) && ranks[3] == .25f, "rank 1");
    expect(ranks[4] == 0 && ranks[5] == 0, "unused ranks are zero");
    // Equal coverage: the lower id first, so ranks do not depend on sample order.
    const uint32_t tie[] = {b, a};
    HdUECryptomatteRanks(tie, 2, ranks, 2);
    expect(ranks[0] == HdUECryptomatteFloat(std::min(a, b)), "ties sort by id");
    return failures;
}
