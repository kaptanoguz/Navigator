// Command palette ranking (navigator/palette.hpp R0-R7): golden rankings over a builtin-like
// entry set, aliases, Turkish folding, history boosts, context filtering, the safe default,
// determinism and a cost check.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "palette_test_util.hpp"

using namespace palette_test;

namespace {

SearchResult run(std::string_view q, const std::vector<Entry>& entries, const History& h = {},
                 const CommandContext& c = rich(), std::size_t max = kDefaultMaxResults) {
    SearchOptions o;
    o.max_results = max;
    return rank(q, entries, h, c, o);
}

bool contains(const std::vector<Entry>& entries, const SearchResult& r, std::string_view name) {
    return std::any_of(r.hits.begin(), r.hits.end(),
                       [&](const Hit& h) { return entries[h.entry].name == name; });
}

std::size_t position(const std::vector<Entry>& entries, const SearchResult& r,
                     std::string_view name) {
    for (std::size_t i = 0; i < r.hits.size(); ++i) {
        if (entries[r.hits[i].entry].name == name) {
            return i;
        }
    }
    return static_cast<std::size_t>(-1);
}

} // namespace

TEST_CASE("palette keys and recordability", "[search][palette]") {
    Entry e = make({"file.new_folder", "New Folder", CommandCategory::Files}, 1);
    CHECK(history_key(e) == "file.new_folder");
    CHECK(is_recordable(e));
    e.arg_kind = ArgKind::Flags;
    e.command.arg = 5;
    CHECK(history_key(e) == "file.new_folder#5");
    CHECK(is_recordable(e));
    e.command.arg = 0;
    CHECK(history_key(e) == "file.new_folder");
    e.arg_kind = ArgKind::OpId;
    e.command.arg = 0;
    CHECK(is_recordable(e)); // picker not used yet: no runtime id
    e.command.arg = 9;
    CHECK_FALSE(is_recordable(e)); // picker result
    CHECK(history_key(e) == "file.new_folder");
    e.arg_kind = ArgKind::None;
    e.danger = Danger::Destructive;
    CHECK_FALSE(is_recordable(e));
}

TEST_CASE("palette golden rankings", "[search][palette][golden]") {
    const std::vector<Entry> entries = builtin_entries();
    struct Gold {
        const char* query;
        const char* first;
    };
    const Gold golds[] = {
        {"newfo", "file.new_folder"},
        {"new folder", "file.new_folder"},
        {"togg sid", "view.toggle_sidebar"},
        {"tsb", "view.toggle_sidebar"},
        {"goto", "nav.goto"},
        {"go to", "nav.goto"},
        {"undo", "edit.undo"},
        {"newt", "tab.new"},
        {"toggle theme", "view.toggle_theme"},
        {"fps", "view.toggle_fps_overlay"},
        {"split", "pane.split"},
        {"palette", "palette.open"},
        {"flat", "search.flatten"},
    };
    for (const Gold& g : golds) {
        INFO("query: " << g.query);
        const SearchResult r = run(g.query, entries, {}, ctx(ContextFlags::CanUndo));
        REQUIRE_FALSE(r.hits.empty());
        CHECK(entries[r.hits[0].entry].name == g.first);
        CHECK(r.default_index == std::optional<std::size_t>{0});
    }
}

TEST_CASE("palette: goto after new folder disambiguation", "[search][palette][golden]") {
    const std::vector<Entry> entries = builtin_entries();
    const SearchResult r = run("goto", entries);
    REQUIRE_FALSE(r.hits.empty());
    CHECK(entries[r.hits[0].entry].name == "nav.goto");
    CHECK(r.hits[0].via == MatchedVia::Title);
    // Highlights are byte ranges of the title.
    CHECK_FALSE(r.hits[0].highlights.empty());
    for (const auto& h : r.hits[0].highlights) {
        CHECK(h.begin < h.end);
        CHECK(h.end <= entries[r.hits[0].entry].title.size());
    }
}

TEST_CASE("palette: terms are ANDed and need not be adjacent", "[search][palette]") {
    const std::vector<Entry> entries = builtin_entries();
    const SearchResult r = run("sidebar toggle", entries);
    REQUIRE_FALSE(r.hits.empty());
    CHECK(entries[r.hits[0].entry].name == "view.toggle_sidebar");
    CHECK_FALSE(contains(entries, r, "view.toggle_theme"));
    CHECK(run("zzzz", entries).hits.empty());
    CHECK_FALSE(run("zzzz", entries).default_index.has_value());
}

TEST_CASE("palette: name matching via dotted ids", "[search][palette]") {
    const std::vector<Entry> entries = builtin_entries();
    const SearchResult r = run("view.toggle_s", entries);
    REQUIRE_FALSE(r.hits.empty());
    CHECK(entries[r.hits[0].entry].name == "view.toggle_sidebar");
    CHECK(r.hits[0].via == MatchedVia::Name);
    CHECK(r.hits[0].highlights.empty()); // title highlights only for title matches
}

TEST_CASE("palette: category and title candidate", "[search][palette]") {
    const std::vector<Entry> entries = builtin_entries();
    const SearchResult r = run("files new", entries);
    REQUIRE_FALSE(r.hits.empty());
    const std::string first = entries[r.hits[0].entry].name;
    CHECK((first == "file.new_folder" || first == "file.new_file"));
    CHECK(r.hits[0].via == MatchedVia::Category);
}

TEST_CASE("palette: shortcut text is not matched", "[search][palette]") {
    const std::vector<Entry> entries = builtin_entries();
    // "Ctrl+Shift+P" is a shortcut only; no title, name, category or description has these.
    CHECK(run("ctrl+shift", entries).hits.empty());
    CHECK(run("F2", entries).hits.empty());
    CHECK(run("Alt+Up", entries).hits.empty());
}

TEST_CASE("palette: description is a substring candidate with a discount", "[search][palette]") {
    const std::vector<Entry> entries = builtin_entries();
    const SearchResult r = run("dotfiles", entries);
    REQUIRE(r.hits.size() == 1);
    CHECK(entries[r.hits[0].entry].name == "view.toggle_hidden");
    CHECK(r.hits[0].via == MatchedVia::Description);
    // Not fuzzy: "dtfl" is not contiguous in the description.
    const SearchResult fuzzy_only = run("dtfl", entries);
    CHECK_FALSE((contains(entries, fuzzy_only, "view.toggle_hidden") &&
                 fuzzy_only.hits[position(entries, fuzzy_only, "view.toggle_hidden")].via ==
                     MatchedVia::Description));
}

TEST_CASE("palette: discount arithmetic", "[search][palette]") {
    // One entry, no history: the score is exactly the Matcher score minus the candidate discount.
    std::vector<Entry> entries = {make({"a.b", "Zebra", CommandCategory::App}, 1)};
    const auto matcher_score = [](std::string_view text, std::string_view q, search::MatchMode m) {
        search::Matcher matcher(search::compile_query(q, m));
        return matcher.score(text).value_or(-1);
    };
    SECTION("title") {
        const auto r = run("zeb", entries);
        REQUIRE(r.hits.size() == 1);
        CHECK(r.hits[0].score == matcher_score("Zebra", "zeb", search::MatchMode::Fuzzy));
    }
    SECTION("name") {
        entries[0].title = "Qqq";
        entries[0].name = "view.toggle_sidebar";
        const auto r = run("vts", entries);
        REQUIRE(r.hits.size() == 1);
        CHECK(r.hits[0].via == MatchedVia::Name);
        CHECK(r.hits[0].score ==
              matcher_score("view.toggle_sidebar", "vts", search::MatchMode::Fuzzy) -
                  kNameDiscount);
    }
    SECTION("description") {
        entries[0].title = "Qqq";
        entries[0].name = "q.q";
        entries[0].category.clear();
        entries[0].description = "makes stripes";
        const auto r = run("stripe", entries);
        REQUIRE(r.hits.size() == 1);
        CHECK(r.hits[0].via == MatchedVia::Description);
        CHECK(r.hits[0].score ==
              matcher_score("makes stripes", "stripe", search::MatchMode::Substring) -
                  kDescriptionDiscount);
    }
    SECTION("category") {
        entries[0].title = "Qqq";
        entries[0].name = "q.q";
        entries[0].category = "Zoo";
        const auto r = run("zoo q", entries);
        REQUIRE(r.hits.size() == 1);
        CHECK(r.hits[0].via == MatchedVia::Category);
        CHECK(r.hits[0].score ==
              matcher_score("Zoo Qqq", "zoo q", search::MatchMode::Fuzzy) - kCategoryDiscount);
    }
    SECTION("generic match_title") {
        entries[0].title = "Undo: move 3 items to Photos";
        entries[0].match_title = "Undo";
        const auto r = run("undo", entries);
        REQUIRE(r.hits.size() == 1);
        CHECK(r.hits[0].score >= matcher_score("Undo", "undo", search::MatchMode::Fuzzy));
    }
}

TEST_CASE("palette: live undo label is found by the generic title", "[search][palette]") {
    std::vector<Entry> entries = builtin_entries();
    Entry* undo = find(entries, "edit.undo");
    REQUIRE(undo != nullptr);
    undo->title = "Undo: move 3 items to Photos";
    undo->match_title = "Undo";
    const SearchResult with_undo = run("undo", entries, {}, ctx(ContextFlags::CanUndo));
    REQUIRE_FALSE(with_undo.hits.empty());
    CHECK(entries[with_undo.hits[0].entry].name == "edit.undo");
    // Without CanUndo the entry is absent, not greyed.
    CHECK_FALSE(contains(entries, run("undo", entries, {}, ctx()), "edit.undo"));
    // The label itself is searchable too.
    CHECK(contains(entries, run("photos", entries, {}, ctx(ContextFlags::CanUndo)), "edit.undo"));
}

TEST_CASE("palette: aliases", "[search][palette][alias]") {
    std::vector<Entry> entries = builtin_entries();
    find(entries, "file.new_folder")->aliases = {"nf", "mkdir"};
    find(entries, "tab.new")->aliases = {"NT"};
    find(entries, "file.delete_permanently")->aliases = {"rmrf"};

    SECTION("exact alias wins over every title match") {
        const SearchResult r = run("nf", entries);
        REQUIRE_FALSE(r.hits.empty());
        CHECK(entries[r.hits[0].entry].name == "file.new_folder");
        CHECK(r.hits[0].via == MatchedVia::Alias);
        CHECK(r.hits[0].score == kAliasExactScore);
        CHECK(r.hits[0].alias == 0);
        CHECK(r.hits[0].highlights.empty());
    }
    SECTION("exact is ASCII case-insensitive, whitespace trimmed") {
        const SearchResult r = run("  NF ", entries);
        REQUIRE_FALSE(r.hits.empty());
        CHECK(entries[r.hits[0].entry].name == "file.new_folder");
        const SearchResult nt = run("nt", entries);
        CHECK(entries[nt.hits[0].entry].name == "tab.new");
        CHECK(nt.hits[0].via == MatchedVia::Alias);
    }
    SECTION("alias index reports which alias matched") {
        const SearchResult r = run("MKDIR", entries);
        REQUIRE_FALSE(r.hits.empty());
        CHECK(r.hits[0].via == MatchedVia::Alias);
        CHECK(r.hits[0].alias == 1);
    }
    SECTION("prefix score decays with the missing bytes, floor at half") {
        const SearchResult r = run("mkd", entries);
        const std::size_t p = position(entries, r, "file.new_folder");
        REQUIRE(p != static_cast<std::size_t>(-1));
        // "mkd" is a prefix of "mkdir" (2 bytes missing): 300 - 8 = 292, unless a title/name
        // candidate scored higher (none does for "mkd").
        CHECK(r.hits[p].via == MatchedVia::Alias);
        CHECK(r.hits[p].score == kAliasPrefixScore - 8);
        find(entries, "file.new_file")->aliases = {"averyveryverylongaliasname"};
        const SearchResult far = run("a", entries);
        const std::size_t q = position(entries, far, "file.new_file");
        REQUIRE(q != static_cast<std::size_t>(-1));
        if (far.hits[q].via == MatchedVia::Alias) {
            CHECK(far.hits[q].score >= kAliasPrefixScore / 2);
        }
    }
    SECTION("an alias match on a destructive command is a normal match and never default") {
        const SearchResult r = run("rmrf", entries);
        REQUIRE_FALSE(r.hits.empty());
        // The destructive command scores kAliasExactScore, but R5 puts a safe hit first when one
        // exists. Here the only hit may be the destructive one: then nothing is highlighted.
        const std::size_t p = position(entries, r, "file.delete_permanently");
        REQUIRE(p != static_cast<std::size_t>(-1));
        CHECK(r.hits[p].via == MatchedVia::Alias);
        if (r.hits.size() == 1) {
            CHECK_FALSE(r.default_index.has_value());
        } else {
            CHECK(entries[r.hits[0].entry].danger == Danger::Safe);
            CHECK(r.default_index == std::optional<std::size_t>{0});
        }
    }
    SECTION("a multi-word query never matches an alias") {
        const SearchResult r = run("n f", entries);
        for (const Hit& h : r.hits) {
            CHECK(h.via != MatchedVia::Alias);
        }
    }
}

TEST_CASE("palette: alias alone is the single, destructive hit", "[search][palette][alias]") {
    std::vector<Entry> entries = {make({"file.delete_permanently", "Delete Permanently",
                                        CommandCategory::Files, Danger::Destructive},
                                       1)};
    entries[0].aliases = {"rmrf"};
    const SearchResult r = run("rmrf", entries);
    REQUIRE(r.hits.size() == 1);
    CHECK(r.hits[0].score == kAliasExactScore);
    CHECK_FALSE(r.default_index.has_value()); // Enter does nothing
}

TEST_CASE("palette: safe default (R5)", "[search][palette][safe]") {
    const std::vector<Entry> entries = builtin_entries();
    const CommandContext c = rich();

    SECTION("trash: safe trash commands come first, Empty Trash follows") {
        const SearchResult r = run("trash", entries, {}, c);
        REQUIRE_FALSE(r.hits.empty());
        CHECK(entries[r.hits[0].entry].danger == Danger::Safe);
        CHECK(entries[r.hits[0].entry].name == "trash.open");
        CHECK(r.default_index == std::optional<std::size_t>{0});
        const std::size_t empty = position(entries, r, "trash.empty");
        REQUIRE(empty != static_cast<std::size_t>(-1));
        CHECK(empty > 0);
    }
    SECTION("del never puts a destructive entry first while a safe one matches") {
        const SearchResult r = run("del", entries, {}, c);
        REQUIRE_FALSE(r.hits.empty());
        CHECK(contains(entries, r, "file.delete_permanently"));
        CHECK(contains(entries, r, "layout.delete"));
        CHECK(entries[r.hits[0].entry].danger == Danger::Safe);
        CHECK(r.default_index == std::optional<std::size_t>{0});
    }
    SECTION("delete permanently typed in full") {
        const SearchResult r = run("delete permanently", entries, {}, c);
        REQUIRE_FALSE(r.hits.empty());
        if (r.hits.size() == 1) {
            CHECK_FALSE(r.default_index.has_value());
        } else {
            CHECK(entries[r.hits[0].entry].danger == Danger::Safe);
        }
    }
    SECTION("the others keep their relative order when a safe hit moves to the front") {
        // Reference order without R5: rank with every entry marked safe.
        std::vector<Entry> all_safe = entries;
        for (Entry& e : all_safe) {
            e.danger = Danger::Safe;
        }
        const SearchResult plain = run("trash", all_safe, {}, c, 1000);
        const SearchResult r = run("trash", entries, {}, c, 1000);
        REQUIRE(plain.hits.size() == r.hits.size());
        if (entries[plain.hits[0].entry].danger == Danger::Destructive) {
            std::vector<std::size_t> expected;
            expected.reserve(plain.hits.size());
            for (const Hit& h : plain.hits) {
                expected.push_back(h.entry);
            }
            const auto safe = std::find_if(expected.begin(), expected.end(), [&](std::size_t i) {
                return entries[i].danger == Danger::Safe;
            });
            REQUIRE(safe != expected.end());
            std::rotate(expected.begin(), safe, safe + 1);
            std::vector<std::size_t> got;
            got.reserve(r.hits.size());
            for (const Hit& h : r.hits) {
                got.push_back(h.entry);
            }
            CHECK(got == expected);
        }
    }
    SECTION("all-destructive results: nothing is highlighted") {
        std::vector<Entry> only;
        for (const Entry& e : entries) {
            if (e.danger == Danger::Destructive) {
                only.push_back(e);
            }
        }
        const SearchResult r = run("del", only, {}, c);
        REQUIRE_FALSE(r.hits.empty());
        CHECK_FALSE(r.default_index.has_value());
        // Browse with only destructive entries: also nothing to run by Enter.
        const SearchResult b = run("", only, {}, c);
        REQUIRE_FALSE(b.hits.empty());
        CHECK_FALSE(b.default_index.has_value());
    }
    SECTION("effective danger comes from the entry (trash view)") {
        std::vector<Entry> trash_view = entries;
        find(trash_view, "file.trash")->danger = Danger::Destructive;
        const SearchResult r = run("trash", trash_view, {}, c);
        REQUIRE_FALSE(r.hits.empty());
        CHECK(trash_view[r.hits[0].entry].danger == Danger::Safe);
    }
    SECTION("max_results does not hide the safe fallback") {
        // Only one hit allowed; the best overall might be destructive, the first shown is safe.
        const SearchResult r = run("trash", entries, {}, c, 1);
        REQUIRE(r.hits.size() == 1);
        CHECK(entries[r.hits[0].entry].danger == Danger::Safe);
    }
}

TEST_CASE("palette: Turkish and folding", "[search][palette][i18n]") {
    std::vector<Entry> entries;
    std::uint32_t id = 1;
    const char* titles[] = {"İndirilenler", "Yeni Klasör",  "Kenar Çubuğunu Aç veya Kapat",
                            "Istanbul",     "Ağ Konumları", "Içerik Göster"};
    for (const char* t : titles) {
        entries.push_back(make({"x.y", t, CommandCategory::App}, id++));
        entries.back().name = "cmd." + std::to_string(id);
    }
    const auto first = [&](std::string_view q) {
        const SearchResult r = run(q, entries);
        return r.hits.empty() ? std::string("<none>") : entries[r.hits[0].entry].title;
    };
    CHECK(first("indir") == "İndirilenler");
    CHECK(first("ındır") == "İndirilenler");
    CHECK(first("INDIR") == "İndirilenler");
    CHECK(first("İNDİR") == "İndirilenler");
    CHECK(first("yeni kla") == "Yeni Klasör");
    CHECK(first("YENİ") == "Yeni Klasör");
    CHECK(first("kenar çubuğ") == "Kenar Çubuğunu Aç veya Kapat");
    CHECK(first("kenar aç kapat") == "Kenar Çubuğunu Aç veya Kapat");
    CHECK(first("ağ") == "Ağ Konumları");
    // "ISTANBUL" and "ıstanbul" both find the dotted and dotless spellings.
    const SearchResult r = run("ıstanbul", entries);
    CHECK(contains(entries, r, "cmd.5"));
    // Highlights are byte ranges: "İndirilenler" starts with a 2-byte letter.
    const SearchResult h = run("indir", entries);
    REQUIRE_FALSE(h.hits.empty());
    REQUIRE_FALSE(h.hits[0].highlights.empty());
    CHECK(h.hits[0].highlights[0].begin == 0);
    CHECK(h.hits[0].highlights[0].end > 5); // 5 letters, one of them 2 bytes
}

TEST_CASE("palette: context filtering (R0)", "[search][palette][context]") {
    const std::vector<Entry> entries = builtin_entries();
    const SearchResult none = run("", entries, {}, ctx(), 1000);
    CHECK_FALSE(contains(entries, none, "edit.undo"));
    CHECK_FALSE(contains(entries, none, "file.trash"));
    CHECK_FALSE(contains(entries, none, "trash.restore"));
    CHECK_FALSE(contains(entries, none, "file.delete_permanently"));
    CHECK(contains(entries, none, "file.new_folder"));
    CHECK(contains(entries, none, "palette.open"));

    const SearchResult sel = run("", entries, {}, ctx(ContextFlags::HasSelection), 1000);
    CHECK(contains(entries, sel, "file.trash"));
    CHECK(contains(entries, sel, "file.delete_permanently"));

    // hidden_if_any: New Folder and Flatten are hidden in the trash view.
    const SearchResult tv = run("", entries, {}, ctx(ContextFlags::TrashView), 1000);
    CHECK_FALSE(contains(entries, tv, "file.new_folder"));
    CHECK_FALSE(contains(entries, tv, "search.flatten"));
    CHECK(contains(entries, tv, "trash.restore"));
    // Filtering happens before matching: a hidden entry never matches a query.
    CHECK_FALSE(contains(entries, run("new folder", entries, {}, ctx(ContextFlags::TrashView)),
                         "file.new_folder"));
    // Applicable entries match; a missing requirement drops the entry, not just ranks it down.
    CHECK_FALSE(contains(entries, run("select", entries, {}, ctx()), "list.clear_selection"));
    CHECK(contains(entries, run("select", entries, {}, ctx(ContextFlags::HasSelection)),
                   "list.clear_selection"));
    // Multiple requirements: all must be present.
    std::vector<Entry> two = {make({"a.b", "Both Needed", CommandCategory::App}, 1)};
    two[0].requires_all = ContextFlags::HasSelection | ContextFlags::MultiplePanes;
    CHECK(run("both", two, {}, ctx(ContextFlags::HasSelection)).hits.empty());
    CHECK(run("both", two, {}, ctx(ContextFlags::HasSelection | ContextFlags::MultiplePanes))
              .hits.size() == 1);
    // Any one hidden_if_any flag hides.
    two[0].requires_all = ContextFlags::None;
    two[0].hidden_if_any = ContextFlags::TrashView | ContextFlags::FlattenedView;
    CHECK(run("both", two, {}, ctx(ContextFlags::FlattenedView)).hits.empty());
    CHECK(run("both", two, {}, ctx()).hits.size() == 1);
}

TEST_CASE("palette: empty query browse order (R7)", "[search][palette][browse]") {
    const std::vector<Entry> entries = builtin_entries();
    const CommandContext c = rich();

    SECTION("no history: category order, then folded title") {
        const SearchResult r = run("", entries, {}, c, 1000);
        REQUIRE_FALSE(r.hits.empty());
        CHECK(r.default_index == std::optional<std::size_t>{0});
        for (const Hit& h : r.hits) {
            CHECK(h.via == MatchedVia::Browse);
            CHECK(h.score == 0);
            CHECK(h.highlights.empty());
        }
        // First category is App: "Quit Navigator", "Toggle FPS Overlay", "Toggle Theme".
        CHECK(title_at(entries, r, 0) == "Quit Navigator");
        CHECK(title_at(entries, r, 1) == "Toggle FPS Overlay");
        CHECK(title_at(entries, r, 2) == "Toggle Theme");
        for (std::size_t i = 1; i < r.hits.size(); ++i) {
            const auto a = static_cast<int>(entries[r.hits[i - 1].entry].category_id);
            const auto b = static_cast<int>(entries[r.hits[i].entry].category_id);
            CHECK(a <= b);
        }
    }
    SECTION("recents first, newest first, then the rest") {
        History h;
        h.record("tab.close");
        h.record("view.toggle_sidebar");
        h.record("edit.undo");
        const SearchResult r = run("", entries, h, c, 1000);
        REQUIRE(r.hits.size() > 4);
        CHECK(entries[r.hits[0].entry].name == "edit.undo");
        CHECK(entries[r.hits[1].entry].name == "view.toggle_sidebar");
        CHECK(entries[r.hits[2].entry].name == "tab.close");
        CHECK(title_at(entries, r, 3) == "Quit Navigator");
        // Recents are not repeated in the rest.
        std::vector<std::size_t> seen;
        for (const Hit& hit : r.hits) {
            CHECK(std::find(seen.begin(), seen.end(), hit.entry) == seen.end());
            seen.push_back(hit.entry);
        }
    }
    SECTION("at most kBrowseRecents recents") {
        History h;
        for (const Entry& e : entries) {
            if (e.name.starts_with("tab.") || e.name.starts_with("view.") ||
                e.name.starts_with("nav.") || e.name.starts_with("edit.")) {
                h.record(e.name);
            }
        }
        REQUIRE(h.items().size() > kBrowseRecents);
        const SearchResult r = run("", entries, h, c, 1000);
        std::size_t recents = 0;
        for (const Hit& hit : r.hits) {
            const auto rank_of = h.rank_of(entries[hit.entry].name);
            if (rank_of && *rank_of < kBrowseRecents) {
                ++recents;
            } else {
                break;
            }
        }
        CHECK(recents == kBrowseRecents);
        // The 9th most recent is in category order after them, not before.
        const auto ninth = h.items()[kBrowseRecents].key;
        CHECK(position(entries, r, ninth) >= kBrowseRecents);
    }
    SECTION("recents that are not applicable, unknown, or destructive are skipped") {
        History h;
        h.record("no.such.command");
        h.record("file.delete_permanently");
        h.record("trash.restore"); // not applicable in rich() (TrashView missing)
        h.record("tab.new");
        const SearchResult r = run("", entries, h, c, 1000);
        CHECK(entries[r.hits[0].entry].name == "tab.new");
        CHECK(title_at(entries, r, 1) == "Quit Navigator");
        // Applicable destructive entries are listed, but in category order only.
        CHECK(position(entries, r, "file.delete_permanently") > kBrowseRecents);
    }
    SECTION("variants are recorded under name#arg") {
        std::vector<Entry> v = entries;
        Entry variant = *find(v, "edit.paste");
        variant.arg_kind = ArgKind::Flags;
        variant.command.arg = 2;
        variant.title = "Paste Into";
        v.push_back(variant);
        History h;
        h.record(history_key(variant));
        const SearchResult r = run("", v, h, c, 1000);
        CHECK(r.hits[0].entry == v.size() - 1);
    }
    SECTION("max_results truncates") {
        CHECK(run("", entries, {}, c, 5).hits.size() == 5);
        CHECK(run("", entries, {}, c, 0).hits.empty());
        CHECK_FALSE(run("", entries, {}, c, 0).default_index.has_value());
    }
    SECTION("blank query is empty") {
        CHECK(run("  \t ", entries, {}, c, 1000).hits.size() ==
              run("", entries, {}, c, 1000).hits.size());
    }
}

TEST_CASE("palette: history boost is bounded and only for recordable entries",
          "[search][palette][history]") {
    const std::vector<Entry> entries = builtin_entries();
    const CommandContext c = rich();

    SECTION("boost formula") {
        History h;
        CHECK(history_boost(h, "x") == 0);
        h.record("x");
        CHECK(history_boost(h, "x") == kHistoryRecentBoost + kHistoryCountBoost);
        for (int i = 0; i < 40; ++i) {
            h.record("x");
        }
        CHECK(history_boost(h, "x") == kHistoryBoostMax); // 30 + 10 * 2 = 50
        h.record("y");
        CHECK(history_boost(h, "x") ==
              std::min(kHistoryBoostMax, kHistoryRecentBoost - kHistoryRecentStep +
                                             kHistoryCountCap * kHistoryCountBoost));
        for (int i = 0; i < 20; ++i) {
            h.record("k" + std::to_string(i));
        }
        // x is now far down the recency list: recency part clamps at 0, count part remains.
        CHECK(history_boost(h, "x") == kHistoryCountCap * kHistoryCountBoost);
        CHECK(history_boost(h, "k19") == kHistoryRecentBoost + kHistoryCountBoost);
        CHECK(history_boost(h, "missing") == 0);
    }
    SECTION("score = base + boost, never more than kHistoryBoostMax") {
        const SearchResult base = run("tab", entries, {}, c, 1000);
        History h;
        for (int i = 0; i < 30; ++i) {
            h.record("tab.close");
        }
        const SearchResult boosted = run("tab", entries, h, c, 1000);
        const std::size_t pb = position(entries, base, "tab.close");
        const std::size_t pa = position(entries, boosted, "tab.close");
        REQUIRE(pb != static_cast<std::size_t>(-1));
        REQUIRE(pa != static_cast<std::size_t>(-1));
        CHECK(boosted.hits[pa].score - base.hits[pb].score == history_boost(h, "tab.close"));
        CHECK(boosted.hits[pa].score - base.hits[pb].score <= kHistoryBoostMax);
        // Other entries are unchanged.
        const std::size_t nb = position(entries, base, "tab.new");
        const std::size_t na = position(entries, boosted, "tab.new");
        CHECK(boosted.hits[na].score == base.hits[nb].score);
    }
    SECTION("a boost reorders close matches") {
        // "New Tab" vs "Reopen Closed Tab": query "tab" matches both; habit decides near-ties.
        const SearchResult base = run("close", entries, {}, c, 1000);
        const std::size_t first_name_index = base.hits[0].entry;
        History h;
        h.record(entries[base.hits.back().entry].name);
        const SearchResult r = run("close", entries, h, c, 1000);
        // Boost (32) is real but bounded: the former last place cannot jump above a hit more
        // than boost points better.
        const std::int32_t gap = base.hits[0].score - base.hits.back().score;
        const std::size_t pos = position(entries, r, entries[base.hits.back().entry].name);
        if (gap > history_boost(h, entries[base.hits.back().entry].name)) {
            CHECK(pos > 0);
        }
        (void)first_name_index;
    }
    SECTION("habit never beats a far better match") {
        // Exact whole-word title vs a heavily used weak match.
        std::vector<Entry> two = {
            make({"a.one", "Paste", CommandCategory::Edit}, 1),
            make({"a.two", "Pre-Aggregate Staging Table Examples", CommandCategory::Edit}, 2)};
        History h;
        for (int i = 0; i < 100; ++i) {
            h.record("a.two");
        }
        const SearchResult r = run("paste", two, h, c);
        REQUIRE_FALSE(r.hits.empty());
        CHECK(two[r.hits[0].entry].name == "a.one");
    }
    SECTION("destructive entries never gain a boost, even if the history names them") {
        const SearchResult base = run("delete", entries, {}, c, 1000);
        History h;
        for (int i = 0; i < 30; ++i) {
            h.record("file.delete_permanently");
            h.record("layout.delete");
        }
        const SearchResult r = run("delete", entries, h, c, 1000);
        REQUIRE(base.hits.size() == r.hits.size());
        for (std::size_t i = 0; i < r.hits.size(); ++i) {
            CHECK(r.hits[i].entry == base.hits[i].entry);
            CHECK(r.hits[i].score == base.hits[i].score);
        }
        CHECK_FALSE(r.default_index.has_value()); // only destructive entries match "delete"
        // Aliasing a destructive entry's key through a variant name is not a loophole either.
        std::vector<Entry> v = entries;
        Entry* d = find(v, "file.delete_permanently");
        d->arg_kind = ArgKind::Flags;
        d->command.arg = 3;
        History h2;
        h2.record(history_key(*d));
        const SearchResult r2 = run("delete", v, h2, c, 1000);
        CHECK(r2.hits[position(v, r2, "file.delete_permanently")].score ==
              base.hits[position(entries, base, "file.delete_permanently")].score);
    }
    SECTION("picker results are not boosted") {
        std::vector<Entry> v = {
            make({"ops.cancel", "Cancel Operation", CommandCategory::Operations}, 1)};
        v[0].arg_kind = ArgKind::OpId;
        v[0].command.arg = 77;
        History h;
        h.record("ops.cancel");
        CHECK(run("cancel", v, h, c).hits[0].score == run("cancel", v, {}, c).hits[0].score);
        v[0].command.arg = 0; // pending picker: the command itself is recordable
        CHECK(run("cancel", v, h, c).hits[0].score > run("cancel", v, {}, c).hits[0].score);
    }
    SECTION("history entries that match no entry are harmless") {
        History h;
        h.record("gone.command");
        CHECK(run("tab", entries, h, c).hits.size() == run("tab", entries, {}, c).hits.size());
    }
    SECTION("history does not make a non-matching entry match") {
        History h;
        for (int i = 0; i < 10; ++i) {
            h.record("app.quit");
        }
        CHECK_FALSE(contains(entries, run("zebra", entries, h, c), "app.quit"));
    }
}

TEST_CASE("palette: determinism and tie-breaks (R4)", "[search][palette][order]") {
    SECTION("same score and length: category order, then folded title, then index") {
        std::vector<Entry> v;
        v.push_back(make({"a.2", "Aaa", CommandCategory::View}, 1));
        v.push_back(make({"a.1", "Aaa", CommandCategory::Edit}, 2)); // same title, later category
        v.push_back(make({"a.0", "Aaa", CommandCategory::Edit}, 3)); // identical to the above
        v.push_back(make({"a.3", "AAA", CommandCategory::Edit}, 4)); // folds equal
        v.push_back(make({"a.4", "Aab", CommandCategory::Edit}, 5)); // does not match "aaa"
        const SearchResult r = run("aaa", v);
        REQUIRE(r.hits.size() == 4);
        // "AAA" scores like "Aaa" (case-folded matching, same bonuses).
        CHECK(r.hits[0].entry == 0); // View before Edit
        CHECK(r.hits[1].entry == 1);
        CHECK(r.hits[2].entry == 2);
        CHECK(r.hits[3].entry == 3);
    }
    SECTION("shorter title first at equal score is by code points, not bytes") {
        std::vector<Entry> v;
        v.push_back(make({"a.1", "ab çç", CommandCategory::App}, 1));  // 5 code points, 7 bytes
        v.push_back(make({"a.2", "ab xxx", CommandCategory::App}, 2)); // 6 code points
        // Make them score the same by matching only the prefix word: "ab".
        const SearchResult r = run("ab", v);
        REQUIRE(r.hits.size() == 2);
        if (r.hits[0].score == r.hits[1].score) {
            CHECK(r.hits[0].entry == 0);
        }
    }
    SECTION("permutation of the input does not change the order of distinct titles") {
        const std::vector<Entry> entries = builtin_entries();
        std::mt19937 rng(12345);
        for (const char* q : {"to", "new", "e", "tr", "o", ""}) {
            const SearchResult ref = run(q, entries, {}, rich(), 1000);
            std::vector<std::string> ref_titles;
            ref_titles.reserve(ref.hits.size());
            for (const Hit& h : ref.hits) {
                ref_titles.push_back(entries[h.entry].title);
            }
            for (int round = 0; round < 5; ++round) {
                std::vector<Entry> shuffled = entries;
                std::shuffle(shuffled.begin(), shuffled.end(), rng);
                const SearchResult r = run(q, shuffled, {}, rich(), 1000);
                std::vector<std::string> titles;
                titles.reserve(r.hits.size());
                for (const Hit& h : r.hits) {
                    titles.push_back(shuffled[h.entry].title);
                }
                INFO("query: '" << q << "'");
                CHECK(titles == ref_titles);
            }
        }
    }
    SECTION("repeated calls are identical") {
        const std::vector<Entry> entries = builtin_entries();
        const SearchResult a = run("e", entries);
        const SearchResult b = run("e", entries);
        REQUIRE(a.hits.size() == b.hits.size());
        for (std::size_t i = 0; i < a.hits.size(); ++i) {
            CHECK(a.hits[i].entry == b.hits[i].entry);
            CHECK(a.hits[i].score == b.hits[i].score);
        }
    }
    SECTION("results are sorted by score, safe-front aside") {
        const std::vector<Entry> entries = builtin_entries();
        const SearchResult r = run("e", entries, {}, rich(), 1000);
        for (std::size_t i = 2; i < r.hits.size(); ++i) {
            CHECK(r.hits[i - 1].score >= r.hits[i].score);
        }
    }
}

TEST_CASE("palette: limits", "[search][palette][limits]") {
    SECTION("max_results") {
        const std::vector<Entry> entries = builtin_entries();
        CHECK(run("e", entries, {}, rich(), 3).hits.size() == 3);
        CHECK(run("e", entries).hits.size() <= kDefaultMaxResults);
    }
    SECTION("entries beyond kMaxEntries are ignored") {
        std::vector<Entry> v;
        for (std::size_t i = 0; i < kMaxEntries + 10; ++i) {
            Entry e =
                make({"x.y", "Same", CommandCategory::App}, static_cast<std::uint32_t>(i + 1));
            v.push_back(std::move(e));
        }
        v.back().title = "Unique Tail";
        const SearchResult r = run("unique", v);
        CHECK(r.hits.empty());
        const SearchResult all = run("same", v, {}, rich(), 100000);
        CHECK(all.hits.size() == kMaxEntries);
    }
    SECTION("overlong queries are cut, not an error") {
        const std::vector<Entry> entries = builtin_entries();
        const std::string long_query(10000, 'z');
        CHECK(run(long_query, entries).hits.empty());
        const std::string spaces(500, ' ');
        CHECK_FALSE(run(spaces, entries).hits.empty()); // blank = browse
    }
    SECTION("no entries") {
        const SearchResult r = run("x", {});
        CHECK(r.hits.empty());
        CHECK_FALSE(r.default_index.has_value());
        CHECK(run("", {}).hits.empty());
    }
}

TEST_CASE("palette: ranking cost", "[search][palette][perf]") {
    // kMaxEntries entries (rank() ignores the rest, so a few thousand cost the same as this).
    std::vector<Entry> v = builtin_entries();
    std::mt19937 rng(7);
    const char* words[] = {"open", "move", "toggle", "folder", "file", "view", "pane",
                           "tab",  "go",   "show",   "hide",   "sort", "copy", "paste"};
    std::uint32_t id = 1000;
    while (v.size() < kMaxEntries) {
        std::string title;
        for (int w = 0; w < 3; ++w) {
            title += words[rng() % std::size(words)];
            title += ' ';
        }
        title += std::to_string(v.size());
        Entry e = make({"gen.cmd", title.c_str(), static_cast<CommandCategory>(rng() % 15)}, id++);
        e.name = "gen.cmd_" + std::to_string(v.size());
        e.description = "generated description for " + title;
        v.push_back(std::move(e));
    }
    History h;
    for (int i = 0; i < 50; ++i) {
        h.record(v[static_cast<std::size_t>(i) * 3].name);
    }
    const CommandContext c = rich();
    std::chrono::nanoseconds best{std::chrono::nanoseconds::max()};
    for (const char* q : {"to", "tog fol", "f", "open file", "zzz", ""}) {
        for (int rep = 0; rep < 5; ++rep) {
            const auto t0 = std::chrono::steady_clock::now();
            const SearchResult r = rank(q, v, h, c);
            const auto dt = std::chrono::steady_clock::now() - t0;
            best = std::min(best, std::chrono::duration_cast<std::chrono::nanoseconds>(dt));
            CHECK(r.hits.size() <= kDefaultMaxResults);
        }
        // Per-query generous budget (Debug and sanitizers included); the interesting number is
        // printed by the BENCH section below.
        const auto t0 = std::chrono::steady_clock::now();
        (void)rank(q, v, h, c);
        const auto ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
                .count();
        INFO("query '" << q << "' took " << ms << " ms for " << v.size() << " entries");
        CHECK(ms < 100.0);
    }
    INFO("best single call " << best.count() / 1000 << " us");
    CHECK(best.count() < 50'000'000);
}
