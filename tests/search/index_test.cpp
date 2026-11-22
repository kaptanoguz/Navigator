// Name index behaviour on generated trees (navigator/index.hpp): cold scan vs brute force,
// persistence, corruption, restart verify, resume, live updates (fake and real inotify), watch
// budget, and queries concurrent with updates.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <navigator/index.hpp>
#include <navigator/search.hpp>
#include <navigator/watch.hpp>

#include "index_test_util.hpp"
#include "index_tree.hpp"

using namespace navigator;
using namespace navigator::search;
using namespace navigator::test;
using namespace std::chrono_literals;
namespace stdfs = std::filesystem;
namespace idx = navigator::search::detail;

namespace {

NameIndexOptions options_for(const TempDir& tree, const Path& file = {}) {
    NameIndexOptions o;
    o.root = tree.path();
    o.identity = test_identity(tree.path());
    o.file = file;
    o.rescan_interval = 0s;
    o.save_interval = 3600s;
    o.background_priority = false;
    return o;
}

std::unique_ptr<IIndex> make_index(NameIndexOptions o) {
    auto r = create_name_index(std::move(o));
    REQUIRE(r);
    return std::move(*r);
}

void check_against_reference(const IIndex& index, const stdfs::path& root,
                             const std::vector<std::string>& queries) {
    const std::vector<RefEntry> ref = reference_entries(root);
    for (const std::string& q : queries) {
        for (const MatchMode mode : {MatchMode::Substring, MatchMode::Fuzzy}) {
            for (const bool hidden : {false, true}) {
                INFO("query '" << q << "' mode " << to_string(mode) << " hidden " << hidden);
                const IndexResult got = run_query(index, q, mode, hidden);
                const std::vector<RefHit> want =
                    reference_query(root, ref, compile_query(q, mode), hidden);
                CHECK(got.matched == want.size());
                CHECK(same_hits(hits_of(got), want));
            }
        }
    }
}

const std::vector<std::string> kQueries = {"",        "rep", "ist",  "e\xcc\x81", "d1", "txt",
                                           "foto 20", "çi",  "zzzz", "_d",        "a b"};

} // namespace

TEST_CASE("create_name_index validates options", "[search][index]") {
    NameIndexOptions o;
    CHECK_FALSE(create_name_index(o));
    o.root = "relative/path";
    CHECK_FALSE(create_name_index(o));
    o.root = "/tmp";
    o.max_entries = 0;
    const auto r = create_name_index(o);
    REQUIRE_FALSE(r);
    CHECK(r.error().code == ErrorCode::InvalidArgument);
}

TEST_CASE("idle index answers empty incomplete results", "[search][index]") {
    TempDir tree;
    auto index = make_index(options_for(tree));
    CHECK(index->status().state == IndexState::Idle);
    CHECK(index->root() == tree.path());
    const IndexResult r = run_query(*index, "x");
    CHECK(r.hits.empty());
    CHECK_FALSE(r.complete);
    index->request_rescan(RescanKind::Verify); // no-op before start
    index->stop();                             // no-op before start
}

TEST_CASE("cold scan of a generated tree equals brute force", "[search][index]") {
    TempDir tree;
    generate_tree(tree.path(), TreeSpec{7, 80, 6, 5, 0.1});
    auto index = make_index(options_for(tree));
    std::atomic<int> notified{0};
    {
        NameIndexOptions o = options_for(tree);
        o.on_status_change = [&] { ++notified; };
        index = make_index(std::move(o));
    }
    index->start();
    index->start(); // no-op
    REQUIRE(wait_ready(*index));
    CHECK(notified.load() > 0);
    const IndexStatus st = index->status();
    CHECK_FALSE(st.loaded_from_file);
    CHECK(st.entries == reference_entries(tree.path()).size());
    CHECK(st.directories >= 81);
    CHECK(st.dirs_pending == 0);
    CHECK_FALSE(st.live); // no watcher factory
    CHECK(st.dirs_unwatched == st.directories);
    CHECK(st.last_complete_scan != Timestamp{});
    check_against_reference(*index, tree.path(), kQueries);

    SECTION("results carry kind, depth and highlights") {
        const IndexResult r = run_query(*index, "_d", MatchMode::Substring, true);
        REQUIRE_FALSE(r.hits.empty());
        CHECK(r.complete);
        for (const IndexHit& h : r.hits) {
            CHECK(h.kind == fs::EntryKind::Directory);
            CHECK(h.directory_like);
            CHECK(h.depth >= 1);
            REQUIRE_FALSE(h.match.highlights.empty());
            const std::string name = h.path.filename().native();
            const HighlightRange hr = h.match.highlights.front();
            CHECK(name.substr(hr.begin, hr.end - hr.begin) == "_d");
        }
    }
    SECTION("limit keeps the best hits") {
        IndexQuery q;
        q.query = compile_query("e", MatchMode::Fuzzy);
        q.show_hidden = true;
        q.limit = 0;
        const auto all = index->query(q, {});
        REQUIRE(all);
        q.limit = 5;
        const auto top = index->query(q, {});
        REQUIRE(top);
        REQUIRE(top->hits.size() == 5);
        CHECK(top->matched == all->matched);
        for (std::size_t i = 0; i < 5; ++i) {
            CHECK(top->hits[i].path == all->hits[i].path);
        }
    }
    SECTION("under restricts to a subtree with depths relative to it") {
        const std::vector<RefEntry> ref = reference_entries(tree.path());
        const RefEntry* sub = nullptr;
        for (const RefEntry& e : ref) {
            if (e.directory && e.depth == 1 && !e.hidden_chain) {
                sub = &e;
                break;
            }
        }
        REQUIRE(sub != nullptr);
        const stdfs::path under = tree.path() / sub->name;
        const IndexResult r = run_query(*index, "", MatchMode::Fuzzy, true, under);
        const std::vector<RefHit> want = reference_query(under, reference_entries(under),
                                                         compile_query("", MatchMode::Fuzzy), true);
        CHECK(same_hits(hits_of(r), want));
        // Trailing slash and "." are fine; "..", relative and outside paths are not.
        CHECK(index->query(
            IndexQuery{
                compile_query("", MatchMode::Fuzzy), Path(under.native() + "/."), {}, true, 0},
            {}));
        for (const Path& bad :
             {Path("relative"), Path("/definitely/not/below"), Path(tree.path() / ".." / "x")}) {
            IndexQuery q;
            q.under = bad;
            const auto res = index->query(q, {});
            REQUIRE_FALSE(res);
            CHECK(res.error().code == ErrorCode::InvalidArgument);
        }
        // Below the root but not indexed: empty.
        CHECK(run_query(*index, "", MatchMode::Fuzzy, true, tree.path() / "no-such-dir")
                  .hits.empty());
    }
    SECTION("type filter applies") {
        TypeFilter types;
        set_visible(types, TypeKey{TypeKind::Folder, {}}, false);
        set_visible(types, TypeKey{TypeKind::Extension, "txt"}, false);
        const IndexResult r = run_query(*index, "", MatchMode::Fuzzy, false, {}, types);
        const std::vector<RefHit> want =
            reference_query(tree.path(), reference_entries(tree.path()),
                            compile_query("", MatchMode::Fuzzy), false, types);
        CHECK(same_hits(hits_of(r), want));
    }
    SECTION("queries are cancellable") {
        std::stop_source src;
        src.request_stop();
        IndexQuery q;
        const auto r = index->query(q, src.get_token());
        REQUIRE_FALSE(r);
        CHECK(r.error().code == ErrorCode::Cancelled);
    }
    index->stop();
    CHECK(index->status().state == IndexState::Stopped);
    index->stop(); // idempotent
}

TEST_CASE("max_entries truncates", "[search][index]") {
    TempDir tree;
    generate_tree(tree.path(), TreeSpec{3, 20, 8, 3, 0.0});
    NameIndexOptions o = options_for(tree);
    o.max_entries = 25;
    auto index = make_index(std::move(o));
    index->start();
    REQUIRE(wait_ready(*index));
    const IndexStatus st = index->status();
    CHECK(st.truncated);
    CHECK(st.entries <= 25);
    CHECK(run_query(*index, "", MatchMode::Fuzzy, true).hits.size() == st.entries);
}

TEST_CASE("persistence round trip", "[search][index][file]") {
    TempDir tree;
    TempDir cache;
    generate_tree(tree.path(), TreeSpec{11, 50, 5, 4, 0.1});
    const Path file = index_file(cache.path(), test_identity(tree.path()));
    {
        auto index = make_index(options_for(tree, file));
        index->start();
        REQUIRE(wait_ready(*index));
        index->stop();
    }
    REQUIRE(stdfs::exists(file));
    struct stat st {};
    REQUIRE(::stat(file.c_str(), &st) == 0);
    CHECK((st.st_mode & 0777) == 0600);
    REQUIRE(::stat(file.parent_path().c_str(), &st) == 0);
    CHECK((st.st_mode & 0777) == 0700);
    // No temporary files left behind.
    std::size_t files = 0;
    for ([[maybe_unused]] const auto& e : stdfs::directory_iterator(file.parent_path())) {
        ++files;
    }
    CHECK(files == 1);

    auto index = make_index(options_for(tree, file));
    index->start();
    REQUIRE(wait_ready(*index));
    const IndexStatus s = index->status();
    CHECK(s.loaded_from_file);
    CHECK_FALSE(s.last_error.has_value());
    check_against_reference(*index, tree.path(), kQueries);
}

TEST_CASE("corrupt index files are deleted and rebuilt", "[search][index][file]") {
    TempDir tree;
    TempDir cache;
    generate_tree(tree.path(), TreeSpec{5, 30, 5, 4, 0.1});
    const Path file = index_file(cache.path(), test_identity(tree.path()));
    {
        auto index = make_index(options_for(tree, file));
        index->start();
        REQUIRE(wait_ready(*index));
    } // destructor stops and saves
    REQUIRE(stdfs::exists(file));
    std::string good;
    {
        std::ifstream in(file, std::ios::binary);
        good.assign(std::istreambuf_iterator<char>(in), {});
    }
    REQUIRE(good.size() > 200);

    const auto write = [&](const std::string& bytes) {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << bytes;
    };
    std::vector<std::pair<std::string, std::string>> cases;
    cases.emplace_back("truncated", good.substr(0, good.size() / 2));
    cases.emplace_back("empty", std::string());
    {
        std::string bad = good;
        bad[good.size() / 2] = static_cast<char>(bad[good.size() / 2] ^ 0x5A);
        cases.emplace_back("bad checksum", bad);
    }
    {
        std::string bad = good;
        bad[8] = 2; // version 2
        cases.emplace_back("wrong version", bad);
    }
    {
        std::string bad = good;
        bad[0] = 'X';
        cases.emplace_back("bad magic", bad);
    }
    for (const auto& [label, bytes] : cases) {
        INFO(label);
        write(bytes);
        auto index = make_index(options_for(tree, file));
        index->start();
        REQUIRE(wait_ready(*index));
        const IndexStatus st = index->status();
        CHECK_FALSE(st.loaded_from_file);
        CHECK(st.last_error.has_value());
        check_against_reference(*index, tree.path(), {"rep", ""});
        index->stop();
        // Rebuilt and saved again: the next run loads it.
        auto again = make_index(options_for(tree, file));
        again->start();
        REQUIRE(wait_ready(*again));
        CHECK(again->status().loaded_from_file);
    }

    // An index of another volume (same file name, different identity) is not used.
    {
        NameIndexOptions o = options_for(tree, file);
        o.identity.device = "/dev/other";
        auto index = make_index(std::move(o));
        index->start();
        REQUIRE(wait_ready(*index));
        CHECK_FALSE(index->status().loaded_from_file);
        REQUIRE(index->status().last_error.has_value());
    }
}

TEST_CASE("restart verify picks up changes made while stopped", "[search][index][file]") {
    TempDir tree;
    TempDir cache;
    const std::vector<std::string> dirs = generate_tree(tree.path(), TreeSpec{21, 40, 4, 4, 0.0});
    age_directories(tree.path(), 3600s);
    const Path file = index_file(cache.path(), test_identity(tree.path()));
    {
        auto index = make_index(options_for(tree, file));
        index->start();
        REQUIRE(wait_ready(*index));
    }
    // Changes while stopped.
    const stdfs::path a = tree.path() / dirs[3];
    const stdfs::path b = tree.path() / dirs[7];
    touch(a / "added_while_stopped.txt");
    stdfs::create_directory(b / "newdir_while_stopped");
    touch(b / "newdir_while_stopped" / "inner_while_stopped.md");
    stdfs::path victim;
    for (const auto& e : stdfs::directory_iterator(tree.path() / dirs[5])) {
        if (e.is_regular_file()) {
            victim = e.path();
            break;
        }
    }
    if (!victim.empty()) {
        stdfs::remove(victim);
    }
    // A change hidden from mtime-based verification: the directory's mtime is restored, so the
    // restart must NOT re-list it (proof that verify is one stat per directory).
    const stdfs::path c = tree.path() / dirs[9];
    struct stat before {};
    REQUIRE(::stat(c.c_str(), &before) == 0);
    touch(c / "sneaky_invisible.txt");
    const struct timespec times[2] = {before.st_atim, before.st_mtim};
    REQUIRE(::utimensat(AT_FDCWD, c.c_str(), times, AT_SYMLINK_NOFOLLOW) == 0);

    auto index = make_index(options_for(tree, file));
    index->start();
    REQUIRE(wait_ready(*index));
    CHECK(index->status().loaded_from_file);
    CHECK(index_has(*index, a / "added_while_stopped.txt"));
    CHECK(index_has(*index, b / "newdir_while_stopped" / "inner_while_stopped.md"));
    if (!victim.empty()) {
        CHECK_FALSE(index_has(*index, victim));
    }
    CHECK_FALSE(index_has(*index, c / "sneaky_invisible.txt"));
    // A rebuild re-walks everything and finds it.
    const Timestamp last = index->status().last_complete_scan;
    index->request_rescan(RescanKind::Rebuild);
    REQUIRE(wait_until([&] {
        const IndexStatus st = index->status();
        return st.state == IndexState::Ready && st.last_complete_scan != last;
    }));
    CHECK(index_has(*index, c / "sneaky_invisible.txt"));
    check_against_reference(*index, tree.path(), {"", "while"});
}

TEST_CASE("a stopped scan resumes from the partial index", "[search][index][file]") {
    TempDir tree;
    TempDir cache;
    generate_tree(tree.path(), TreeSpec{33, 1500, 3, 8, 0.05});
    age_directories(tree.path(), 3600s);
    const Path file = index_file(cache.path(), test_identity(tree.path()));
    std::atomic<bool> reached{false};
    {
        NameIndexOptions o = options_for(tree, file);
        IIndex* self = nullptr;
        o.on_status_change = [&] {
            if (self != nullptr && !reached.load()) {
                const IndexStatus st = self->status();
                if (st.state == IndexState::Scanning && st.entries > 1000) {
                    reached = true;
                    // Hold the service thread so stop() lands mid-scan.
                    std::this_thread::sleep_for(200ms);
                }
            }
        };
        auto index = make_index(std::move(o));
        self = index.get();
        index->start();
        REQUIRE(wait_until([&] { return reached.load(); }));
        index->stop();
    }
    REQUIRE(stdfs::exists(file));
    std::string bytes;
    {
        std::ifstream in(file, std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(in), {});
    }
    auto partial = idx::decode_index(bytes, test_identity(tree.path()));
    REQUIRE(partial);
    CHECK_FALSE(partial->complete);
    std::size_t scanned = 0;
    std::size_t unscanned = 0;
    for (const idx::Dir& d : partial->dirs) {
        ((d.flags & idx::kDirScanned) != 0 ? scanned : unscanned) += 1;
    }
    CHECK(scanned > 0);
    CHECK(unscanned > 0);

    auto index = make_index(options_for(tree, file));
    index->start();
    REQUIRE(wait_until([&] {
        return index->status().state == IndexState::Scanning ||
               index->status().state == IndexState::Ready;
    }));
    REQUIRE(wait_ready(*index));
    CHECK(index->status().loaded_from_file);
    check_against_reference(*index, tree.path(), {"", "rep", "_d1"});
}

TEST_CASE("live updates through a fake watcher", "[search][index][watch]") {
    TempDir tree;
    const std::vector<std::string> dirs = generate_tree(tree.path(), TreeSpec{9, 20, 3, 3, 0.0});
    auto hub = std::make_shared<FakeHub>();
    NameIndexOptions o = options_for(tree);
    o.watcher_factory = fake_factory(hub);
    auto index = make_index(std::move(o));
    index->start();
    REQUIRE(wait_ready(*index));
    CHECK(index->status().live);
    CHECK(index->status().dirs_unwatched == 0);
    const stdfs::path sub = tree.path() / dirs[2];
    REQUIRE(hub->watched(sub));
    const std::uint64_t gen0 = index->status().generation;

    // Created
    touch(sub / "fresh_file.txt");
    REQUIRE(hub->emit(sub, WatchEventKind::Created, "fresh_file.txt"));
    REQUIRE(wait_until([&] { return index_has(*index, sub / "fresh_file.txt"); }));
    CHECK(index->status().generation > gen0);
    // Renamed
    stdfs::rename(sub / "fresh_file.txt", sub / "renamed_file.md");
    REQUIRE(hub->emit(sub, WatchEventKind::Renamed, "renamed_file.md", "fresh_file.txt"));
    REQUIRE(wait_until([&] { return index_has(*index, sub / "renamed_file.md"); }));
    CHECK_FALSE(index_has(*index, sub / "fresh_file.txt"));
    // Deleted
    stdfs::remove(sub / "renamed_file.md");
    REQUIRE(hub->emit(sub, WatchEventKind::Deleted, "renamed_file.md"));
    REQUIRE(wait_until([&] { return !index_has(*index, sub / "renamed_file.md"); }));
    // A new directory with content is listed and watched.
    stdfs::create_directory(sub / "newdir_live");
    touch(sub / "newdir_live" / "deep_live.txt");
    REQUIRE(hub->emit(sub, WatchEventKind::Created, "newdir_live"));
    REQUIRE(wait_until([&] { return index_has(*index, sub / "newdir_live" / "deep_live.txt"); }));
    REQUIRE(wait_until([&] { return hub->watched(sub / "newdir_live"); }));
    // MovedFrom of a directory drops its subtree (and its watch).
    TempDir elsewhere;
    stdfs::rename(sub / "newdir_live", elsewhere.path() / "moved_out_of_tree_live");
    REQUIRE(hub->emit(sub, WatchEventKind::MovedFrom, "newdir_live"));
    REQUIRE(wait_until([&] { return !index_has(*index, sub / "newdir_live"); }));
    CHECK_FALSE(index_has(*index, sub / "newdir_live" / "deep_live.txt"));
    REQUIRE(wait_until([&] { return !hub->watched(sub / "newdir_live"); }));
    // Renaming a directory keeps its subtree under the new name.
    const stdfs::path dir_a = sub / "dir_before";
    stdfs::create_directory(dir_a);
    touch(dir_a / "child_of_renamed.txt");
    REQUIRE(hub->emit(sub, WatchEventKind::Created, "dir_before"));
    REQUIRE(wait_until([&] { return index_has(*index, dir_a / "child_of_renamed.txt"); }));
    stdfs::rename(dir_a, sub / "dir_after");
    REQUIRE(hub->emit(sub, WatchEventKind::Renamed, "dir_after", "dir_before"));
    REQUIRE(
        wait_until([&] { return index_has(*index, sub / "dir_after" / "child_of_renamed.txt"); }));
    CHECK_FALSE(index_has(*index, dir_a / "child_of_renamed.txt"));
    // The backend reports the moved directory's own watch as gone: the directory is still in
    // the tree (same inode under the new name) and stays indexed.
    const bool gone_sent = hub->emit(dir_a, WatchEventKind::WatchedDirGone, "");
    if (gone_sent) {
        std::this_thread::sleep_for(50ms);
        CHECK(index_has(*index, sub / "dir_after" / "child_of_renamed.txt"));
    }
    // Overflow: changes without events are found by the verify pass it schedules.
    touch(sub / "after_overflow.txt");
    stdfs::remove(sub / "dir_after" / "child_of_renamed.txt");
    hub->overflow();
    REQUIRE(wait_until([&] { return index_has(*index, sub / "after_overflow.txt"); }));
    REQUIRE(
        wait_until([&] { return !index_has(*index, sub / "dir_after" / "child_of_renamed.txt"); }));
    REQUIRE(wait_ready(*index));
    check_against_reference(*index, tree.path(), {"", "live", "over"});
    // WatchedDirGone for a directory that really vanished removes it.
    const stdfs::path doomed = tree.path() / dirs[4];
    stdfs::remove_all(doomed);
    REQUIRE(hub->emit(doomed, WatchEventKind::WatchedDirGone, ""));
    REQUIRE(wait_until([&] { return !index_has(*index, doomed); }));
}

TEST_CASE("watch budget: unwatched directories are covered by the periodic pass",
          "[search][index][watch]") {
    TempDir tree;
    const std::vector<std::string> dirs = generate_tree(tree.path(), TreeSpec{4, 15, 2, 4, 0.0});
    auto hub = std::make_shared<FakeHub>();
    NameIndexOptions o = options_for(tree);
    o.watcher_factory = fake_factory(hub);
    o.watch_budget = 3;
    o.rescan_interval = 1s;
    auto index = make_index(std::move(o));
    index->start();
    REQUIRE(wait_ready(*index));
    const IndexStatus st = index->status();
    CHECK(hub->watch_count() == 3);
    CHECK(st.dirs_unwatched == st.directories - 3);
    CHECK_FALSE(st.watch_budget_exhausted);
    CHECK(hub->watched(tree.path())); // breadth-first: the root comes first
    // Find an unwatched directory and change it without any event.
    stdfs::path quiet;
    for (const std::string& d : dirs) {
        if (!d.empty() && !hub->watched(tree.path() / d)) {
            quiet = tree.path() / d;
            break;
        }
    }
    REQUIRE_FALSE(quiet.empty());
    touch(quiet / "found_by_periodic_pass.txt");
    REQUIRE(
        wait_until([&] { return index_has(*index, quiet / "found_by_periodic_pass.txt"); }, 10s));
}

TEST_CASE("watch limit errors stop adding watches", "[search][index][watch]") {
    TempDir tree;
    generate_tree(tree.path(), TreeSpec{4, 15, 2, 4, 0.0});
    auto hub = std::make_shared<FakeHub>();
    hub->fail_after = 2;
    NameIndexOptions o = options_for(tree);
    o.watcher_factory = fake_factory(hub);
    auto index = make_index(std::move(o));
    index->start();
    REQUIRE(wait_ready(*index));
    const IndexStatus st = index->status();
    CHECK(st.watch_budget_exhausted);
    CHECK(hub->watch_count() == 2);
    CHECK(st.dirs_unwatched == st.directories - 2);
    REQUIRE(st.last_error.has_value());
    CHECK(st.last_error.value_or(Error{}).code == ErrorCode::PlatformError);
}

TEST_CASE("root that disappears fails and then recovers", "[search][index]") {
    TempDir base;
    const stdfs::path root = base.path() / "vol";
    stdfs::create_directory(root);
    touch(root / "one.txt");
    NameIndexOptions o;
    o.root = root;
    o.identity = test_identity(root);
    o.rescan_interval = 1s;
    o.background_priority = false;
    auto index = make_index(std::move(o));
    stdfs::remove_all(root);
    index->start();
    REQUIRE(wait_until([&] { return index->status().state == IndexState::Failed; }));
    CHECK(index->status().last_error.has_value());
    stdfs::create_directory(root);
    touch(root / "two.txt");
    index->request_rescan(RescanKind::Verify);
    REQUIRE(wait_ready(*index));
    CHECK(index_has(*index, root / "two.txt"));
}

TEST_CASE("live updates through the real inotify watcher", "[search][index][inotify]") {
    TempDir tree;
    const std::vector<std::string> dirs = generate_tree(tree.path(), TreeSpec{12, 12, 2, 3, 0.0});
    NameIndexOptions o = options_for(tree);
    o.watcher_factory = [] { return create_inotify_watcher(); };
    auto index = make_index(std::move(o));
    index->start();
    REQUIRE(wait_ready(*index));
    REQUIRE(index->status().live);
    const stdfs::path sub = tree.path() / dirs[1];

    touch(sub / "inotify_new.txt");
    REQUIRE(wait_until([&] { return index_has(*index, sub / "inotify_new.txt"); }));
    stdfs::rename(sub / "inotify_new.txt", sub / "inotify_renamed.txt");
    REQUIRE(wait_until([&] { return index_has(*index, sub / "inotify_renamed.txt"); }));
    CHECK_FALSE(index_has(*index, sub / "inotify_new.txt"));
    stdfs::remove(sub / "inotify_renamed.txt");
    REQUIRE(wait_until([&] { return !index_has(*index, sub / "inotify_renamed.txt"); }));

    stdfs::create_directories(sub / "inotify_dir" / "nested");
    touch(sub / "inotify_dir" / "nested" / "inotify_deep.txt");
    REQUIRE(wait_until(
        [&] { return index_has(*index, sub / "inotify_dir" / "nested" / "inotify_deep.txt"); }));
    // Files created inside the new directory later are seen through its new watch.
    touch(sub / "inotify_dir" / "later.txt");
    REQUIRE(wait_until([&] { return index_has(*index, sub / "inotify_dir" / "later.txt"); }));
    // Rename the directory: the subtree follows.
    stdfs::rename(sub / "inotify_dir", tree.path() / "inotify_moved");
    REQUIRE(wait_until([&] {
        return index_has(*index, tree.path() / "inotify_moved" / "nested" / "inotify_deep.txt");
    }));
    CHECK_FALSE(index_has(*index, sub / "inotify_dir" / "later.txt"));
    touch(tree.path() / "inotify_moved" / "after_move.txt");
    REQUIRE(wait_until(
        [&] { return index_has(*index, tree.path() / "inotify_moved" / "after_move.txt"); }));
    stdfs::remove_all(tree.path() / "inotify_moved");
    REQUIRE(wait_until([&] { return !index_has(*index, tree.path() / "inotify_moved"); }));
    REQUIRE(wait_until(
        [&] { return run_query(*index, "inotify", MatchMode::Substring, true).hits.empty(); }));
    check_against_reference(*index, tree.path(), {"", "rep"});
}

TEST_CASE("queries run concurrently with live updates", "[search][index][concurrency]") {
    TempDir tree;
    const std::vector<std::string> dirs = generate_tree(tree.path(), TreeSpec{17, 30, 4, 3, 0.0});
    NameIndexOptions o = options_for(tree);
    o.watcher_factory = [] { return create_inotify_watcher(); };
    auto index = make_index(std::move(o));
    index->start();
    REQUIRE(wait_ready(*index));

    std::atomic<bool> done{false};
    std::atomic<std::uint64_t> queries{0};
    std::atomic<bool> failed{false};
    std::vector<std::jthread> readers;
    readers.reserve(3);
    for (int t = 0; t < 3; ++t) {
        readers.emplace_back([&, t] {
            const char* texts[] = {"churn", "rep", ""};
            while (!done.load()) {
                IndexQuery q;
                q.query = compile_query(texts[t], MatchMode::Fuzzy);
                q.show_hidden = true;
                q.limit = 50;
                const auto r = index->query(q, {});
                if (!r) {
                    failed = true;
                }
                (void)index->status();
                ++queries;
            }
        });
    }
    for (int i = 0; i < 150; ++i) {
        const stdfs::path d = tree.path() / dirs[static_cast<std::size_t>(i) % dirs.size()];
        touch(d / ("churn_" + std::to_string(i) + ".txt"));
        if (i % 3 == 0) {
            stdfs::remove(d / ("churn_" + std::to_string(i) + ".txt"));
        }
        if (i % 25 == 0) {
            stdfs::create_directory(d / ("churn_dir_" + std::to_string(i)));
        }
    }
    index->request_rescan(RescanKind::Verify);
    std::this_thread::sleep_for(300ms);
    REQUIRE(wait_until([&] {
        const IndexResult r = run_query(*index, "churn_", MatchMode::Substring, true);
        return r.hits.size() == 100 + 6;
    }));
    done = true;
    readers.clear();
    CHECK_FALSE(failed.load());
    CHECK(queries.load() > 0);
    REQUIRE(wait_ready(*index));
    check_against_reference(*index, tree.path(), {"churn", ""});
}
