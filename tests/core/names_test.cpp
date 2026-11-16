// fold_name / compare_names / to_display_name / extension (navigator/fs.hpp, "Names").
#include <compare>
#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include <navigator/fs.hpp>

using navigator::fs::compare_names;
using navigator::fs::fold_name;
using navigator::fs::SortOptions;
using navigator::fs::to_display_name;

namespace {

SortOptions opts(bool natural, bool case_insensitive) {
    SortOptions o;
    o.natural = natural;
    o.case_insensitive = case_insensitive;
    return o;
}

bool less(std::string_view a, std::string_view b, const SortOptions& o = SortOptions{}) {
    return compare_names(a, b, o) < 0 && compare_names(b, a, o) > 0;
}

bool equiv(std::string_view a, std::string_view b, const SortOptions& o = SortOptions{}) {
    return compare_names(a, b, o) == 0 && compare_names(b, a, o) == 0;
}

} // namespace

TEST_CASE("fold_name: ASCII, Latin, Greek, Cyrillic, Armenian, Deseret", "[names][fold]") {
    CHECK(fold_name("README.TXT") == "readme.txt");
    CHECK(fold_name("ÀÉÎÕÜÇÑ") == "àéîõüçñ");
    CHECK(fold_name("ĞÜŞÖÇ") == "ğüşöç");
    CHECK(fold_name("ΑΒΓΔΣ") == "αβγδσ");
    CHECK(fold_name("ς") == "σ"); // final sigma, status C
    CHECK(fold_name("Привет") == "привет");
    CHECK(fold_name("ԱԲԳ") == "աբգ");
    CHECK(fold_name("ＡＢＣ") == "ａｂｃ");                     // fullwidth
    CHECK(fold_name("\xF0\x90\x90\x80") == "\xF0\x90\x90\xA8"); // U+10400 -> U+10428 (Deseret)
    CHECK(fold_name("\xE2\x84\xAA") == "k");                    // KELVIN SIGN -> k
    CHECK(fold_name("ẞ") == "ß"); // U+1E9E: status F maps to "ss", status S -> U+00DF
    CHECK(fold_name("ß") == "ß"); // no simple fold
    CHECK(fold_name("ﬃ") == "ﬃ"); // ligature: full folding only, untouched
    CHECK(fold_name("日本語") == "日本語");
    CHECK(fold_name("").empty());
}

TEST_CASE("fold_name: Turkish I variants all fold to i", "[names][fold][turkish]") {
    CHECK(fold_name("İ") == "i");
    CHECK(fold_name("ı") == "i");
    CHECK(fold_name("I") == "i");
    CHECK(fold_name("i") == "i");
    CHECK(fold_name("İSTANBUL") == "istanbul");
    CHECK(fold_name("ığdır") == "iğdir");
    CHECK(fold_name("ĞÜŞIÖÇİ") == "ğüşiöçi");
}

TEST_CASE("fold_name: idempotent, no normalization", "[names][fold]") {
    const std::string nfd = "Cafe\xCC\x81"; // E + COMBINING ACUTE
    CHECK(fold_name(nfd) == "cafe\xCC\x81");
    for (const std::string_view s : {"MiXeD Ünïcödé ΣΑΣ", "İıIi", "Ⰰⰱ", "ᏸᏹ", "ⴀႠ"}) {
        CHECK(fold_name(fold_name(s)) == fold_name(s));
    }
}

TEST_CASE("fold_name: invalid bytes are passed through unchanged", "[names][fold][utf8]") {
    CHECK(fold_name("A\xFF"
                    "B") == "a\xFF"
                            "b");
    CHECK(fold_name("\xC0\xAF") == "\xC0\xAF");                 // overlong '/'
    CHECK(fold_name("\xED\xA0\x80Z") == "\xED\xA0\x80z");       // encoded surrogate
    CHECK(fold_name("\xE2\x82") == "\xE2\x82");                 // truncated
    CHECK(fold_name("\xF4\x90\x80\x80") == "\xF4\x90\x80\x80"); // > U+10FFFF
}

TEST_CASE("to_display_name: invalid bytes become U+FFFD", "[names][utf8]") {
    const std::string fffd = "\xEF\xBF\xBD";
    CHECK(to_display_name("plain.txt") == "plain.txt");
    CHECK(to_display_name("ğüşıöçİ") == "ğüşıöçİ");
    CHECK(to_display_name("a\xFF"
                          "b") == "a" + fffd + "b");
    CHECK(to_display_name("\xC3") == fffd);
    CHECK(to_display_name("\xE2\x82\x41") == fffd + fffd + "A"); // one per invalid byte
    CHECK(to_display_name("\xED\xA0\x80") == fffd + fffd + fffd);
    CHECK(to_display_name("\xF0\x9F\x98\x80") == "\xF0\x9F\x98\x80"); // emoji kept
    CHECK(to_display_name("").empty());
}

TEST_CASE("extension()", "[names]") {
    using navigator::fs::extension;
    CHECK(extension("a.tar.gz") == "gz");
    CHECK(extension(".bashrc").empty());
    CHECK(extension("x.").empty());
    CHECK(extension("noext").empty());
    CHECK(extension("Photo.JPG") == "JPG");
}

TEST_CASE("compare_names: natural numbers", "[names][compare][natural]") {
    CHECK(less("file9", "file10"));
    CHECK(less("file2", "file10"));
    CHECK(less("a1b2", "a1b10"));
    CHECK(less("1", "2"));
    CHECK(less("x99", "x100"));
    CHECK(equiv("a01", "a1"));
    CHECK(equiv("a001b", "a1B"));
    CHECK(equiv("0", "000"));
    CHECK(less("a01", "a2"));
    // Leading zeros do not count toward magnitude.
    CHECK(less("v0009", "v10"));
    // Very long digit runs (beyond any integer type).
    const std::string big1 = "n" + std::string(200, '9');
    const std::string big2 = "n1" + std::string(200, '0');
    CHECK(less(big1, big2));
    CHECK(less("n" + std::string(100, '0') + "5", "n6"));
    CHECK(equiv("n" + std::string(100, '0') + "5", "n5"));
    CHECK(less("n" + std::string(60, '1') + "2", "n" + std::string(60, '1') + "3"));
    // Not natural: plain code point order.
    CHECK(less("file10", "file9", opts(false, true)));
    CHECK(less("a01", "a1", opts(false, true)));
}

TEST_CASE("compare_names: digit vs non-digit uses code point order", "[names][compare]") {
    CHECK(less("file.txt", "file1.txt"));
    CHECK(less("file1.txt", "fileA.txt"));
    CHECK(less("file.txt", "fileA.txt"));
    CHECK(less("a", "a1"));   // prefix first
    CHECK(less("a-1", "a1")); // '-' < '1'
    CHECK(less("a9", "a:"));  // ':' after digits
    CHECK(less("a99999", "a:"));
    // Non-ASCII digits are ordinary code points.
    CHECK(less("x\xD9\xA1"
               "0",
               "x\xD9\xA2")); // ARABIC-INDIC 1,0 vs 2 compared as code points
    CHECK(less("x2", "x\xD9\xA1"));
}

TEST_CASE("compare_names: case insensitive, Turkish I equivalence", "[names][compare]") {
    CHECK(equiv("README", "readme"));
    CHECK(equiv("İstanbul", "istanbul"));
    CHECK(equiv("Istanbul", "ıstanbul"));
    CHECK(equiv("İstanbul", "ıstanbul"));
    CHECK(equiv("ĞÜŞÖÇ", "ğüşöç"));
    CHECK(less("apple", "Banana"));
    CHECK(less("Apple", "banana"));
    CHECK(less("ilk", "İzmir")); // i vs i, l < z
    // Case-sensitive: raw code points ('B' < 'a').
    const SortOptions cs = opts(true, false);
    CHECK(less("Banana", "apple", cs));
    CHECK(less("README", "readme", cs));
    CHECK(less("Istanbul", "istanbul", cs));
    CHECK_FALSE(equiv("İ", "i", cs));
}

TEST_CASE("compare_names: code point order, escapes, NFC vs NFD", "[names][compare][utf8]") {
    CHECK(less("", "a"));
    CHECK(equiv("", ""));
    CHECK(less("z", "é"));                // U+007A < U+00E9
    CHECK(less("e\xCC\x81", "\xC3\xA9")); // NFD 'e'+U+0301 < NFC U+00E9 (e < é)
    CHECK_FALSE(equiv("e\xCC\x81", "\xC3\xA9"));
    // Escaped byte 0xFF -> U+DCFF: after U+D7FF, before U+E000 (unlike raw byte order).
    CHECK(less("\xED\x9F\xBF", "\xFF"));
    CHECK(less("\xFF", "\xEE\x80\x80"));
    CHECK(less("\xFF", "\xF0\x9F\x98\x80"));
    CHECK(less("\x80", "\xFF"));
    // Escaped bytes are never folded.
    CHECK(equiv("A\xFF", "a\xFF"));
}

TEST_CASE("compare_names: weak ordering is consistent", "[names][compare]") {
    const std::string_view names[] = {"a",     "A",    "a1",    "a01",     "a2", "a10",
                                      "b",     "İ",    "ı",     "i2",      "",   "\xFF",
                                      "file.", "file", "file1", "file01a", "_x", "Ω"};
    const SortOptions o;
    for (const std::string_view a : names) {
        CHECK(compare_names(a, a, o) == 0);
        for (const std::string_view b : names) {
            const auto ab = compare_names(a, b, o);
            const auto ba = compare_names(b, a, o);
            CHECK((ab < 0) == (ba > 0));
            CHECK((ab == 0) == (ba == 0));
            for (const std::string_view c : names) {
                if (ab < 0 && compare_names(b, c, o) < 0) {
                    CHECK(compare_names(a, c, o) < 0);
                }
                if (ab == 0 && compare_names(b, c, o) == 0) {
                    CHECK(compare_names(a, c, o) == 0);
                }
            }
        }
    }
}
