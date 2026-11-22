// Edge cases of the name index: entry accounting, directory moves, stale event paths, hostile
// offsets, permission skips and self-induced saves (navigator/index.hpp).
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <utility>

#include <catch2/catch_test_macros.hpp>
#include <sys/stat.h>

#include <navigator/index.hpp>
#include <navigator/search.hpp>

#include "index_test_util.hpp"
#include "index_tree.hpp"

using namespace navigator;
using namespace navigator::search;
using namespace navigator::test;
using namespace std::chrono_literals;
namespace stdfs = std::filesystem;
namespace idx = navigator::search::detail;

namespace {

NameIndexOptions options_at(const stdfs::path& root, const Path& file = {}) {
    NameIndexOptions o;
    o.root = root;
    o.identity = test_identity(root);
    o.file = file;
    o.rescan_interval = 0s;
    o.save_interval = 3600s;
    o.background_priority = false;
    return o;
}

std::unique_ptr<IIndex> started(NameIndexOptions o) {
    auto r = create_name_index(std::move(o));
    REQUIRE(r);
    (*r)->start();
    REQUIRE(wait_ready(**r));
    return std::move(*r);
}

void verify_now(IIndex& index) {
    const Timestamp last = index.status().last_complete_scan;
    index.request_rescan(RescanKind::Verify);
    REQUIRE(wait_until([&] {
        const IndexStatus s = index.status();
        return s.state == IndexState::Ready && s.last_complete_scan != last;
    }));
}

void put_u64(std::string& bytes, std::size_t at, std::uint64_t value) {
    for (std::size_t i = 0; i < 8; ++i) {
        bytes[at + i] = static_cast<char>((value >> (8U * i)) & 0xFFU);
    }
}

} // namespace

TEST_CASE("re-listing accounts for removed subtrees", "[search][index][regression]") {
    const TempDir t;
    stdfs::create_directories(t / "doomed/inner");
    touch(t / "doomed/inner/a");
    touch(t / "doomed/b");
    touch(t / "stays");
    const auto index = started(options_at(t.path()));
    REQUIRE(index->status().entries == 5);
    stdfs::remove_all(t / "doomed");
    verify_now(*index);
    CHECK(index->status().entries == 1);
    CHECK(run_query(*index, "").hits.size() == 1);
}

TEST_CASE("max_entries never evicts what is already indexed", "[search][index][regression]") {
    const TempDir t;
    for (int i = 0; i < 5; ++i) {
        touch(t / ("old" + std::to_string(i)));
    }
    NameIndexOptions o = options_at(t.path());
    o.max_entries = 6;
    const auto index = started(std::move(o));
    for (int i = 0; i < 20; ++i) {
        touch(t / ("new" + std::to_string(i)));
    }
    verify_now(*index);
    CHECK(index->status().truncated);
    CHECK(run_query(*index, "old").hits.size() == 5);
    CHECK(index->status().entries == 6);
}

TEST_CASE("a directory moved to a shallower parent keeps its subtree",
          "[search][index][regression]") {
    const TempDir t;
    stdfs::create_directories(t / "a/b/c/moved/inner");
    touch(t / "a/b/c/moved/inner/leaf");
    const auto index = started(options_at(t.path()));
    stdfs::rename(t / "a/b/c/moved", t / "moved");
    verify_now(*index);
    CHECK(index_has(*index, t / "moved/inner/leaf"));
    CHECK_FALSE(index_has(*index, t / "a/b/c/moved/inner/leaf"));
    CHECK(index->status().entries == reference_entries(t.path()).size());
}

TEST_CASE("MovedTo delivered before MovedFrom keeps the subtree", "[search][index][regression]") {
    const TempDir t;
    stdfs::create_directories(t / "from/moved/inner");
    stdfs::create_directory(t / "to");
    touch(t / "from/moved/inner/leaf");
    auto hub = std::make_shared<FakeHub>();
    NameIndexOptions o = options_at(t.path());
    o.watcher_factory = fake_factory(hub);
    const auto index = started(std::move(o));
    stdfs::rename(t / "from/moved", t / "to/moved");
    REQUIRE(hub->emit(t / "to", WatchEventKind::MovedTo, "moved"));
    REQUIRE(hub->emit(t / "from", WatchEventKind::MovedFrom, "moved"));
    CHECK(wait_until([&] { return index_has(*index, t / "to/moved/inner/leaf"); }, 3s));
    CHECK(wait_until([&] { return !index_has(*index, t / "from/moved"); }, 3s));
}

TEST_CASE("events queued before an ancestor rename are not lost", "[search][index][regression]") {
    const TempDir t;
    stdfs::create_directories(t / "a/b/c");
    auto hub = std::make_shared<FakeHub>();
    NameIndexOptions o = options_at(t.path());
    o.watcher_factory = fake_factory(hub);
    const auto index = started(std::move(o));
    touch(t / "a/b/c/created_then_moved");
    stdfs::rename(t / "a", t / "renamed");
    REQUIRE(hub->emit(t / "a/b/c", WatchEventKind::Created, "created_then_moved"));
    REQUIRE(hub->emit(t.path(), WatchEventKind::Renamed, "renamed", "a"));
    CHECK(wait_until([&] { return index_has(*index, t / "renamed/b/c/created_then_moved"); }, 3s));
}

TEST_CASE("an Overflow during a scan schedules a verify pass", "[search][index][regression]") {
    const TempDir t;
    for (int i = 0; i < 200; ++i) {
        stdfs::create_directory(t / ("d" + std::to_string(i)));
    }
    auto hub = std::make_shared<FakeHub>();
    hub->overflow();
    std::atomic<IIndex*> self{nullptr};
    std::atomic<bool> verified{false};
    NameIndexOptions o = options_at(t.path());
    o.watcher_factory = fake_factory(hub);
    o.on_status_change = [&] {
        if (IIndex* p = self.load(); p != nullptr && p->status().state == IndexState::Verifying) {
            verified = true;
        }
    };
    auto r = create_name_index(std::move(o));
    REQUIRE(r);
    self = r->get();
    (*r)->start();
    CHECK(wait_until([&] { return verified.load(); }, 3s));
    (*r)->stop();
}

TEST_CASE("a readable but unsearchable directory counts as skipped",
          "[search][index][regression]") {
    if (running_as_root()) {
        SKIP("permissions do not apply to root");
    }
    const TempDir t;
    stdfs::create_directory(t / "no_exec");
    touch(t / "no_exec/inside");
    REQUIRE(::chmod((t / "no_exec").c_str(), 0600) == 0);
    const auto index = started(options_at(t.path()));
    CHECK(index->status().skipped_permission == 1);
}

TEST_CASE("a failed root keeps serving its data while retrying", "[search][index][regression]") {
    const TempDir t;
    const stdfs::path root = t / "vol";
    stdfs::create_directory(root);
    touch(root / "kept");
    NameIndexOptions o = options_at(root);
    o.rescan_interval = 1s;
    const auto index = started(std::move(o));
    stdfs::rename(root, t / "gone");
    REQUIRE(wait_until([&] { return index->status().state == IndexState::Failed; }));
    std::this_thread::sleep_for(1500ms); // at least one retry
    CHECK(index_has(*index, root / "kept"));
}

TEST_CASE("the index's own saves do not cause more saves", "[search][index][regression]") {
    const TempDir t;
    const Path file = t / "cache/index/self.idx";
    stdfs::create_directories(file.parent_path());
    auto hub = std::make_shared<FakeHub>();
    NameIndexOptions o = options_at(t.path(), file);
    o.watcher_factory = fake_factory(hub);
    o.save_interval = 0s;
    const auto index = started(std::move(o));
    REQUIRE(wait_until([&] { return stdfs::exists(file); }));
    std::this_thread::sleep_for(100ms);
    struct stat before {};
    REQUIRE(::stat(file.c_str(), &before) == 0);
    // What inotify reports for the save that just happened.
    REQUIRE(hub->emit(file.parent_path(), WatchEventKind::Created, "self.idx"));
    REQUIRE(wait_until([&] { return index_has(*index, file); }));
    std::this_thread::sleep_for(300ms);
    struct stat after {};
    REQUIRE(::stat(file.c_str(), &after) == 0);
    CHECK(after.st_ino == before.st_ino); // a save replaces the inode
}

TEST_CASE("names that overlap in the pool are rejected", "[search][index][file][regression]") {
    idx::Tree tree;
    tree.reset();
    std::string fold;
    for (const char* name : {"aaaa", "bbbb"}) {
        idx::Child c;
        REQUIRE(idx::append_name(tree.dirs[0].pool, c, name, fold));
        tree.dirs[0].children.push_back(c);
    }
    tree.entries = 2;
    const VolumeIdentity id = test_identity("/vol");
    auto bytes = idx::encode_index(tree, id, 0);
    REQUIRE(bytes);
    // Both entries claim the whole 8-byte pool (memory amplification on load).
    std::size_t entries = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        entries |= std::size_t{static_cast<unsigned char>((*bytes)[64 + i])} << (8U * i);
    }
    for (std::size_t e = 0; e < 2; ++e) {
        const std::size_t at = entries + (16 * e);
        for (std::size_t i = 0; i < 6; ++i) {
            (*bytes)[at + i] = '\0';
        }
        (*bytes)[at + 4] = 8;
    }
    put_u64(*bytes, bytes->size() - 8,
            fnv1a64(std::string_view(*bytes).substr(0, bytes->size() - 8)));
    CHECK_FALSE(idx::decode_index(*bytes, id));
}

TEST_CASE("section offsets that wrap are rejected", "[search][index][file][regression]") {
    idx::Tree tree;
    tree.reset();
    const VolumeIdentity id = test_identity("/vol");
    auto bytes = idx::encode_index(tree, id, 0);
    REQUIRE(bytes);
    const std::uint64_t end = bytes->size() - 8;
    put_u64(*bytes, 56, 0xFFFFFFFFFFFFFFF8ULL); // dirs_offset + 48 wraps to 40
    put_u64(*bytes, 64, 48);                    // entries_offset
    put_u64(*bytes, 72, end);                   // pool_offset
    put_u64(*bytes, end, fnv1a64(std::string_view(*bytes).substr(0, end)));
    CHECK_FALSE(idx::decode_index(*bytes, id));
}

TEST_CASE("an unmounted mount point becomes an ordinary directory again",
          "[search][index][file][regression]") {
    const TempDir t;
    const TempDir cache;
    stdfs::create_directory(t / "mnt");
    touch(t / "mnt/underneath");
    const Path file = cache / "m.idx";
    NameIndexOptions o = options_at(t.path(), file);
    o.background_priority = true; // best effort, must not fail the scan
    started(o)->stop();
    // Make the saved index say that a mount covered "mnt" when it was saved.
    auto bytes = idx::read_file(file);
    REQUIRE(bytes);
    auto tree = idx::decode_index(*bytes, o.identity);
    REQUIRE(tree);
    REQUIRE(tree->dirs.size() == 2);
    tree->entries -= tree->dirs[1].children.size();
    tree->dirs[1].children.clear();
    tree->dirs[1].flags = idx::kDirOtherFs;
    auto edited = idx::encode_index(*tree, o.identity, 0);
    REQUIRE(edited);
    REQUIRE(idx::write_file_atomic(file, *edited));
    const auto index = started(o);
    CHECK(index->status().loaded_from_file);
    CHECK(index_has(*index, t / "mnt/underneath"));
}
