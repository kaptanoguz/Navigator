// fold_name_into (navigator/fs.hpp, Phase 5): equals fold_name, reuses buffers, and maps every
// result byte back to the input code point that produced it.
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <navigator/fs.hpp>

using navigator::fs::fold_name;
using navigator::fs::fold_name_into;

namespace {

using Offsets = std::vector<std::uint32_t>;

// Length of the UTF-8 sequence led by `b` in a result (results of valid input are valid UTF-8;
// escaped input bytes are copied as single bytes).
std::size_t seq_len(unsigned char b) {
    if (b >= 0xF0 && b <= 0xF4) {
        return 4;
    }
    if (b >= 0xE0 && b <= 0xEF) {
        return 3;
    }
    if (b >= 0xC2 && b <= 0xDF) {
        return 2;
    }
    return 1;
}

// Generic invariants of the offset map.
void check_offsets(std::string_view name, const std::string& out, const Offsets& off) {
    REQUIRE(off.size() == out.size() + 1);
    CHECK(off.back() == name.size());
    for (std::size_t i = 0; i + 1 < off.size(); ++i) {
        CHECK(off[i] <= off[i + 1]);
        CHECK(off[i] < name.size());
    }
}

} // namespace

TEST_CASE("fold_name_into equals fold_name", "[names][fold][fold_into]") {
    const std::vector<std::string> samples = {
        "",
        "README.md",
        "\xC4\xB0stanbul",              // İstanbul
        "\xC4\xB1\xC4\xB0Ii",           // ıİIi
        "\xC3\x87\x41\xC4\x9E\xC3\x96", // ÇAĞÖ
        "\xCE\xA3\xCE\x9F\xCE\xA6",     // ΣΟΦ
        "e\xCC\x81",                    // NFD é
        "\xF0\x90\x90\x80",             // Deseret capital long I
        "bad\xFF\xFE\x65nd",            // invalid bytes
        "\xC3",                         // truncated sequence
        "\xED\xA0\x80",                 // encoded surrogate (invalid)
        "\xE1\xBA\x9E",                 // U+1E9E capital sharp s -> U+00DF
    };
    std::string out = "garbage that must be replaced";
    Offsets off = {9, 9, 9};
    for (const std::string& s : samples) {
        fold_name_into(s, out, &off);
        CHECK(out == fold_name(s));
        check_offsets(s, out, off);
        std::string plain;
        fold_name_into(s, plain);
        CHECK(plain == out);
    }
}

TEST_CASE("fold_name_into: offsets for ASCII are the identity", "[names][fold][fold_into]") {
    std::string out;
    Offsets off;
    fold_name_into("AbC.TXT", out, &off);
    CHECK(out == "abc.txt");
    CHECK(off == Offsets{0, 1, 2, 3, 4, 5, 6, 7});
}

TEST_CASE("fold_name_into: Turkish dotted/dotless I shrink to one byte",
          "[names][fold][fold_into][turkish]") {
    std::string out;
    Offsets off;
    // "İı" + "x": U+0130 (2 bytes) -> 'i', U+0131 (2 bytes) -> 'i', 'x'.
    const std::string_view name = "\xC4\xB0\xC4\xB1x";
    fold_name_into(name, out, &off);
    CHECK(out == "iix");
    CHECK(off == Offsets{0, 2, 4, 5});

    // "dİr": the match "i" in the result (byte 1) maps back to raw bytes [1, 3).
    fold_name_into("d\xC4\xB0r", out, &off);
    REQUIRE(out == "dir");
    CHECK(off == Offsets{0, 1, 3, 4});
    CHECK(off[2] - off[1] == 2);
}

TEST_CASE("fold_name_into: multi-byte code points keep one offset per byte",
          "[names][fold][fold_into]") {
    std::string out;
    Offsets off;
    // "ÖĞ": U+00D6 -> U+00F6 (2 -> 2), U+011E -> U+011F (2 -> 2).
    fold_name_into("\xC3\x96\xC4\x9E", out, &off);
    CHECK(out == "\xC3\xB6\xC4\x9F");
    CHECK(off == Offsets{0, 0, 2, 2, 4});

    // U+1E9E (3 bytes) folds to U+00DF (2 bytes).
    fold_name_into("A\xE1\xBA\x9E", out, &off);
    CHECK(out == "a\xC3\x9F");
    CHECK(off == Offsets{0, 1, 1, 4});

    // U+2126 OHM SIGN (3 bytes) folds to U+03C9 (2 bytes); 4-byte Deseret stays 4 bytes.
    fold_name_into("\xE2\x84\xA6\xF0\x90\x90\x80", out, &off);
    CHECK(out == "\xCF\x89\xF0\x90\x90\xA8");
    CHECK(off == Offsets{0, 0, 3, 3, 3, 3, 7});
}

TEST_CASE("fold_name_into: invalid bytes map one to one", "[names][fold][fold_into][utf8]") {
    std::string out;
    Offsets off;
    const std::string_view name = "A\xFF\xC3Z";
    fold_name_into(name, out, &off);
    CHECK(out == "a\xFF\xC3z");
    CHECK(off == Offsets{0, 1, 2, 3, 4});
}

TEST_CASE("fold_name_into: code point count is preserved and ranges map back",
          "[names][fold][fold_into]") {
    const std::string name = "Fo\xC4\xB0L\xC4\xB1\x64\xC3\x9C\xFFr"; // FoİLıdÜ<FF>r
    std::string out;
    Offsets off;
    fold_name_into(name, out, &off);
    check_offsets(name, out, off);
    // Walk result code points; each maps to exactly one input code point, in order.
    std::size_t result_cps = 0;
    std::size_t prev_src = 0;
    bool first = true;
    for (std::size_t i = 0; i < out.size();) {
        const std::size_t len = seq_len(static_cast<unsigned char>(out[i]));
        for (std::size_t k = 1; k < len; ++k) {
            CHECK(off[i + k] == off[i]); // all bytes of one result code point share a source
        }
        if (!first) {
            CHECK(off[i] > prev_src);
        }
        first = false;
        prev_src = off[i];
        ++result_cps;
        i += len;
    }
    CHECK(result_cps == 9); // F o İ L ı d Ü <FF> r
}

TEST_CASE("fold_name_into: reuses capacity", "[names][fold][fold_into]") {
    std::string out;
    out.reserve(256);
    Offsets off;
    off.reserve(257);
    const char* data_before = out.data();
    const std::uint32_t* off_before = off.data();
    fold_name_into("Hello W\xC3\x96RLD", out, &off);
    CHECK(out == "hello w\xC3\xB6rld");
    CHECK(out.data() == data_before);
    CHECK(off.data() == off_before);
}
