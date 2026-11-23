// Phase 5 gold tests (owner: qa): search ranking on a fixed corpus, Substring vs Fuzzy, Turkish
// I/ı/İ/i folding, NFC vs NFD and invalid UTF-8 names, through every path that ranks names:
//   1. the reference: fs::Walker + search::Matcher + search::ranks_before (the contract),
//   2. the flattened view over the real walk (BrowserModel, Ctrl+E),
//   3. a real name index (create_name_index, IIndex::query),
//   4. the flattened view backed by that index (Ctrl+Shift+E).
// The golden orders below are computed by hand from the scoring constants of search.hpp (the
// comments show the arithmetic), so a ranking change in any path, or in the contract, fails.
// Temporary fixture directories only.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <sys/stat.h>

#include <navigator/fs.hpp>
#include <navigator/index.hpp>
#include <navigator/search.hpp>
#include <navigator/walk.hpp>

#include "browser/browser_model.hpp"
#include "browser_support.hpp"
#include "fixture.hpp"
#include "search_support.hpp"

using namespace navigator;
using namespace navigator::integration_test;
using navigator::search::MatchMode;
using Rels = std::vector<std::string>;

namespace {

// NFC "Café.txt" and NFD "Café.txt" are two different names on disk.
constexpr std::string_view kCafeNfc = "Caf\xC3\xA9.txt";
constexpr std::string_view kCafeNfd = "Cafe\xCC\x81.txt";
constexpr std::string_view kInvalid = "bad\xFFname.txt"; // not UTF-8
// çalışma and çalışma/İzmir.txt (split literals: "\xA7a" would be one hex escape).
constexpr std::string_view kCalisma = "\xC3\xA7"
                                      "al\xC4\xB1\xC5\x9Fma";
constexpr std::string_view kIzmir = "\xC3\xA7"
                                    "al\xC4\xB1\xC5\x9Fma/\xC4\xB0zmir.txt";

struct Corpus {
    TempDir t;
    Corpus() {
        for (const std::string_view d :
             {std::string_view("reports"), std::string_view("docs"), std::string_view("a"),
              std::string_view("b"), kCalisma, std::string_view(".cache"),
              std::string_view(".cache/report")}) {
            std::filesystem::create_directories(t / d);
        }
        for (const std::string_view f :
             {std::string_view("report.txt"),
              std::string_view("Report2024.pdf"),
              std::string_view("annual_report.docx"),
              std::string_view("reports/report.txt"),
              std::string_view("reports/q1-report.md"),
              std::string_view("rePort-final.TXT"),
              std::string_view("my report (1).txt"),
              std::string_view("repo_root.txt"),
              std::string_view("rapport.txt"),
              std::string_view("docs/Report"),
              std::string_view("docs/ReadMe.md"),
              std::string_view("a/notes.md"),
              std::string_view("b/notes.md"),
              std::string_view("istanbul-notes.md"),
              std::string_view("\xC4\xB0stanbul.txt"),           // İstanbul.txt
              std::string_view("ISTANBUL.log"),                  //
              std::string_view("I\xC5\x9F\xC4\xB1k.png"),        // Işık.png
              std::string_view("\xC4\xB1\xC5\x9F\xC4\xB1k.jpg"), // ışık.jpg
              std::string_view("\xC4\xB1l\xC4\xB1k.txt"),        // ılık.txt
              kIzmir,
              kCafeNfc,
              kCafeNfd,
              kInvalid,
              std::string_view(".hidden-report.txt"),
              std::string_view(".cache/report/report.txt")}) {
            REQUIRE(touch(t / f));
        }
    }
    [[nodiscard]] const Path& root() const { return t.path(); }
};

struct Ref {
    std::string rel;
    std::string name;
    std::string parent;
    std::uint32_t depth = 0;
    search::Match match;
};

// Path 1: the contract's own pieces. show_hidden false, like a default flatten.
Rels reference_order(const Path& root, std::string_view text, MatchMode mode) {
    fs::WalkOptions wo;
    wo.root = root;
    Result<fs::Walker> walker = fs::Walker::open(wo);
    REQUIRE(walker.has_value());
    search::Matcher matcher(search::compile_query(text, mode));
    std::vector<std::string> dirs{""};
    std::vector<Ref> refs;
    fs::WalkBatch batch;
    while (true) {
        Result<bool> more = walker->next_batch(batch, {});
        REQUIRE(more.has_value());
        for (std::string& d : batch.new_dirs) {
            dirs.push_back(std::move(d));
        }
        for (const fs::WalkEntry& e : batch.entries) {
            if (std::optional<search::Match> mt = matcher.match(e.entry.name); mt) {
                Ref r;
                r.parent = dirs.at(e.dir);
                r.name = e.entry.name;
                r.rel = r.parent.empty() ? r.name : r.parent + "/" + r.name;
                r.depth = e.depth;
                r.match = std::move(*mt);
                refs.push_back(std::move(r));
            }
        }
        if (!more.value()) {
            break;
        }
    }
    std::sort(refs.begin(), refs.end(), [](const Ref& a, const Ref& b) {
        return search::ranks_before({a.match.score, a.depth, a.name, a.parent},
                                    {b.match.score, b.depth, b.name, b.parent});
    });
    Rels out;
    for (const Ref& r : refs) {
        out.push_back(r.rel);
    }
    return out;
}

// Paths 2-4 must equal `gold`; path 1 too.
void check_all_paths(const Corpus& c, const std::shared_ptr<search::IIndex>& index,
                     std::string_view text, MatchMode mode, const Rels& gold) {
    INFO("query '" << text << "' " << search::to_string(mode));
    CHECK(reference_order(c.root(), text, mode) == gold);
    {
        const View walk(c.root());
        walk.flatten(c.root());
        walk.mode(mode);
        walk.query(text);
        CHECK_FALSE(walk.m().flatten_progress().from_index);
        CHECK(row_rels(walk.m()) == gold);
    }
    search::IndexQuery q;
    q.query = search::compile_query(text, mode);
    q.limit = 0;
    CHECK(query_rels(*index, q) == gold);
    {
        const View via_index(c.root());
        via_index.flatten(c.root(), index);
        via_index.mode(mode);
        via_index.query(text);
        CHECK(via_index.m().flatten_progress().from_index);
        CHECK(row_rels(via_index.m()) == gold);
    }
}

std::shared_ptr<search::IIndex> corpus_index(const Corpus& c) {
    return ready_index(index_options(c.root()));
}

} // namespace

TEST_CASE("Gold: 'report' ranks identically in every path, Substring and Fuzzy", "[search][gold]") {
    const Corpus c;
    const std::shared_ptr<search::IIndex> index = corpus_index(c);
    // Contiguous "report" at p0: 6*16 + 5*24 = 216, + prefix 32 or word start 20, - p0.
    //   docs/Report           216+32+100 (exact name)       = 348  depth 2
    //   report.txt            216+32+60  (exact stem)       = 308  depth 1
    //   reports/report.txt                                  = 308  depth 2
    //   rePort-final.TXT      216+32+20  ('P' camelCase)    = 268
    //   reports               216+32                        = 248  7 bytes
    //   Report2024.pdf        216+32                        = 248  14 bytes
    //   my report (1).txt     216+20-3                      = 233  depth 1
    //   reports/q1-report.md  216+20-3                      = 233  depth 2
    //   annual_report.docx    216+20-7                      = 229
    // Never: rapport.txt (no 'e'), .hidden-report.txt and .cache/report/* (hidden policy).
    const Rels substring{
        "docs/Report",       "report.txt",     "reports/report.txt", "rePort-final.TXT",
        "reports",           "Report2024.pdf", "my report (1).txt",  "reports/q1-report.md",
        "annual_report.docx"};
    check_all_paths(c, index, "report", MatchMode::Substring, substring);
    // Fuzzy adds repo_root.txt: r0 e1 p2 o3 r5 t8 = 6*16 + 32 + 3*24 + 20 (r after '_')
    //   - 3*(1+2) gaps = 211; every Substring score stays (the contiguous alignment is best).
    Rels fuzzy = substring;
    fuzzy.emplace_back("repo_root.txt");
    check_all_paths(c, index, "report", MatchMode::Fuzzy, fuzzy);
    // Case and surrounding spaces do not matter.
    check_all_paths(c, index, "  REPORT ", MatchMode::Substring, substring);
    index->stop();
}

TEST_CASE("Gold: ties break on depth, length, name and finally the parent path", "[search][gold]") {
    const Corpus c;
    const std::shared_ptr<search::IIndex> index = corpus_index(c);
    // notes.md in a/ and b/: 5*16 + 4*24 + 32 + 60 = 268, same depth, length and name: the
    // parent decides. istanbul-notes.md: 176 + 20 - 9 = 187.
    const Rels gold{"a/notes.md", "b/notes.md", "istanbul-notes.md"};
    check_all_paths(c, index, "notes", MatchMode::Substring, gold);
    check_all_paths(c, index, "notes", MatchMode::Fuzzy, gold);
    // Two terms are ANDed: "rep 2024" only finds Report2024.pdf.
    check_all_paths(c, index, "rep 2024", MatchMode::Fuzzy, Rels{"Report2024.pdf"});
    index->stop();
}

TEST_CASE("Gold: Turkish dotted and dotless I fold together everywhere", "[search][gold][i18n]") {
    const Corpus c;
    const std::shared_ptr<search::IIndex> index = corpus_index(c);
    // 8*16 + 7*24 + 32 + 60 (stem) = 388 for ISTANBUL.log and İstanbul.txt; the shorter name in
    // bytes first (İ is two bytes); istanbul-notes.md has no stem bonus: 328.
    const Rels istanbul{"ISTANBUL.log", "\xC4\xB0stanbul.txt", "istanbul-notes.md"};
    for (const char* q :
         {"istanbul", "ISTANBUL", "\xC4\xB0stanbul", "\xC4\xB1stanbul", "\xC4\xB0STANBUL"}) {
        check_all_paths(c, index, q, MatchMode::Substring, istanbul);
        check_all_paths(c, index, q, MatchMode::Fuzzy, istanbul);
    }
    // ışık / IŞIK / Işık: Işık.png (10 bytes) before ışık.jpg (11 bytes); ılık.txt never.
    const Rels isik{"I\xC5\x9F\xC4\xB1k.png", "\xC4\xB1\xC5\x9F\xC4\xB1k.jpg"};
    for (const char* q : {"\xC4\xB1\xC5\x9F\xC4\xB1k", "I\xC5\x9EIK", "i\xC5\x9Fik"}) {
        check_all_paths(c, index, q, MatchMode::Fuzzy, isik);
    }
    // No transliteration: ASCII "isik" does not find ş.
    check_all_paths(c, index, "isik", MatchMode::Fuzzy, Rels{});
    // A folder name with Turkish letters as the parent: çalışma/İzmir.txt.
    check_all_paths(c, index, "izmir", MatchMode::Substring, Rels{std::string(kIzmir)});
    index->stop();
}

TEST_CASE("Gold: the folder filter keeps the sort order and folds Turkish I",
          "[search][gold][i18n]") {
    const Corpus c;
    const View v(c.root());
    v.query("ISTANBUL");
    CHECK(v.m().match_mode() == MatchMode::Substring);
    // compare_names after folding: "istanbul-" < "istanbul.l" < "istanbul.t".
    CHECK(row_names(v.m()) ==
          std::vector<std::string>{"istanbul-notes.md", "ISTANBUL.log", "\xC4\xB0stanbul.txt"});
    v.query("\xC4\xB1\xC5\x9F\xC4\xB1k"); // ışık; both fold to "işik.", so the extension decides
    CHECK(row_names(v.m()) ==
          std::vector<std::string>{"\xC4\xB1\xC5\x9F\xC4\xB1k.jpg", "I\xC5\x9F\xC4\xB1k.png"});
}

TEST_CASE("Gold: NFC and NFD names stay distinct; invalid UTF-8 names are found and opened",
          "[search][gold][unicode]") {
    const Corpus c;
    const std::shared_ptr<search::IIndex> index = corpus_index(c);
    // No normalization: "café" (NFC) finds only the NFC name, "cafe" only the NFD one (its 'e'
    // is followed by U+0301).
    check_all_paths(c, index, "caf\xC3\xA9", MatchMode::Substring, Rels{std::string(kCafeNfc)});
    check_all_paths(c, index, "caf\xC3\xA9", MatchMode::Fuzzy, Rels{std::string(kCafeNfc)});
    check_all_paths(c, index, "cafe", MatchMode::Substring, Rels{std::string(kCafeNfd)});
    check_all_paths(c, index, "cafe", MatchMode::Fuzzy, Rels{std::string(kCafeNfd)});
    // The invalid byte is one escaped code point: "badname" matches only as a subsequence.
    check_all_paths(c, index, "badname", MatchMode::Substring, Rels{});
    check_all_paths(c, index, "badname", MatchMode::Fuzzy, Rels{std::string(kInvalid)});

    // Highlights are byte ranges on code point boundaries of the raw name.
    search::Matcher m(search::compile_query("istanbul", MatchMode::Substring));
    std::optional<search::Match> hit = m.match("\xC4\xB0stanbul.txt");
    REQUIRE(hit.has_value());
    CHECK(hit.value_or(search::Match{}).highlights == std::vector<search::HighlightRange>{{0, 9}});
    search::Matcher nfd(search::compile_query("cafe", MatchMode::Substring));
    hit = nfd.match(kCafeNfd);
    REQUIRE(hit.has_value());
    CHECK(hit.value_or(search::Match{}).highlights ==
          std::vector<search::HighlightRange>{{0, 4}}); // the combining mark is not split off
    search::Matcher bad(search::compile_query("dn", MatchMode::Fuzzy));
    hit = bad.match(kInvalid);
    REQUIRE(hit.has_value());
    for (const search::HighlightRange& r : hit.value_or(search::Match{}).highlights) {
        CHECK_FALSE((r.begin <= 3 && 3 < r.end)); // byte 3 (0xFF) is not part of the match
    }

    // Walk and index list the same raw names; every row's path exists byte for byte.
    const View walk(c.root());
    walk.flatten(c.root());
    const View via_index(c.root());
    via_index.flatten(c.root(), index);
    CHECK(row_rel_set(walk.m()) == row_rel_set(via_index.m()));
    for (const View* v : {&walk, &via_index}) {
        const ui::BrowserModel& bm = v->m();
        std::set<std::string> names;
        for (std::size_t i = 0; i < bm.row_count(); ++i) {
            struct stat st {};
            INFO(bm.row_path(i).native());
            CHECK(::lstat(bm.row_path(i).c_str(), &st) == 0);
            names.insert(bm.row_entry(i).name);
        }
        CHECK(names.contains(std::string(kCafeNfc)));
        CHECK(names.contains(std::string(kCafeNfd)));
        CHECK(names.contains(std::string(kInvalid)));
    }
    // Activating the invalid-UTF-8 row launches exactly those bytes.
    via_index.query("badname");
    REQUIRE(via_index.m().row_count() == 1);
    via_index.m().click_row(0, Modifiers::None);
    via_index.m().activate();
    REQUIRE(via_index.launcher.requests.size() == 1);
    CHECK(via_index.launcher.requests[0].argv.back() == (c.root() / kInvalid).native());
    index->stop();
}

TEST_CASE("Gold: results are identical across runs and readdir orders", "[search][gold]") {
    // The same corpus built in two folders in opposite creation orders ranks the same.
    const Corpus first;
    TempDir second;
    std::vector<std::string> rels;
    for (const auto& e : std::filesystem::recursive_directory_iterator(first.root())) {
        rels.push_back(std::filesystem::relative(e.path(), first.root()).native());
    }
    std::sort(rels.begin(), rels.end());
    std::reverse(rels.begin(), rels.end()); // opposite creation order
    for (const std::string& rel : rels) {
        const Path src = first.root() / rel;
        const Path dst = second / rel;
        std::filesystem::create_directories(dst.parent_path());
        if (std::filesystem::is_directory(src)) {
            std::filesystem::create_directories(dst);
        } else {
            REQUIRE(touch(dst));
        }
    }
    for (const char* q : {"report", "notes", "istanbul", "a", "t"}) {
        INFO(q);
        CHECK(reference_order(first.root(), q, MatchMode::Fuzzy) ==
              reference_order(second.path(), q, MatchMode::Fuzzy));
    }
}
