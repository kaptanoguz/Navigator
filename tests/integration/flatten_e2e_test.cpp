// Phase 5 flattened view end to end (owner: qa): the real fs::Walker (Ctrl+E) and a real name
// index (Ctrl+Shift+E) behind BrowserModel on fixture trees.
//   - Hidden policy: children of hidden folders never appear unless show_hidden (File Pilot
//     v0.5 bugfix, walk.hpp "Hidden policy"), in both the walk- and the index-backed view.
//   - Permission-denied folders are skipped and counted, never fatal.
//   - The type filter gives the same rows and popup in walk- and index-backed views.
//   - Cancel mid-walk and mid-index-query: rows found so far stay and are never inconsistent
//     (no duplicates, every row exists, breadth-first completeness, every row matches the query
//     the rows reflect), and nothing arrives after the cancel.
// Temporary fixture directories only; unreadable folders get their mode back before removal.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <sys/stat.h>
#include <unistd.h>

#include <navigator/fs.hpp>
#include <navigator/index.hpp>
#include <navigator/search.hpp>
#include <navigator/walk.hpp>

#include "browser/browser_model.hpp"
#include "browser_support.hpp"
#include "fixture.hpp"
#include "search/flatten_walk.hpp"
#include "search_support.hpp"

using namespace navigator;
using namespace navigator::integration_test;
using Rels = std::set<std::string>;

namespace {

void make_tree(const TempDir& t, const std::vector<std::string>& dirs,
               const std::vector<std::string>& files) {
    for (const std::string& d : dirs) {
        std::filesystem::create_directories(t / d);
    }
    for (const std::string& f : files) {
        REQUIRE(touch(t / f));
    }
}

// Visible: top.txt, dir/, dir/keep.txt, dir/sub/, dir/sub/deep.txt.
// Hidden: .secret/ (with children two levels down), dir/.git/ (with children), .dotfile,
// dir/.env, and dir/sub/.cache/x.txt.
struct HiddenTree {
    TempDir t;
    HiddenTree() {
        make_tree(t, {".secret/inner", "dir/.git/objects", "dir/sub/.cache"},
                  {"top.txt", ".dotfile", ".secret/child.txt", ".secret/inner/deeper.txt",
                   "dir/keep.txt", "dir/.env", "dir/.git/config", "dir/.git/objects/ab",
                   "dir/sub/deep.txt", "dir/sub/.cache/x.txt"});
    }
    static const Rels& visible() {
        static const Rels r{"top.txt", "dir", "dir/keep.txt", "dir/sub", "dir/sub/deep.txt"};
        return r;
    }
    static const Rels& all() {
        static const Rels r{"top.txt",
                            ".dotfile",
                            ".secret",
                            ".secret/child.txt",
                            ".secret/inner",
                            ".secret/inner/deeper.txt",
                            "dir",
                            "dir/keep.txt",
                            "dir/.env",
                            "dir/.git",
                            "dir/.git/config",
                            "dir/.git/objects",
                            "dir/.git/objects/ab",
                            "dir/sub",
                            "dir/sub/deep.txt",
                            "dir/sub/.cache",
                            "dir/sub/.cache/x.txt"};
        return r;
    }
};

bool has_hidden_component(const std::string& rel) {
    std::size_t start = 0;
    while (start <= rel.size()) {
        const std::size_t slash = rel.find('/', start);
        const std::string part =
            rel.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (fs::is_hidden_name(part)) {
            return true;
        }
        if (slash == std::string::npos) {
            break;
        }
        start = slash + 1;
    }
    return false;
}

// Restores a folder's mode before the fixture is removed.
struct ModeGuard {
    Path path;
    explicit ModeGuard(Path p, mode_t mode) : path(std::move(p)) {
        REQUIRE(::chmod(path.c_str(), mode) == 0);
    }
    ModeGuard(const ModeGuard&) = delete;
    ModeGuard& operator=(const ModeGuard&) = delete;
    ModeGuard(ModeGuard&&) = delete;
    ModeGuard& operator=(ModeGuard&&) = delete;
    ~ModeGuard() { (void)::chmod(path.c_str(), 0755); }
};

// Rows of the current view must be consistent at all times. While a match/order or index job
// is outstanding the rows are the previous query's ("old rows stay until the newest result",
// docs/architecture.md), so the query check applies once nothing is organizing.
std::string row_problem(const ui::BrowserModel& m) {
    std::set<std::string> seen;
    const bool settled = !m.flatten_progress().organizing;
    search::Matcher matcher(settled ? m.query() : search::Query{});
    for (std::size_t i = 0; i < m.row_count(); ++i) {
        const std::string rel = row_rel(m, i);
        if (!seen.insert(rel).second) {
            return "duplicate row " + rel;
        }
        if (m.row_path(i) != m.flatten_root() / rel) {
            return "row_path differs from root/parent/name for " + rel;
        }
        if (!matcher.matches(m.row_entry(i).name)) {
            return "row " + rel + " does not match the query the rows reflect";
        }
        if (!m.show_hidden() && has_hidden_component(rel)) {
            return "hidden row " + rel;
        }
    }
    if (m.row_count() > 0 && m.cursor() >= m.row_count()) {
        return "cursor out of range";
    }
    return {};
}

} // namespace

TEST_CASE("Flatten never shows children of hidden folders unless show_hidden (walk)",
          "[flatten][hidden][e2e]") {
    const HiddenTree h;
    const View v(h.t.path());
    v.flatten(h.t.path());
    CHECK(row_rel_set(v.m()) == HiddenTree::visible());
    const ui::FlattenProgress p = v.m().flatten_progress();
    CHECK(p.found == HiddenTree::visible().size());
    CHECK(p.stats.hidden_skipped == 5); // .dotfile, .secret, dir/.env, dir/.git, dir/sub/.cache
    // A query that only hidden entries would match finds nothing.
    v.query("deeper");
    CHECK(v.m().row_count() == 0);
    v.query("config");
    CHECK(v.m().row_count() == 0);
    v.query("");
    // view.toggle_hidden restarts the walk with show_hidden.
    v.m().toggle_hidden();
    REQUIRE(pump(v.m(), [&] { return !v.m().busy(); }));
    REQUIRE(v.m().settle_search(kSearchWait));
    CHECK(v.m().show_hidden());
    CHECK(v.m().flattened());
    CHECK(row_rel_set(v.m()) == HiddenTree::all());
    v.m().toggle_hidden();
    REQUIRE(pump(v.m(), [&] { return !v.m().busy(); }));
    REQUIRE(v.m().settle_search(kSearchWait));
    CHECK(row_rel_set(v.m()) == HiddenTree::visible());
    CHECK(row_problem(v.m()).empty());
}

TEST_CASE("Flatten inside a hidden folder walks it and still hides what is hidden below",
          "[flatten][hidden][e2e]") {
    const HiddenTree h;
    const View v(h.t / ".secret");
    v.flatten(h.t / ".secret");
    CHECK(row_rel_set(v.m()) == Rels{"child.txt", "inner", "inner/deeper.txt"});
    // The index answers a query under a hidden folder the same way ("relative to under").
    const std::shared_ptr<search::IIndex> index = ready_index(index_options(h.t.path()));
    search::IndexQuery q;
    q.under = h.t / ".secret";
    q.limit = 0;
    const std::vector<std::string> rels = query_rels(*index, q);
    CHECK(Rels(rels.begin(), rels.end()) == Rels{"child.txt", "inner", "inner/deeper.txt"});
    q.under = h.t / "dir";
    const std::vector<std::string> dir_rels = query_rels(*index, q);
    CHECK(Rels(dir_rels.begin(), dir_rels.end()) == Rels{"keep.txt", "sub", "sub/deep.txt"});
    index->stop();
}

TEST_CASE("The index-backed flatten (Ctrl+Shift+E) hides children of hidden folders too",
          "[flatten][hidden][index][e2e]") {
    const HiddenTree h;
    const std::shared_ptr<search::IIndex> index = ready_index(index_options(h.t.path()));
    // Hidden entries ARE indexed (index.hpp "Scanning"): the query applies the policy.
    CHECK(index->status().entries == HiddenTree::all().size());
    const View v(h.t.path());
    v.flatten(h.t.path(), index);
    REQUIRE(v.m().flatten_progress().from_index);
    CHECK(row_rel_set(v.m()) == HiddenTree::visible());
    for (const char* q : {"deeper", "config", "x.txt", "env", "ab"}) {
        INFO(q);
        v.query(q);
        CHECK(v.m().row_count() == 0);
        CHECK(row_problem(v.m()).empty());
    }
    v.query("");
    v.m().toggle_hidden();
    REQUIRE(pump(v.m(), [&] { return !v.m().busy(); }));
    REQUIRE(v.m().settle_search(kSearchWait));
    CHECK(row_rel_set(v.m()) == HiddenTree::all());
    v.query("deeper");
    CHECK(row_rels(v.m()) == std::vector<std::string>{".secret/inner/deeper.txt"});
    v.m().toggle_hidden();
    REQUIRE(pump(v.m(), [&] { return !v.m().busy(); }));
    REQUIRE(v.m().settle_search(kSearchWait));
    CHECK(v.m().row_count() == 0);
    index->stop();
}

TEST_CASE("Permission-denied folders are skipped and counted in walk and index",
          "[flatten][permission][e2e]") {
    if (::geteuid() == 0) {
        SKIP("root reads every folder");
    }
    TempDir t;
    make_tree(t, {"open/sub", "locked/inside", "open/sub/also_locked"},
              {"open/a.txt", "open/sub/b.txt", "locked/secret.txt", "locked/inside/deep.txt",
               "open/sub/also_locked/c.txt", "z.txt"});
    const ModeGuard g1(t / "locked", 0000);
    const ModeGuard g2(t / "open/sub/also_locked", 0300); // -wx: entered, not listable
    const Rels shown{"open",   "open/a.txt", "open/sub", "open/sub/b.txt", "open/sub/also_locked",
                     "locked", "z.txt"};

    const View v(t.path());
    v.flatten(t.path());
    CHECK(row_rel_set(v.m()) == shown); // the folders themselves are listed, not their content
    const ui::FlattenProgress p = v.m().flatten_progress();
    CHECK_FALSE(p.error.has_value());
    CHECK(p.stats.skipped_permission == 2);
    CHECK(p.skipped == 2);
    std::set<std::string> samples;
    for (const fs::WalkSkip& s : p.stats.samples) {
        CHECK(s.code == ErrorCode::PermissionDenied);
        samples.insert(s.relative_path);
    }
    CHECK(samples == std::set<std::string>{"locked", "open/sub/also_locked"});

    const std::shared_ptr<search::IIndex> index = ready_index(index_options(t.path()));
    CHECK(index->status().skipped_permission == 2);
    const View vi(t.path());
    vi.flatten(t.path(), index);
    CHECK(row_rel_set(vi.m()) == shown);
    index->stop();
}

TEST_CASE("An unreadable flatten root fails cleanly", "[flatten][permission][e2e]") {
    if (::geteuid() == 0) {
        SKIP("root reads every folder");
    }
    TempDir t;
    make_tree(t, {"box"}, {"box/x.txt"});
    const View v(t / "box");
    const ModeGuard g(t / "box", 0000);
    v.m().flatten(ui::FlattenRequest{});
    REQUIRE(v.m().settle_search(kSearchWait));
    const std::optional<Error> e = v.m().flatten_progress().error;
    REQUIRE(e.has_value());
    CHECK(e.value_or(Error{}).code == ErrorCode::PermissionDenied);
    CHECK(v.m().row_count() == 0);
}

// Folders, links to folders and files, a dangling link, names with and without extensions.
struct TypeTree {
    TempDir t;
    TypeTree() {
        make_tree(t, {"pics", "pics/raw.d"},
                  {"pics/a.JPG", "pics/b.jpg", "Makefile", "notes.txt", "x.", ".h.jpg",
                   "pics/raw.d/c.png"});
        std::filesystem::create_directory_symlink("pics", t / "linkdir");
        std::filesystem::create_symlink("notes.txt", t / "linkfile");
        std::filesystem::create_symlink("nowhere", t / "dangling.lnk");
    }
};

std::map<std::string, std::size_t> summary_of(const ui::BrowserModel& m) {
    std::map<std::string, std::size_t> out;
    for (const search::TypeCount& c : m.type_summary()) {
        const std::string key = c.key.kind == search::TypeKind::Folder        ? "<folder>"
                                : c.key.kind == search::TypeKind::NoExtension ? "<none>"
                                                                              : c.key.extension;
        out[key] = c.count;
    }
    return out;
}

TEST_CASE("Walk- and index-backed views agree on the type filter (links to folders included)",
          "[flatten][types][index][e2e]") {
    const TypeTree tt;
    const std::shared_ptr<search::IIndex> index = ready_index(index_options(tt.t.path()));
    const View walk(tt.t.path());
    walk.flatten(tt.t.path());
    const View via(tt.t.path());
    via.flatten(tt.t.path(), index);
    REQUIRE(row_rel_set(walk.m()) == row_rel_set(via.m()));

    // Every filter the popup can produce from these types, one hidden type at a time, then
    // all hidden but one.
    std::vector<search::TypeFilter> filters;
    search::TypeFilter none;
    filters.push_back(none);
    search::TypeFilter f;
    f.hide_folders = true;
    filters.push_back(f);
    // hide_no_extension alone is covered by the BUG-P5-1 regression case below.
    for (const char* ext : {"jpg", "png", "txt", "lnk"}) {
        f = {};
        f.hidden_extensions = {ext};
        filters.push_back(f);
    }
    f = {};
    f.hide_folders = true;
    f.hide_no_extension = true;
    f.hidden_extensions = {"jpg", "lnk", "png"};
    filters.push_back(f); // only .txt left
    for (search::TypeFilter tf : filters) {
        search::normalize(tf);
        INFO("hide_folders " << tf.hide_folders << " hide_no_extension " << tf.hide_no_extension
                             << " extensions " << tf.hidden_extensions.size()
                             << (tf.hidden_extensions.empty() ? "" : " first ")
                             << (tf.hidden_extensions.empty() ? "" : tf.hidden_extensions[0]));
        walk.m().set_type_filter(tf);
        via.m().set_type_filter(tf);
        REQUIRE(walk.m().settle_search(kSearchWait));
        REQUIRE(via.m().settle_search(kSearchWait));
        CHECK(row_rel_set(walk.m()) == row_rel_set(via.m()));
    }
    // linkdir is a Folder in both (search.hpp "Type visibility filter").
    walk.m().set_type_filter({});
    via.m().set_type_filter({});
    REQUIRE(walk.m().settle_search(kSearchWait));
    REQUIRE(via.m().settle_search(kSearchWait));
    CHECK(summary_of(walk.m()) == summary_of(via.m()));
    CHECK(summary_of(walk.m())["<folder>"] == 3); // pics, pics/raw.d, linkdir
    index->stop();
}

TEST_CASE("Walk- and index-backed views agree on a link to a folder hidden by its type",
          "[flatten][types][index][e2e]") {
    // BUG-P5-1 (see docs/qa-phase5.md): the index scans with metadata = false, so a symlink to
    // a folder is never directory-like in the index (kEntryLinkToDir stays clear); its query
    // applies the type filter with "NoExtension" for "linkdir" and drops it when "No extension"
    // is hidden, while the walk-backed view (metadata = true) keeps it as a Folder.
    const TypeTree tt;
    const std::shared_ptr<search::IIndex> index = ready_index(index_options(tt.t.path()));
    search::TypeFilter tf;
    tf.hide_no_extension = true;
    search::IndexQuery q;
    q.types = tf;
    q.limit = 0;
    const std::vector<std::string> rels = query_rels(*index, q);
    CHECK(std::find(rels.begin(), rels.end(), "linkdir") != rels.end());
    // What the user sees: the walk-backed view keeps linkdir, the index-backed one drops it.
    const View walk(tt.t.path());
    walk.flatten(tt.t.path());
    const View via(tt.t.path());
    via.flatten(tt.t.path(), index);
    walk.m().set_type_filter(tf);
    via.m().set_type_filter(tf);
    REQUIRE(walk.m().settle_search(kSearchWait));
    REQUIRE(via.m().settle_search(kSearchWait));
    CHECK(walk.m().find_row("linkdir") != ui::kNoRow);
    CHECK(row_rel_set(walk.m()) == row_rel_set(via.m()));
    Result<search::IndexResult> r = index->query(search::IndexQuery{}, {});
    REQUIRE(r.has_value());
    for (const search::IndexHit& hit : r.value().hits) {
        if (hit.path.filename() == "linkdir") {
            CHECK(hit.directory_like);
        }
    }
    index->stop();
}

namespace {

// The real walker, slowed down and in small batches, so a cancel lands mid-walk.
ui::WalkFactory slow_walk(std::chrono::microseconds per_batch, std::size_t batch) {
    return [per_batch,
            batch](const fs::WalkOptions& options) -> Result<std::unique_ptr<ui::WalkSource>> {
        class Slow final : public ui::WalkSource {
        public:
            Slow(fs::Walker w, std::chrono::microseconds d) : w_(std::move(w)), d_(d) {}
            Result<bool> next_batch(fs::WalkBatch& out, std::stop_token stop) override {
                std::this_thread::sleep_for(d_);
                return w_.next_batch(out, std::move(stop));
            }
            [[nodiscard]] const fs::WalkStats& stats() const noexcept override {
                return w_.stats();
            }

        private:
            fs::Walker w_;
            std::chrono::microseconds d_;
        };
        fs::WalkOptions o = options;
        o.batch_size = batch;
        Result<fs::Walker> w = fs::Walker::open(o);
        if (!w) {
            return fail(w.error().code, w.error().message);
        }
        return std::unique_ptr<ui::WalkSource>(std::make_unique<Slow>(std::move(*w), per_batch));
    };
}

// depth 1: d0..d9, f0..f9; depth 2: dK/e0..e19 and dK/sK0..sK4 folders; depth 3: 10 files each.
struct WideTree {
    TempDir t;
    std::map<std::string, std::uint32_t> depth; // rel -> depth
    WideTree() {
        for (int k = 0; k < 10; ++k) {
            const std::string d = "d" + std::to_string(k);
            add(d, true);
            add("f" + std::to_string(k) + ".txt", false);
            for (int e = 0; e < 20; ++e) {
                add(d + "/e" + std::to_string(e) + ".md", false);
            }
            for (int s = 0; s < 5; ++s) {
                const std::string sd = d + "/s" + std::to_string(k) + std::to_string(s);
                add(sd, true);
                for (int x = 0; x < 10; ++x) {
                    add(sd + "/x" + std::to_string(x) + ".txt", false);
                }
            }
        }
    }
    void add(const std::string& rel, bool dir) {
        if (dir) {
            std::filesystem::create_directories(t / rel);
        } else {
            REQUIRE(touch(t / rel));
        }
        depth[rel] = static_cast<std::uint32_t>(std::count(rel.begin(), rel.end(), '/') + 1);
    }
};

// Breadth-first completeness of a partial flatten: if any shown row has depth D, every entry
// of the full tree with depth < D that matches the query is shown too.
std::string bfs_problem(const ui::BrowserModel& m, const WideTree& w) {
    const Rels rows = row_rel_set(m);
    std::uint32_t deepest = 0;
    for (const std::string& rel : rows) {
        const auto it = w.depth.find(rel);
        if (it == w.depth.end()) {
            return "unknown row " + rel;
        }
        deepest = std::max(deepest, it->second);
    }
    search::Matcher matcher(m.query());
    for (const auto& [rel, d] : w.depth) {
        const std::string name = rel.substr(rel.rfind('/') + 1);
        if (d < deepest && matcher.matches(name) && !rows.contains(rel)) {
            return "missing shallower match " + rel;
        }
    }
    return {};
}

} // namespace

TEST_CASE("Cancelling a flatten walk mid-way keeps a consistent partial result",
          "[flatten][cancel][e2e]") {
    const WideTree w;
    const std::size_t total = w.depth.size(); // 20 + 250 + 500 = 770
    REQUIRE(total == 770);
    for (const std::size_t cancel_after :
         {std::size_t{1}, std::size_t{30}, std::size_t{300}, std::size_t{600}}) {
        for (const char* q : {"", "x", "s2"}) {
            INFO("cancel after " << cancel_after << " rows found, query '" << q << "'");
            ui::BrowserModel::Options o;
            o.walk_factory = slow_walk(std::chrono::microseconds(1500), 8);
            const View v(w.t.path(), std::move(o));
            v.m().set_filter_text(q);
            v.m().flatten(ui::FlattenRequest{});
            REQUIRE(pump(v.m(), [&] {
                return v.m().flatten_progress().found >= cancel_after ||
                       !v.m().flatten_progress().walking;
            }));
            CHECK(row_problem(v.m()).empty());
            const auto before = std::chrono::steady_clock::now();
            v.m().cancel_search();
            CHECK(std::chrono::steady_clock::now() - before < std::chrono::milliseconds(50));
            const ui::FlattenProgress p = v.m().flatten_progress();
            CHECK_FALSE(p.walking);
            CHECK(p.found < total);
            CHECK(p.cancelled);
            REQUIRE(v.m().settle_search(kSearchWait));
            CHECK(ui::FlattenWalk::wait_for_exit(kSearchWait));
            const std::vector<std::string> frozen = row_rels(v.m());
            CHECK(row_problem(v.m()).empty());
            CHECK(bfs_problem(v.m(), w).empty());
            if (std::string_view(q).empty()) {
                CHECK(frozen.size() == v.m().flatten_progress().found);
            }
            // Nothing arrives after the cancel.
            const auto quiet = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
            (void)pump(v.m(), [&] { return std::chrono::steady_clock::now() >= quiet; });
            CHECK(row_rels(v.m()) == frozen);
            // A query edit after the cancel re-matches the rows found so far, no new walk.
            v.query("e1");
            CHECK(row_problem(v.m()).empty());
            CHECK_FALSE(v.m().flatten_progress().walking);
        }
    }
}

TEST_CASE("Typing during a walk never shows rows of a stale query", "[flatten][cancel][e2e]") {
    const WideTree w;
    ui::BrowserModel::Options o;
    o.walk_factory = slow_walk(std::chrono::microseconds(800), 16);
    o.ui_filter_limit = 64; // exercise the worker path for re-matching
    const View v(w.t.path(), std::move(o));
    v.m().flatten(ui::FlattenRequest{});
    std::string typed;
    std::size_t problems = 0;
    std::string first;
    for (const char ch : std::string("x1.tx")) {
        typed.push_back(ch);
        v.m().set_filter_text(typed);
        for (int i = 0; i < 20; ++i) {
            (void)v.m().frame_start();
            if (const std::string pr = row_problem(v.m()); !pr.empty()) {
                ++problems;
                first = first.empty() ? pr : first;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    REQUIRE(v.m().settle_search(kSearchWait));
    INFO(first);
    CHECK(problems == 0);
    CHECK(row_names(v.m()) == std::vector<std::string>(50, "x1.txt")); // one per sKN folder
}

TEST_CASE("Index queries cancel cleanly: an error or a complete result, never a partial one",
          "[index][cancel][e2e]") {
    TempDir t;
    // ~20k entries: 100 folders x 200 files.
    for (int d = 0; d < 100; ++d) {
        const Path dir = t / ("dir" + std::to_string(d));
        std::filesystem::create_directory(dir);
        for (int f = 0; f < 200; ++f) {
            REQUIRE(touch(dir / ("file_" + std::to_string(d) + "_" + std::to_string(f) + ".txt")));
        }
    }
    const std::shared_ptr<search::IIndex> index = ready_index(index_options(t.path()));
    search::IndexQuery q;
    q.query = search::compile_query("fl1", search::MatchMode::Fuzzy);
    q.limit = 0;
    const std::vector<std::string> full = query_rels(*index, q);
    REQUIRE_FALSE(full.empty());
    {
        std::stop_source stopped;
        stopped.request_stop();
        Result<search::IndexResult> r = index->query(q, stopped.get_token());
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().code == ErrorCode::Cancelled);
    }
    std::mt19937 rng(5);
    int cancelled = 0;
    int completed = 0;
    for (int trial = 0; trial < 40; ++trial) {
        std::stop_source source;
        const auto delay = std::chrono::microseconds(rng() % 4000);
        std::jthread canceller([&source, delay] {
            std::this_thread::sleep_for(delay);
            source.request_stop();
        });
        Result<search::IndexResult> r = index->query(q, source.get_token());
        canceller.join();
        if (!r) {
            CHECK(r.error().code == ErrorCode::Cancelled);
            ++cancelled;
            continue;
        }
        ++completed;
        std::vector<std::string> rels;
        for (const search::IndexHit& hit : r.value().hits) {
            rels.push_back(hit.path.lexically_relative(t.path()).string());
        }
        CHECK(rels == full);
        CHECK(r.value().matched == full.size());
    }
    CHECK(cancelled + completed == 40);

    // Index-backed view under rapid typing (each query cancels the previous) and an explicit
    // cancel: rows always match the query they reflect; the last query wins.
    ui::BrowserModel::Options o;
    o.index_debounce = std::chrono::milliseconds(5);
    o.index_limit = 0;
    const View v(t.path(), std::move(o));
    v.flatten(t.path(), index);
    std::size_t problems = 0;
    std::string typed;
    for (const char ch : std::string("file_9_19")) {
        typed.push_back(ch);
        v.m().set_filter_text(typed);
        for (int i = 0; i < 3; ++i) {
            (void)v.m().frame_start();
            problems += row_problem(v.m()).empty() ? 0U : 1U;
        }
    }
    REQUIRE(v.m().settle_search(kSearchWait));
    CHECK(problems == 0);
    CHECK(row_rels(v.m()).front() == "dir9/file_9_19.txt");
    v.m().set_filter_text("file_1");
    v.m().cancel_search();
    REQUIRE(v.m().settle_search(kSearchWait));
    CHECK(row_problem(v.m()).empty());
    index->stop();
}
