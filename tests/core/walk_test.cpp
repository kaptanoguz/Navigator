// fs::Walker (navigator/walk.hpp): BFS order, directory table, hidden policy, cycles, skips,
// limits, cancellation, batching, Unicode names. Every tree lives in a TempDir fixture.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <set>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <navigator/fs.hpp>
#include <navigator/walk.hpp>

#include "temp_dir.hpp"

namespace fs = navigator::fs;
using navigator::ErrorCode;
using navigator::test::TempDir;

namespace {

using Clock = std::chrono::steady_clock;

double ms_since(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

struct Collected {
    std::vector<std::string> dirs{std::string{}}; // the consumer's copy of the directory table
    std::vector<fs::WalkEntry> entries;
    std::vector<std::string> paths; // relative path of each entry
    fs::WalkStats stats;
    std::size_t batches = 0;
    std::size_t max_batch = 0;
};

// Applies one batch to `c`, checking every documented table invariant.
void apply(Collected& c, const fs::WalkBatch& b, std::map<std::uint32_t, std::uint32_t>& depth_of) {
    REQUIRE(b.first_dir == c.dirs.size());
    for (const std::string& d : b.new_dirs) {
        c.dirs.push_back(d);
    }
    c.max_batch = std::max(c.max_batch, b.entries.size());
    for (const fs::WalkEntry& e : b.entries) {
        REQUIRE(e.dir < c.dirs.size());
        REQUIRE(depth_of.count(e.dir) == 1);
        CHECK(e.depth == depth_of[e.dir] + 1);
        if (!c.entries.empty()) {
            CHECK(c.entries.back().depth <= e.depth); // breadth-first
        }
        const std::string& parent = c.dirs[e.dir];
        std::string path = parent.empty() ? e.entry.name : parent + "/" + e.entry.name;
        if (e.self_dir != 0) {
            REQUIRE(e.self_dir < c.dirs.size());
            CHECK(c.dirs[e.self_dir] == path);
            depth_of[e.self_dir] = e.depth;
        }
        c.paths.push_back(std::move(path));
        c.entries.push_back(e);
    }
}

Collected walk_all(const fs::WalkOptions& o) {
    auto w = fs::Walker::open(o);
    REQUIRE(w.has_value());
    Collected c;
    std::map<std::uint32_t, std::uint32_t> depth_of{{0U, 0U}};
    fs::WalkBatch b;
    for (;;) {
        auto r = w->next_batch(b, std::stop_token{});
        REQUIRE(r.has_value());
        ++c.batches;
        apply(c, b, depth_of);
        if (!*r) {
            break;
        }
        REQUIRE(c.batches < 10'000'000);
    }
    // Finished walkers keep answering false with an empty batch.
    auto again = w->next_batch(b, std::stop_token{});
    REQUIRE(again.has_value());
    CHECK_FALSE(*again);
    CHECK(b.entries.empty());
    CHECK(b.new_dirs.empty());
    c.stats = w->stats();
    CHECK(c.stats.done);
    CHECK(c.stats.entries_returned == c.entries.size());
    CHECK(c.stats.dirs_pending == 0);
    return c;
}

fs::WalkOptions opts_for(const std::filesystem::path& root) {
    fs::WalkOptions o;
    o.root = root;
    return o;
}

std::string join(const std::string& a, const std::string& b) {
    std::string r;
    r.reserve(a.size() + 1 + b.size());
    r.append(a).append("/").append(b);
    return r;
}

std::set<std::string> as_set(const std::vector<std::string>& v) { return {v.begin(), v.end()}; }

const fs::WalkEntry* find(const Collected& c, const std::string& path) {
    for (std::size_t i = 0; i < c.paths.size(); ++i) {
        if (c.paths[i] == path) {
            return &c.entries[i];
        }
    }
    return nullptr;
}

void make_file(const std::string& p) {
    const int fd = ::open(p.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        throw std::runtime_error("open failed: " + p);
    }
    ::close(fd);
}

void make_dir(const std::string& p) {
    if (::mkdir(p.c_str(), 0755) != 0) {
        throw std::runtime_error("mkdir failed: " + p);
    }
}

// Tree of `depth` levels, `fan` subdirectories and `files` files per directory. Returns every
// relative path created.
void gen_tree(const std::string& abs, const std::string& rel, int depth, int fan, int files,
              std::vector<std::string>& out) {
    for (int f = 0; f < files; ++f) {
        const std::string name = "f" + std::to_string(f) + ".txt";
        make_file(join(abs, name));
        out.push_back(rel.empty() ? name : join(rel, name));
    }
    if (depth == 0) {
        return;
    }
    for (int d = 0; d < fan; ++d) {
        const std::string name = "d" + std::to_string(d);
        const std::string r = rel.empty() ? name : join(rel, name);
        make_dir(join(abs, name));
        out.push_back(r);
        gen_tree(join(abs, name), r, depth - 1, fan, files, out);
    }
}

// `dirs` directories of `files` empty files each (wide, shallow: the 200k/300k trees).
std::size_t gen_wide(const std::filesystem::path& root, int top, int sub, int files) {
    std::size_t n = 0;
    for (int a = 0; a < top; ++a) {
        const std::string da = root.native() + "/t" + std::to_string(a);
        make_dir(da);
        ++n;
        for (int b = 0; b < sub; ++b) {
            const std::string db = da + "/s" + std::to_string(b);
            make_dir(db);
            ++n;
            for (int f = 0; f < files; ++f) {
                make_file(db + "/file-" + std::to_string(f) + ".dat");
                ++n;
            }
        }
    }
    return n;
}

} // namespace

TEST_CASE("walk: generated tree, BFS order, directory table", "[walk]") {
    const TempDir t;
    std::vector<std::string> expected;
    gen_tree(t.path().native(), "", 3, 3, 2, expected); // 3 levels, fan-out 3, 2 files per dir

    for (const bool metadata : {true, false}) {
        INFO("metadata " << metadata);
        auto o = opts_for(t.path());
        o.metadata = metadata;
        o.batch_size = 7; // force many batches
        const Collected c = walk_all(o);
        CHECK(as_set(c.paths) == as_set(expected));
        CHECK(c.paths.size() == expected.size());
        CHECK(c.batches > 1);
        CHECK(c.max_batch <= 7);
        // 1 + 3 + 9 + 27 directories, all entered.
        CHECK(c.stats.dirs_visited == 40);
        CHECK(c.dirs.size() == 40);
        CHECK(c.stats.skipped_cycles == 0);
        CHECK(c.stats.skipped_other_fs == 0);
        CHECK_FALSE(c.stats.truncated);
        for (std::size_t i = 0; i < c.entries.size(); ++i) {
            const fs::WalkEntry& e = c.entries[i];
            const auto depth = static_cast<std::uint32_t>(
                std::count(c.paths[i].begin(), c.paths[i].end(), '/') + 1);
            CHECK(e.depth == depth);
            const bool is_dir = c.paths[i].back() != 't'; // "dN" vs "fN.txt"
            CHECK(e.entry.kind == (is_dir ? fs::EntryKind::Directory : fs::EntryKind::File));
            CHECK((e.self_dir != 0) == is_dir);
            CHECK_FALSE(e.entry.metadata_error.has_value());
            struct stat st {};
            REQUIRE(::lstat((t.path() / c.paths[i]).c_str(), &st) == 0);
            CHECK(e.entry.id.device == static_cast<std::uint64_t>(st.st_dev));
            CHECK(e.entry.id.inode == static_cast<std::uint64_t>(st.st_ino));
            if (metadata) {
                CHECK(e.entry.mode == static_cast<std::uint32_t>(st.st_mode));
                CHECK(e.entry.mtime != navigator::Timestamp{});
                CHECK(e.entry.readable);
            } else {
                CHECK(e.entry.mode == 0);
                CHECK(e.entry.mtime == navigator::Timestamp{});
                CHECK_FALSE(e.entry.readable);
            }
        }
    }
}

TEST_CASE("walk: metadata matches list_directory", "[walk]") {
    const TempDir t;
    t.dir("sub");
    t.file("sub/a.bin", 1234);
    t.link("sub/l", "a.bin");
    t.link("sub/dangling", "missing");
    const Collected c = walk_all(opts_for(t.path()));
    auto listed = fs::list_directory(t / "sub");
    REQUIRE(listed.has_value());
    REQUIRE(listed->size() == 3);
    for (const fs::DirEntry& l : *listed) {
        const fs::WalkEntry* w = find(c, "sub/" + l.name);
        REQUIRE(w != nullptr);
        CHECK(w->entry.kind == l.kind);
        CHECK(w->entry.target_kind == l.target_kind);
        CHECK(w->entry.dangling == l.dangling);
        CHECK(w->entry.size == l.size);
        CHECK(w->entry.mtime == l.mtime);
        CHECK(w->entry.mode == l.mode);
        CHECK(w->entry.id == l.id);
        CHECK(w->entry.readable == l.readable);
    }
}

TEST_CASE("walk: hidden directories are not entered unless show_hidden", "[walk][hidden]") {
    const TempDir t;
    t.dir(".git");
    t.dir(".git/objects");
    t.file(".git/objects/x");
    t.file(".bashrc");
    t.dir("src");
    t.file("src/a.c");
    t.file("src/.hidden_file");
    t.dir("visible");
    t.dir("visible/.cache");
    t.file("visible/.cache/y");

    SECTION("show_hidden false") {
        const Collected c = walk_all(opts_for(t.path()));
        CHECK(as_set(c.paths) == std::set<std::string>{"src", "src/a.c", "visible"});
        CHECK(c.stats.hidden_skipped == 4); // .git, .bashrc, src/.hidden_file, visible/.cache
        CHECK(c.stats.dirs_visited == 3);   // root, src, visible
    }
    SECTION("show_hidden true") {
        auto o = opts_for(t.path());
        o.show_hidden = true;
        const Collected c = walk_all(o);
        CHECK(as_set(c.paths) == std::set<std::string>{".git", ".git/objects", ".git/objects/x",
                                                       ".bashrc", "src", "src/a.c",
                                                       "src/.hidden_file", "visible",
                                                       "visible/.cache", "visible/.cache/y"});
        CHECK(c.stats.hidden_skipped == 0);
        const fs::WalkEntry* git = find(c, ".git");
        REQUIRE(git != nullptr);
        CHECK(git->entry.hidden);
    }
    SECTION("a hidden root is always walked") {
        const Collected c = walk_all(opts_for(t / ".git"));
        CHECK(as_set(c.paths) == std::set<std::string>{"objects", "objects/x"});
    }
}

TEST_CASE("walk: symlinks are not followed by default; followed cycles are blocked",
          "[walk][symlink]") {
    const TempDir t;
    t.dir("a");
    t.dir("a/b");
    t.file("a/b/f");
    t.link("a/up", "..");
    t.link("self", ".");
    t.link("linkb", "a/b");
    t.link("loop1", "loop2");
    t.link("loop2", "loop1");
    t.link("dangling", "nowhere");

    SECTION("follow_symlinks false") {
        const Collected c = walk_all(opts_for(t.path()));
        CHECK(as_set(c.paths) == std::set<std::string>{"a", "a/b", "a/b/f", "a/up", "self", "linkb",
                                                       "loop1", "loop2", "dangling"});
        for (const char* l : {"a/up", "self", "linkb", "loop1", "dangling"}) {
            const fs::WalkEntry* e = find(c, l);
            REQUIRE(e != nullptr);
            CHECK(e->entry.kind == fs::EntryKind::Symlink);
            CHECK(e->self_dir == 0);
        }
        CHECK(c.stats.skipped_cycles == 0);
        CHECK(c.stats.dirs_visited == 3);
    }
    SECTION("follow_symlinks true") {
        for (const bool metadata : {true, false}) {
            INFO("metadata " << metadata);
            auto o = opts_for(t.path());
            o.follow_symlinks = true;
            o.metadata = metadata;
            const Collected c = walk_all(o);
            // a/b is reached twice (as linkb at depth 1 and as a/b at depth 2): entered once.
            const auto fs_count = std::count_if(c.paths.begin(), c.paths.end(), [](const auto& p) {
                return p.size() >= 2 && p.compare(p.size() - 2, 2, "/f") == 0;
            });
            CHECK(fs_count == 1);
            CHECK(find(c, "linkb/f") != nullptr); // BFS: the shallower name wins
            // up -> root, self -> root, a/b already entered via linkb.
            CHECK(c.stats.skipped_cycles == 3);
            CHECK(c.stats.dirs_visited == 3); // root, a, linkb
            const fs::WalkEntry* loop = find(c, "loop1");
            REQUIRE(loop != nullptr);
            CHECK(loop->self_dir == 0);
            CHECK(c.paths.size() == 9);
        }
    }
}

TEST_CASE("walk: unreadable directories are skipped and counted", "[walk][permission]") {
    if (navigator::test::running_as_root()) {
        SKIP("permission tests are meaningless as root");
    }
    const TempDir t;
    t.dir("ok");
    t.file("ok/f");
    t.dir("locked");
    t.file("locked/secret");
    REQUIRE(::chmod((t / "locked").c_str(), 0) == 0);

    SECTION("skip_unreadable true") {
        const Collected c = walk_all(opts_for(t.path()));
        CHECK(as_set(c.paths) == std::set<std::string>{"ok", "ok/f", "locked"});
        CHECK(c.stats.skipped_permission == 1);
        REQUIRE(c.stats.samples.size() == 1);
        CHECK(c.stats.samples[0].relative_path == "locked");
        CHECK(c.stats.samples[0].code == ErrorCode::PermissionDenied);
        CHECK(c.stats.dirs_visited == 2);
    }
    SECTION("skip_unreadable false fails the walk") {
        auto o = opts_for(t.path());
        o.skip_unreadable = false;
        auto w = fs::Walker::open(o);
        REQUIRE(w.has_value());
        fs::WalkBatch b;
        bool failed = false;
        for (int i = 0; i < 100; ++i) {
            auto r = w->next_batch(b, std::stop_token{});
            if (!r) {
                CHECK(r.error().code == ErrorCode::PermissionDenied);
                failed = true;
                break;
            }
            if (!*r) {
                break;
            }
        }
        CHECK(failed);
        CHECK_FALSE(w->stats().done);
        auto after = w->next_batch(b, std::stop_token{});
        REQUIRE(after.has_value());
        CHECK_FALSE(*after);
    }
    SECTION("an unreadable root fails open") {
        auto w = fs::Walker::open(opts_for(t / "locked"));
        REQUIRE_FALSE(w.has_value());
        CHECK(w.error().code == ErrorCode::PermissionDenied);
    }
}

TEST_CASE("walk: directories that vanish mid-walk are skipped", "[walk][race]") {
    const TempDir t;
    t.dir("a");
    t.file("a/x");
    t.dir("b");
    t.file("b/y");
    t.dir("c");
    t.file("c/z");
    auto o = opts_for(t.path());
    o.batch_size = 3;
    auto w = fs::Walker::open(o);
    REQUIRE(w.has_value());
    fs::WalkBatch b;
    auto r = w->next_batch(b, std::stop_token{});
    REQUIRE(r.has_value());
    REQUIRE(*r);
    REQUIRE(b.entries.size() == 3); // a, b, c returned and queued; none opened yet
    REQUIRE(b.new_dirs.size() == 3);
    // a disappears, b becomes a file, c becomes a symlink to a directory.
    std::filesystem::remove_all(t / "a");
    std::filesystem::remove_all(t / "b");
    t.file("b");
    std::filesystem::remove_all(t / "c");
    const TempDir other;
    other.file("w");
    t.link("c", other.path().native());
    std::vector<std::string> rest;
    for (;;) {
        auto n = w->next_batch(b, std::stop_token{});
        REQUIRE(n.has_value());
        for (const auto& e : b.entries) {
            rest.push_back(e.entry.name);
        }
        if (!*n) {
            break;
        }
    }
    // readdir may or may not report the replacement entries of the root; nothing below the
    // vanished or replaced directories may appear.
    for (const std::string& name : rest) {
        CHECK(name != "x");
        CHECK(name != "y");
        CHECK(name != "z");
        CHECK(name != "w");
    }
    CHECK(w->stats().skipped_vanished == 3);
    CHECK(w->stats().done);
    CHECK(w->stats().samples.empty());
}

TEST_CASE("walk: max_depth", "[walk]") {
    const TempDir t;
    std::vector<std::string> all;
    gen_tree(t.path().native(), "", 3, 2, 1, all);
    SECTION("1 = a listing") {
        auto o = opts_for(t.path());
        o.max_depth = 1;
        const Collected c = walk_all(o);
        CHECK(as_set(c.paths) == std::set<std::string>{"f0.txt", "d0", "d1"});
        for (const auto& e : c.entries) {
            CHECK(e.self_dir == 0);
        }
        CHECK(c.stats.dirs_visited == 1);
    }
    SECTION("2") {
        auto o = opts_for(t.path());
        o.max_depth = 2;
        const Collected c = walk_all(o);
        CHECK(c.paths.size() == 3 + 2 * 3);
        for (const auto& e : c.entries) {
            CHECK(e.depth <= 2);
            CHECK((e.self_dir != 0) == (e.depth == 1 && e.entry.kind == fs::EntryKind::Directory));
        }
    }
}

TEST_CASE("walk: max_entries truncates", "[walk]") {
    const TempDir t;
    for (int i = 0; i < 100; ++i) {
        t.file("f" + std::to_string(i));
    }
    SECTION("below the total") {
        auto o = opts_for(t.path());
        o.max_entries = 10;
        o.batch_size = 4;
        const Collected c = walk_all(o);
        CHECK(c.entries.size() == 10);
        CHECK(c.stats.truncated);
    }
    SECTION("exactly the total is not truncated") {
        auto o = opts_for(t.path());
        o.max_entries = 100;
        const Collected c = walk_all(o);
        CHECK(c.entries.size() == 100);
        CHECK_FALSE(c.stats.truncated);
    }
    SECTION("0 = unlimited") {
        auto o = opts_for(t.path());
        o.max_entries = 0;
        const Collected c = walk_all(o);
        CHECK(c.entries.size() == 100);
    }
}

TEST_CASE("walk: cross_filesystems boundary at /proc", "[walk][mount]") {
    struct stat root_st {};
    struct stat proc_st {};
    if (::stat("/", &root_st) != 0 || ::stat("/proc/self", &proc_st) != 0 ||
        root_st.st_dev == proc_st.st_dev) {
        SKIP("/proc is not a separate mount here");
    }
    for (const bool cross : {false, true}) {
        INFO("cross_filesystems " << cross);
        fs::WalkOptions o;
        o.root = "/";
        o.max_depth = 2;
        o.metadata = false;
        o.cross_filesystems = cross;
        const Collected c = walk_all(o);
        const fs::WalkEntry* proc = find(c, "proc");
        REQUIRE(proc != nullptr);
        CHECK(proc->entry.kind == fs::EntryKind::Directory);
        if (cross) {
            CHECK(proc->self_dir != 0);
            CHECK(find(c, "proc/self") != nullptr);
        } else {
            CHECK(proc->self_dir == 0);
            CHECK(c.stats.skipped_other_fs >= 1);
            CHECK(find(c, "proc/self") == nullptr);
        }
    }
}

TEST_CASE("walk: Unicode and invalid UTF-8 names round-trip as raw bytes", "[walk][unicode]") {
    const TempDir t;
    t.dir("M\xC3\xBCzik");                             // Müzik
    t.file("M\xC3\xBCzik/\xC5\x9E\x61rk\xC4\xB1.mp3"); // Şarkı.mp3
    t.dir("\xC4\xB0\xC3\xA7\x65rik");                  // İçerik
    t.file("\xC4\xB0\xC3\xA7\x65rik/\xC4\x9F\xC3\xBC\xC5\x9F\xC4\xB1\xC3\xB6\xC3\xA7.txt");
    t.file("caf\xC3\xA9");  // NFC
    t.file("cafe\xCC\x81"); // NFD: a different name
    t.dir("bad\xFF\xFE");
    t.file("bad\xFF\xFE/inner");
    t.dir("\xE6\x97\xA5\xE6\x9C\xAC"); // 日本
    t.file("\xE6\x97\xA5\xE6\x9C\xAC/\xF0\x9F\x93\x81 emoji");
    const Collected c = walk_all(opts_for(t.path()));
    CHECK(as_set(c.paths) ==
          std::set<std::string>{
              "M\xC3\xBCzik", "M\xC3\xBCzik/\xC5\x9E\x61rk\xC4\xB1.mp3", "\xC4\xB0\xC3\xA7\x65rik",
              "\xC4\xB0\xC3\xA7\x65rik/\xC4\x9F\xC3\xBC\xC5\x9F\xC4\xB1\xC3\xB6\xC3\xA7.txt",
              "caf\xC3\xA9", "cafe\xCC\x81", "bad\xFF\xFE", "bad\xFF\xFE/inner",
              "\xE6\x97\xA5\xE6\x9C\xAC", "\xE6\x97\xA5\xE6\x9C\xAC/\xF0\x9F\x93\x81 emoji"});
    for (const auto& p : c.paths) {
        struct stat st {};
        CHECK(::lstat((t.path() / p).c_str(), &st) == 0);
    }
}

TEST_CASE("walk: open() validation and root symlink", "[walk]") {
    const TempDir t;
    t.file("plain");
    t.dir("real");
    t.file("real/inside");
    t.link("via", "real");

    auto bad = [](const fs::WalkOptions& o, ErrorCode code) {
        auto w = fs::Walker::open(o);
        REQUIRE_FALSE(w.has_value());
        CHECK(w.error().code == code);
    };
    fs::WalkOptions o;
    bad(o, ErrorCode::InvalidArgument); // empty
    o.root = "relative/dir";
    bad(o, ErrorCode::InvalidArgument);
    o.root = t / "plain";
    bad(o, ErrorCode::InvalidArgument);
    o.root = t / "missing";
    bad(o, ErrorCode::NotFound);
    o.root = t.path();
    o.batch_size = 0;
    bad(o, ErrorCode::InvalidArgument);
    o.batch_size = 16;
    o.max_depth = 0;
    bad(o, ErrorCode::InvalidArgument);

    const Collected c = walk_all(opts_for(t / "via"));
    CHECK(as_set(c.paths) == std::set<std::string>{"inside"});

    // Moving a walker keeps it working.
    auto w = fs::Walker::open(opts_for(t / "real"));
    REQUIRE(w.has_value());
    fs::Walker moved = std::move(*w);
    CHECK(moved.options().root == t / "real");
    fs::WalkBatch b;
    auto r = moved.next_batch(b, std::stop_token{});
    REQUIRE(r.has_value());
    CHECK(b.entries.size() == 1);
}

TEST_CASE("walk: stop already requested", "[walk][cancel]") {
    const TempDir t;
    t.file("a");
    auto w = fs::Walker::open(opts_for(t.path()));
    REQUIRE(w.has_value());
    std::stop_source src;
    src.request_stop();
    fs::WalkBatch b;
    auto r = w->next_batch(b, src.get_token());
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == ErrorCode::Cancelled);
    auto after = w->next_batch(b, std::stop_token{});
    REQUIRE(after.has_value());
    CHECK_FALSE(*after);
    CHECK_FALSE(w->stats().done);
}

TEST_CASE("walk: batch_latency and batch_size shape the batches", "[walk][batch]") {
    const TempDir t;
    const std::size_t n = gen_wide(t.path(), 2, 10, 1000); // 20k files + 22 dirs
    SECTION("small latency, huge batch_size: several batches, each returned promptly") {
        auto o = opts_for(t.path());
        o.batch_size = 10'000'000;
        o.batch_latency = std::chrono::milliseconds{1};
        auto w = fs::Walker::open(o);
        REQUIRE(w.has_value());
        fs::WalkBatch b;
        std::size_t total = 0;
        std::size_t batches = 0;
        double worst_ms = 0;
        for (;;) {
            const auto start = Clock::now();
            auto r = w->next_batch(b, std::stop_token{});
            worst_ms = std::max(worst_ms, ms_since(start));
            REQUIRE(r.has_value());
            total += b.entries.size();
            ++batches;
            if (!*r) {
                break;
            }
            CHECK_FALSE(b.entries.empty());
        }
        CHECK(total == n);
        CHECK(batches > 1);
        // 1 ms latency + at most 32 entries of slack; generous bound for sanitizer builds.
        CHECK(worst_ms < 250.0);
    }
    SECTION("long latency: every batch but the last is exactly batch_size") {
        auto o = opts_for(t.path());
        o.batch_size = 333;
        o.batch_latency = std::chrono::hours{1};
        auto w = fs::Walker::open(o);
        REQUIRE(w.has_value());
        fs::WalkBatch b;
        std::vector<std::size_t> sizes;
        for (;;) {
            auto r = w->next_batch(b, std::stop_token{});
            REQUIRE(r.has_value());
            sizes.push_back(b.entries.size());
            if (!*r) {
                break;
            }
        }
        std::size_t total = 0;
        for (std::size_t i = 0; i < sizes.size(); ++i) {
            if (i + 1 < sizes.size()) {
                CHECK(sizes[i] == 333);
            }
            total += sizes[i];
        }
        CHECK(total == n);
    }
}

TEST_CASE("walk: cancellation latency during a 200k-entry walk", "[walk][cancel]") {
    const TempDir t;
    const std::size_t n = gen_wide(t.path(), 20, 10, 1000); // 200k files + 220 dirs
    REQUIRE(n > 200'000);
    auto o = opts_for(t.path());
    o.batch_size = 10'000'000; // one call would cover the whole walk
    o.batch_latency = std::chrono::hours{1};
    auto w = fs::Walker::open(o);
    REQUIRE(w.has_value());

    std::stop_source src;
    std::atomic<bool> started{false};
    Clock::time_point returned_at{};
    bool cancelled = false;
    std::size_t returned_entries = 0;
    {
        std::jthread worker([&] {
            fs::WalkBatch b;
            started.store(true);
            auto r = w->next_batch(b, src.get_token());
            returned_at = Clock::now();
            cancelled = !r.has_value() && r.error().code == ErrorCode::Cancelled;
            returned_entries = w->stats().entries_returned;
        });
        while (!started.load()) {
            std::this_thread::yield();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{30});
        const auto requested_at = Clock::now();
        src.request_stop();
        worker.join();
        const double latency_ms =
            std::chrono::duration<double, std::milli>(returned_at - requested_at).count();
        std::printf("[walk cancel] stop -> Cancelled: %.3f ms after %zu of %zu entries\n",
                    latency_ms, returned_entries, n);
        REQUIRE(cancelled);
        CHECK(returned_entries < n);
        CHECK(latency_ms < 100.0); // contract: one getdents chunk + <= 1024 fstatat calls
    }
    CHECK_FALSE(w->stats().done);
}

// Manual: `navigator_core_tests "[.walkperf]"` (docs/perf/phase5-walk.md).
TEST_CASE("walk perf: 300k entries, metadata on and off", "[.walkperf]") {
    const TempDir t;
    auto start = Clock::now();
    const std::size_t n = gen_wide(t.path(), 30, 10, 1000); // 300k files + 330 dirs
    std::printf("[walk perf] created %zu entries in %.0f ms\n", n, ms_since(start));
    for (int round = 0; round < 3; ++round) {
        for (const bool metadata : {true, false}) {
            auto o = opts_for(t.path());
            o.metadata = metadata;
            auto w = fs::Walker::open(o);
            REQUIRE(w.has_value());
            fs::WalkBatch b;
            std::size_t total = 0;
            double first_ms = -1;
            start = Clock::now();
            for (;;) {
                auto r = w->next_batch(b, std::stop_token{});
                REQUIRE(r.has_value());
                if (first_ms < 0) {
                    first_ms = ms_since(start);
                }
                total += b.entries.size();
                if (!*r) {
                    break;
                }
            }
            const double ms = ms_since(start);
            REQUIRE(total == n);
            std::printf("[walk perf] round %d metadata=%d: %.1f ms total, first batch %.2f ms, "
                        "%.0f entries/s\n",
                        round, metadata ? 1 : 0, ms, first_ms,
                        static_cast<double>(n) / ms * 1000.0);
        }
    }
}
