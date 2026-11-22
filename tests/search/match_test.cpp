// Scoring gold cases, highlights, query compilation, narrows() and ranking order
// (navigator/search.hpp "Matching model").
#include <algorithm>
#include <cstdint>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <navigator/search.hpp>

using namespace navigator;
using namespace navigator::search;

namespace {

std::optional<std::int32_t> score_of(std::string_view name, std::string_view q, MatchMode mode) {
    Matcher m(compile_query(q, mode));
    return m.score(name);
}

std::vector<HighlightRange> hl(std::string_view name, std::string_view q, MatchMode mode) {
    const std::optional<Match> m = match(name, compile_query(q, mode));
    REQUIRE(m.has_value());
    return m.value_or(Match{}).highlights;
}

constexpr auto kSub = MatchMode::Substring;
constexpr auto kFuzzy = MatchMode::Fuzzy;

} // namespace

TEST_CASE("compile_query trims, splits, folds and caps", "[search][query]") {
    const Query q = compile_query("  Foo   BAR ", kSub);
    CHECK(q.text == "  Foo   BAR ");
    CHECK(q.terms == std::vector<std::string>{"foo", "bar"});
    CHECK(compile_query("   ", kFuzzy).empty());
    CHECK(compile_query("", kFuzzy).empty());

    // Turkish: I, i, İ, ı all fold to i.
    CHECK(compile_query("İSTANBUL", kSub).terms == std::vector<std::string>{"istanbul"});
    CHECK(compile_query("ıstanbul", kSub).terms == std::vector<std::string>{"istanbul"});

    // More than kMaxQueryTerms: the rest joins the last term with single spaces.
    const Query many = compile_query("a b c d e f g h i   j", kSub);
    REQUIRE(many.terms.size() == kMaxQueryTerms);
    CHECK(many.terms.back() == "h i j");

    // Cut at kMaxQueryBytes on a code point boundary ("é" is two bytes).
    std::string longq(kMaxQueryBytes - 1, 'a');
    longq += "é";
    const Query cut = compile_query(longq, kSub);
    CHECK(cut.text.size() == kMaxQueryBytes - 1);
    CHECK(cut.text == std::string(kMaxQueryBytes - 1, 'a'));
}

TEST_CASE("empty query matches everything with score 0", "[search][match]") {
    Matcher m;
    const std::optional<Match> r = m.match("anything.txt");
    REQUIRE(r);
    CHECK(r.value_or(Match{-1, {}}) == Match{0, {}});
    CHECK(m.matches(""));
}

TEST_CASE("substring scoring: prefix > word start > middle", "[search][score]") {
    // 3 chars: 3*16 + 2*24 consecutive = 96, plus position bonus and leading penalty.
    CHECK(score_of("report.txt", "rep", kSub) == 96 + kBonusPrefix);           // 128
    CHECK(score_of("my report.txt", "rep", kSub) == 96 + kBonusWordStart - 3); // 113
    CHECK(score_of("unreported.txt", "rep", kSub) == 96 - 2);                  // 94
    const auto points = [](std::string_view name) {
        return score_of(name, "rep", kSub).value_or(0);
    };
    CHECK(points("report.txt") > points("my report.txt"));
    CHECK(points("my report.txt") > points("unreported.txt"));
    CHECK_FALSE(score_of("rpeort", "rep", kSub).has_value());
    // Case-insensitive.
    CHECK(score_of("REPORT.TXT", "rep", kSub) == 128);
}

TEST_CASE("word starts: separators, camelCase and letter-digit", "[search][score]") {
    CHECK(score_of("a_b", "b", kSub) == 16 + 20 - 2);
    CHECK(score_of("a-b", "b", kSub) == 16 + 20 - 2);
    CHECK(score_of("a(b", "b", kSub) == 16 + 20 - 2);
    CHECK(score_of("ab", "b", kSub) == 16 - 1);
    CHECK(score_of("fooBar", "bar", kSub) == 48 + 20 + 48 - 3);
    CHECK(score_of("FOOBAR", "bar", kSub) == 48 + 48 - 3);   // no lower->upper boundary
    CHECK(score_of("report2024", "2", kSub) == 16 + 20 - 6); // first '2' after a letter
    // Non-ASCII letters never form a camelCase boundary.
    CHECK(score_of("éB", "b", kSub) == 16 - 1);
}

TEST_CASE("leading penalty is capped", "[search][score]") {
    const std::string far = std::string(30, 'x') + "a";
    CHECK(score_of(far, "a", kSub) == 16 - kPenaltyLeadingMax);
}

TEST_CASE("whole-name and stem bonuses", "[search][score]") {
    // readme: 6*16 + 32 + 5*24 = 248.
    CHECK(score_of("README", "readme", kSub) == 248 + kBonusExactName);
    CHECK(score_of("README.md", "readme", kSub) == 248 + kBonusExactStem);
    CHECK(score_of("README.md.bak", "readme", kSub) == 248); // stem is "README.md"
    CHECK(score_of("README.md.bak", "readme.md", kSub) ==
          (9 * 16) + 32 + (8 * 24) + 20 /* 'm' after '.' */ + kBonusExactStem);
    CHECK(score_of(".bashrc", ".bashrc", kSub) == (7 * 16) + 32 + 20 + (6 * 24) + 100);
    // Fuzzy mode gets the same bonuses.
    CHECK(score_of("README", "readme", kFuzzy) == 248 + kBonusExactName);
}

TEST_CASE("fuzzy scoring: gaps, consecutive bonus, best alignment", "[search][score][fuzzy]") {
    // r0 e1 p2 o3 r4 t5: "rpt" -> 0, 2, 5.
    CHECK(score_of("report", "rpt", kFuzzy) == 48 + 32 - 3 - 6);
    CHECK_FALSE(score_of("report", "rpt", kSub).has_value());
    // Consecutive alignment beats a gapped one.
    CHECK(score_of("report", "rep", kFuzzy) == 128);
    // "ab" in "a_b_ab": (0,2) = 48 + 36 - 3 = 81 beats (4,5) = 36 + 16 + 24 - 4 = 72.
    CHECK(score_of("a_b_ab", "ab", kFuzzy) == 81);
    CHECK(hl("a_b_ab", "ab", kFuzzy) == std::vector<HighlightRange>{{0, 1}, {2, 3}});
    // camelCase in fuzzy: f0 + B3 = 48 + 36 - 6.
    CHECK(score_of("fooBar", "fb", kFuzzy) == 48 + 36 - 6);
    CHECK_FALSE(score_of("bf", "fb", kFuzzy).has_value()); // order matters
    // Every substring match is a fuzzy match with a score at least as high.
    for (const char* name : {"my report.txt", "unreported", "A_B_C", "xxabcxxabc"}) {
        for (const char* q : {"rep", "b_c", "abc", "x"}) {
            const auto s = score_of(name, q, kSub);
            if (s) {
                const auto f = score_of(name, q, kFuzzy);
                REQUIRE(f);
                CHECK(f.value_or(-100000) >= *s);
            }
        }
    }
}

TEST_CASE("ties pick the earliest alignment", "[search][highlight]") {
    // Both 'a's score 16 + 20 - 15 (cap): the first one is highlighted.
    const std::string name = std::string(20, '_') + "a___a";
    CHECK(score_of(name, "a", kSub) == 21);
    CHECK(hl(name, "a", kSub) == std::vector<HighlightRange>{{20, 21}});
    CHECK(hl(name, "a", kFuzzy) == std::vector<HighlightRange>{{20, 21}});
}

TEST_CASE("multi-term AND with overlapping highlights", "[search][match]") {
    // rep (128) + txt at 7 (48 + 20 + 48 - 7 = 109).
    CHECK(score_of("report.txt", "rep txt", kSub) == 237);
    CHECK_FALSE(score_of("report.md", "rep txt", kSub).has_value());
    CHECK(hl("report", "rep port", kSub) == std::vector<HighlightRange>{{0, 6}});
    CHECK(hl("report.txt", "txt rep", kSub) == std::vector<HighlightRange>{{0, 3}, {7, 10}});
    // Adjacent ranges merge.
    CHECK(hl("abcd", "ab cd", kSub) == std::vector<HighlightRange>{{0, 4}});
}

TEST_CASE("Turkish dotted and dotless i match each other", "[search][unicode]") {
    for (const char* name : {"İstanbul.txt", "ISTANBUL.txt", "ıstanbul.txt", "istanbul.txt"}) {
        INFO(name);
        CHECK(score_of(name, "istanbul", kSub) == score_of("istanbul.txt", "istanbul", kSub));
        CHECK(score_of(name, "İSTANBUL", kFuzzy).has_value());
        CHECK(score_of(name, "ıst", kSub).has_value());
    }
    // Stem bonus applies through the fold.
    CHECK(score_of("İstanbul.txt", "istanbul", kSub) == (8 * 16) + 32 + (7 * 24) + kBonusExactStem);
    // Highlights are raw byte ranges: İ is two bytes.
    CHECK(hl("İstanbul.txt", "ist", kSub) == std::vector<HighlightRange>{{0, 4}});
    CHECK(hl("xİy", "i", kSub) == std::vector<HighlightRange>{{1, 3}});
    CHECK(hl("Kayıt Defteri", "kayit", kSub) == std::vector<HighlightRange>{{0, 6}});
}

TEST_CASE("highlights never split multibyte sequences or escaped bytes", "[search][unicode]") {
    // "çğü€" : 2 + 2 + 2 + 3 bytes.
    CHECK(hl("açğü€b", "ğ€", kFuzzy) == std::vector<HighlightRange>{{3, 5}, {7, 10}});
    CHECK(hl("açğü€b", "ü€b", kSub) == std::vector<HighlightRange>{{5, 11}});
    // Invalid UTF-8: each bad byte is one code point.
    const std::string bad = std::string("a\xff") + "b\xc3";
    CHECK(hl(bad, "b", kSub) == std::vector<HighlightRange>{{2, 3}});
    CHECK(hl(bad, std::string("\xff"), kSub) == std::vector<HighlightRange>{{1, 2}});
    // A lone escaped lead byte does not match the full code point it starts.
    CHECK_FALSE(score_of("é", std::string("\xc3"), kSub).has_value());
    CHECK_FALSE(score_of("é", std::string("\xc3"), kFuzzy).has_value());
    // NFD and NFC are different strings.
    const std::string nfd = "e\xcc\x81"; // e + combining acute
    CHECK_FALSE(score_of(nfd, "é", kSub).has_value());
    CHECK(hl(nfd + ".txt", "e", kSub) == std::vector<HighlightRange>{{0, 1}});
    CHECK(hl(nfd + "x", nfd, kSub) == std::vector<HighlightRange>{{0, 3}});
}

TEST_CASE("score, match and matches agree", "[search][match]") {
    const Query q = compile_query("ab c", kFuzzy);
    Matcher m(q);
    for (const char* name : {"abc", "a_b_c", "cab", "xaybzc", "ABC.txt", "ac"}) {
        const auto s = m.score(name);
        const auto full = m.match(name);
        CHECK(m.matches(name) == s.has_value());
        CHECK(full.has_value() == s.has_value());
        if (s) {
            CHECK(full.value_or(Match{}).score == *s);
        }
    }
    Matcher copy = m; // copyable, same query
    CHECK(copy.query() == q);
}

TEST_CASE("narrows", "[search][query]") {
    const Query a = compile_query("rep", kSub);
    CHECK(narrows(a, compile_query("repo", kSub)));
    CHECK(narrows(a, compile_query("rep txt", kSub)));
    CHECK(narrows(a, a));
    CHECK_FALSE(narrows(a, compile_query("re", kSub)));
    CHECK_FALSE(narrows(a, compile_query("repo", kFuzzy)));
    CHECK_FALSE(narrows(a, compile_query("xrep", kSub)));
    // An incomplete UTF-8 sequence completed by the next keystroke does not narrow.
    CHECK_FALSE(narrows(compile_query("\xc3", kSub), compile_query("\xc3\xa9", kSub)));
    // A query that may have been cut never narrows to a longer one (conservative near the cap).
    const std::string full(kMaxQueryBytes, 'a');
    const Query cut = compile_query(full + "b", kSub);
    CHECK(narrows(cut, compile_query(full + "bc", kSub))); // both cut to the same text
    const std::string near(kMaxQueryBytes - 2, 'a');
    CHECK_FALSE(narrows(compile_query(near, kSub), compile_query(near + "b", kSub)));
}

TEST_CASE("ranks_before is a deterministic total order", "[search][rank]") {
    std::vector<std::string> names = {"a",     "B",     "b",      "ab",     "file10",
                                      "file9", "File9", "README", "readme", "İ",
                                      "i",     "x.txt", "x.TXT",  "zz",     "é"};
    std::vector<std::string> parents = {"", "dir", "Dir", "dir/sub"};
    struct Row {
        std::int32_t score;
        std::uint32_t depth;
        std::string name;
        std::string parent;
    };
    std::vector<Row> rows;
    for (std::size_t i = 0; i < names.size(); ++i) {
        for (std::size_t p = 0; p < parents.size(); ++p) {
            rows.push_back(Row{static_cast<std::int32_t>((i * 7) % 3) * 10,
                               static_cast<std::uint32_t>(p == 0 ? 1 : 2), names[i], parents[p]});
        }
    }
    const auto cmp = [](const Row& a, const Row& b) {
        return ranks_before(RankInfo{a.score, a.depth, a.name, a.parent},
                            RankInfo{b.score, b.depth, b.name, b.parent});
    };
    // Irreflexive, asymmetric and total for distinct rows.
    for (const Row& a : rows) {
        CHECK_FALSE(cmp(a, a));
        for (const Row& b : rows) {
            if (&a != &b) {
                CHECK(cmp(a, b) != cmp(b, a));
            }
        }
    }
    std::vector<Row> sorted = rows;
    std::sort(sorted.begin(), sorted.end(), cmp);
    std::mt19937 rng(42);
    for (int round = 0; round < 10; ++round) {
        std::vector<Row> shuffled = rows;
        std::shuffle(shuffled.begin(), shuffled.end(), rng);
        std::sort(shuffled.begin(), shuffled.end(), cmp);
        for (std::size_t i = 0; i < rows.size(); ++i) {
            CHECK(shuffled[i].name == sorted[i].name);
            CHECK(shuffled[i].parent == sorted[i].parent);
        }
    }
    // The documented chain.
    CHECK(ranks_before({20, 5, "zzzz", ""}, {10, 1, "a", ""}));        // score first
    CHECK(ranks_before({10, 1, "zzzz", ""}, {10, 2, "a", ""}));        // then depth
    CHECK(ranks_before({10, 1, "zz", ""}, {10, 1, "aaa", ""}));        // then name length
    CHECK(ranks_before({10, 1, "file9", ""}, {10, 1, "filea", ""}));   // compare_names
    CHECK(ranks_before({10, 1, "README", ""}, {10, 1, "readme", ""})); // raw bytes
    CHECK(ranks_before({10, 2, "a", "dir"}, {10, 2, "a", "dir2"}));    // parent
}
