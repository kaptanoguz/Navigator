// inotify IWatcher: real kernel events in temporary directories, plus the internal seams for
// paths a test cannot provoke reliably (read errors, synthetic records).
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <fcntl.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

#include <navigator/watch.hpp>

#include "inotify_watcher.hpp"
#include "test_support.hpp"

namespace {

using navigator::IWatcher;
using navigator::kInvalidWatchHandle;
using navigator::WatchEvent;
using navigator::WatchEventKind;
using navigator::WatchHandle;
using navigator::platform_internal::InotifyWatcher;
using navigator::platform_internal::RawInotifyEvent;
using navigator::platform_test::TempDir;
using navigator::platform_test::write_file;
namespace stdfs = std::filesystem;
using namespace std::chrono_literals;

std::unique_ptr<InotifyWatcher> make_watcher() {
    auto created = InotifyWatcher::create();
    REQUIRE(created.has_value());
    return std::move(*created);
}

WatchHandle add_ok(IWatcher& w, const navigator::Path& dir) {
    auto handle = w.add(dir);
    REQUIRE(handle.has_value());
    REQUIRE(*handle != kInvalidWatchHandle);
    return *handle;
}

// Drains everything currently available. Local filesystem events are queued synchronously by
// the syscall that causes them, so one wait()+poll() after the action sees them all.
std::vector<WatchEvent> drain(IWatcher& w) {
    std::vector<WatchEvent> events;
    while (w.wait(50ms)) {
        auto n = w.poll(events);
        REQUIRE(n.has_value());
        if (*n == 0) {
            break;
        }
    }
    return events;
}

bool has_event(const std::vector<WatchEvent>& events, WatchEventKind kind, WatchHandle handle,
               const std::string& name) {
    return std::any_of(events.begin(), events.end(), [&](const WatchEvent& e) {
        return e.kind == kind && e.handle == handle && e.name == name;
    });
}

std::size_t count_kind(const std::vector<WatchEvent>& events, WatchEventKind kind) {
    return static_cast<std::size_t>(std::count_if(
        events.begin(), events.end(), [&](const WatchEvent& e) { return e.kind == kind; }));
}

void touch(const navigator::Path& path) { write_file(path, ""); }

} // namespace

TEST_CASE("create_inotify_watcher returns a pollable watcher", "[watch]") {
    auto created = navigator::create_inotify_watcher();
    REQUIRE(created.has_value());
    CHECK((*created)->native_handle() >= 0);
    std::vector<WatchEvent> out;
    auto n = (*created)->poll(out);
    REQUIRE(n.has_value());
    CHECK(*n == 0);
}

TEST_CASE("created, modified, attrib and deleted files are reported", "[watch]") {
    TempDir tmp;
    auto w = make_watcher();
    const WatchHandle h = add_ok(*w, tmp.path());

    touch(tmp / "a.txt");
    auto events = drain(*w);
    CHECK(has_event(events, WatchEventKind::Created, h, "a.txt"));
    for (const WatchEvent& e : events) {
        if (e.kind == WatchEventKind::Created) {
            CHECK_FALSE(e.is_directory);
        }
    }

    {
        std::ofstream out(tmp / "a.txt", std::ios::app);
        out << "hello";
    }
    events = drain(*w);
    CHECK(has_event(events, WatchEventKind::Modified, h, "a.txt"));
    // Coalescing: never two identical consecutive Modified events.
    for (std::size_t i = 1; i < events.size(); ++i) {
        const bool dup = events[i].kind == WatchEventKind::Modified &&
                         events[i - 1].kind == WatchEventKind::Modified &&
                         events[i].handle == events[i - 1].handle &&
                         events[i].name == events[i - 1].name;
        CHECK_FALSE(dup);
    }

    REQUIRE(::chmod((tmp / "a.txt").c_str(), 0600) == 0);
    events = drain(*w);
    CHECK(has_event(events, WatchEventKind::Attrib, h, "a.txt"));

    REQUIRE(::chmod(tmp.path().c_str(), 0750) == 0);
    events = drain(*w);
    CHECK(has_event(events, WatchEventKind::Attrib, h, "")); // the directory itself

    stdfs::remove(tmp / "a.txt");
    events = drain(*w);
    CHECK(has_event(events, WatchEventKind::Deleted, h, "a.txt"));
}

TEST_CASE("subdirectory creation sets is_directory", "[watch]") {
    TempDir tmp;
    auto w = make_watcher();
    const WatchHandle h = add_ok(*w, tmp.path());
    stdfs::create_directory(tmp / "sub");
    auto events = drain(*w);
    REQUIRE(events.size() == 1);
    CHECK(events[0].kind == WatchEventKind::Created);
    CHECK(events[0].handle == h);
    CHECK(events[0].name == "sub");
    CHECK(events[0].is_directory);

    // Changes inside the subdirectory are not reported (non-recursive).
    touch(tmp / "sub" / "inner");
    events = drain(*w);
    CHECK_FALSE(has_event(events, WatchEventKind::Created, h, "inner"));
}

TEST_CASE("rename within a directory is one Renamed event", "[watch]") {
    TempDir tmp;
    touch(tmp / "old");
    auto w = make_watcher();
    const WatchHandle h = add_ok(*w, tmp.path());
    stdfs::rename(tmp / "old", tmp / "new");
    auto events = drain(*w);
    REQUIRE(events.size() == 1);
    CHECK(events[0].kind == WatchEventKind::Renamed);
    CHECK(events[0].handle == h);
    CHECK(events[0].old_name == "old");
    CHECK(events[0].name == "new");
    CHECK_FALSE(events[0].is_directory);
    CHECK(count_kind(events, WatchEventKind::MovedFrom) == 0);
    CHECK(count_kind(events, WatchEventKind::MovedTo) == 0);
}

TEST_CASE("moves out of and into a watched directory", "[watch]") {
    TempDir tmp;
    stdfs::create_directory(tmp / "watched");
    stdfs::create_directory(tmp / "other");
    touch(tmp / "watched" / "leaving");
    touch(tmp / "other" / "arriving");
    auto w = make_watcher();
    const WatchHandle h = add_ok(*w, tmp / "watched");

    stdfs::rename(tmp / "watched" / "leaving", tmp / "other" / "leaving");
    auto events = drain(*w);
    REQUIRE(events.size() == 1);
    CHECK(events[0].kind == WatchEventKind::MovedFrom);
    CHECK(events[0].name == "leaving");

    stdfs::rename(tmp / "other" / "arriving", tmp / "watched" / "arriving");
    events = drain(*w);
    REQUIRE(events.size() == 1);
    CHECK(events[0].kind == WatchEventKind::MovedTo);
    CHECK(events[0].handle == h);
    CHECK(events[0].name == "arriving");
}

TEST_CASE("moves between two watched directories are not paired", "[watch]") {
    TempDir tmp;
    stdfs::create_directory(tmp / "a");
    stdfs::create_directory(tmp / "b");
    stdfs::create_directory(tmp / "a" / "dir");
    auto w = make_watcher();
    const WatchHandle ha = add_ok(*w, tmp / "a");
    const WatchHandle hb = add_ok(*w, tmp / "b");
    REQUIRE(ha != hb);
    stdfs::rename(tmp / "a" / "dir", tmp / "b" / "dir2");
    auto events = drain(*w);
    REQUIRE(events.size() == 2);
    CHECK(events[0].kind == WatchEventKind::MovedFrom);
    CHECK(events[0].handle == ha);
    CHECK(events[0].name == "dir");
    CHECK(events[0].is_directory);
    CHECK(events[1].kind == WatchEventKind::MovedTo);
    CHECK(events[1].handle == hb);
    CHECK(events[1].name == "dir2");
    CHECK(events[1].is_directory);
}

TEST_CASE("deleting the watched directory reports WatchedDirGone once", "[watch]") {
    TempDir tmp;
    stdfs::create_directory(tmp / "victim");
    touch(tmp / "victim" / "f");
    auto w = make_watcher();
    const WatchHandle h = add_ok(*w, tmp / "victim");
    REQUIRE(w->test_live_handles() == 1);

    stdfs::remove_all(tmp / "victim");
    auto events = drain(*w);
    CHECK(has_event(events, WatchEventKind::Deleted, h, "f"));
    CHECK(count_kind(events, WatchEventKind::WatchedDirGone) == 1);
    REQUIRE_FALSE(events.empty());
    CHECK(events.back().kind == WatchEventKind::WatchedDirGone);
    CHECK(events.back().handle == h);
    CHECK(events.back().name.empty());
    CHECK(w->test_live_handles() == 0);
    CHECK(w->test_wd(h) == -1);

    w->remove(h); // already gone: no-op
    events = drain(*w);
    CHECK(events.empty());

    // A new directory gets a fresh handle and works.
    stdfs::create_directory(tmp / "victim");
    const WatchHandle h2 = add_ok(*w, tmp / "victim");
    CHECK(h2 != h);
    touch(tmp / "victim" / "g");
    events = drain(*w);
    CHECK(has_event(events, WatchEventKind::Created, h2, "g"));
}

TEST_CASE("renaming the watched directory reports WatchedDirGone", "[watch]") {
    TempDir tmp;
    stdfs::create_directory(tmp / "before");
    auto w = make_watcher();
    const WatchHandle h = add_ok(*w, tmp / "before");
    stdfs::rename(tmp / "before", tmp / "after");
    auto events = drain(*w);
    REQUIRE(count_kind(events, WatchEventKind::WatchedDirGone) == 1);
    CHECK(events.back().handle == h);
    CHECK(w->test_live_handles() == 0);

    // The moved directory no longer reports anything through the old handle.
    touch(tmp / "after" / "x");
    events = drain(*w);
    CHECK(events.empty());
}

TEST_CASE("remove() stops events and discards queued ones", "[watch]") {
    TempDir tmp;
    auto w = make_watcher();
    const WatchHandle h = add_ok(*w, tmp.path());
    touch(tmp / "queued"); // queued in the kernel, not yet polled
    w->remove(h);
    touch(tmp / "after");
    auto events = drain(*w);
    CHECK(events.empty());
    w->remove(h);                   // unknown now: ignored
    w->remove(kInvalidWatchHandle); // ignored
    w->remove(987654);              // never issued: ignored
}

TEST_CASE("adding the same directory twice is reference counted", "[watch]") {
    TempDir tmp;
    stdfs::create_directory(tmp / "d");
    stdfs::create_directory_symlink(tmp / "d", tmp / "link");
    auto w = make_watcher();
    const WatchHandle h1 = add_ok(*w, tmp / "d");
    const WatchHandle h2 = add_ok(*w, tmp / "d");
    const WatchHandle h3 = add_ok(*w, tmp / "link"); // same inode via a symlink
    CHECK(h1 == h2);
    CHECK(h1 == h3);
    CHECK(w->test_live_handles() == 1);

    w->remove(h1);
    w->remove(h1);
    touch(tmp / "d" / "still");
    auto events = drain(*w);
    CHECK(has_event(events, WatchEventKind::Created, h1, "still"));

    w->remove(h1); // last reference
    touch(tmp / "d" / "gone");
    events = drain(*w);
    CHECK(events.empty());
    CHECK(w->test_live_handles() == 0);
}

TEST_CASE("handles are never reused", "[watch]") {
    TempDir tmp;
    auto w = make_watcher();
    std::vector<WatchHandle> seen;
    for (int i = 0; i < 5; ++i) {
        const WatchHandle h = add_ok(*w, tmp.path());
        CHECK(std::find(seen.begin(), seen.end(), h) == seen.end());
        seen.push_back(h);
        w->remove(h);
    }
    CHECK(std::is_sorted(seen.begin(), seen.end()));
    // Events of re-added watches carry only the newest handle.
    const WatchHandle last = add_ok(*w, tmp.path());
    touch(tmp / "n");
    auto events = drain(*w);
    REQUIRE_FALSE(events.empty());
    for (const WatchEvent& e : events) {
        CHECK(e.handle == last);
    }
}

TEST_CASE("add() errors", "[watch]") {
    TempDir tmp;
    auto w = make_watcher();
    touch(tmp / "file");

    auto empty = w->add("");
    REQUIRE_FALSE(empty.has_value());
    CHECK(empty.error().code == navigator::ErrorCode::InvalidArgument);

    auto file = w->add(tmp / "file");
    REQUIRE_FALSE(file.has_value());
    CHECK(file.error().code == navigator::ErrorCode::InvalidArgument);

    auto missing = w->add(tmp / "missing");
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().code == navigator::ErrorCode::NotFound);

    if (::geteuid() != 0) {
        stdfs::create_directory(tmp / "locked");
        REQUIRE(::chmod((tmp / "locked").c_str(), 0) == 0);
        auto denied = w->add(tmp / "locked");
        REQUIRE_FALSE(denied.has_value());
        CHECK(denied.error().code == navigator::ErrorCode::PermissionDenied);
        REQUIRE(::chmod((tmp / "locked").c_str(), 0700) == 0);
    }
    CHECK(w->test_live_handles() == 0);
}

TEST_CASE("wait() honours timeouts and readiness", "[watch]") {
    TempDir tmp;
    auto w = make_watcher();
    const WatchHandle h = add_ok(*w, tmp.path());

    CHECK_FALSE(w->wait(0ms));
    const auto start = std::chrono::steady_clock::now();
    CHECK_FALSE(w->wait(60ms));
    CHECK(std::chrono::steady_clock::now() - start >= 50ms);

    touch(tmp / "x");
    CHECK(w->wait(0ms));
    CHECK(w->wait(-1ms)); // events pending: returns at once
    std::vector<WatchEvent> out;
    REQUIRE(w->poll(out).has_value());
    CHECK(has_event(out, WatchEventKind::Created, h, "x"));
    CHECK_FALSE(w->wait(0ms));
}

TEST_CASE("poll() appends and keeps existing contents", "[watch]") {
    TempDir tmp;
    auto w = make_watcher();
    const WatchHandle h = add_ok(*w, tmp.path());
    touch(tmp / "x");
    std::vector<WatchEvent> out(1);
    out[0].name = "sentinel";
    REQUIRE(w->wait(1000ms));
    auto n = w->poll(out);
    REQUIRE(n.has_value());
    REQUIRE(*n >= 1);
    CHECK(out.size() == 1 + *n);
    CHECK(out[0].name == "sentinel");
    CHECK(out[1].handle == h);
}

TEST_CASE("kernel queue overflow is reported and watches stay active", "[watch][overflow]") {
    long limit = 0;
    if (FILE* f = std::fopen("/proc/sys/fs/inotify/max_queued_events", "r")) {
        if (std::fscanf(f, "%ld", &limit) != 1) {
            limit = 0;
        }
        std::fclose(f);
    }
    if (limit <= 0 || limit > 200000) {
        SKIP("fs.inotify.max_queued_events is " << limit << ": too large to provoke quickly");
    }
    TempDir tmp;
    touch(tmp / "p");
    touch(tmp / "q");
    auto w = make_watcher();
    const WatchHandle h = add_ok(*w, tmp.path());
    // Alternate two files so the kernel cannot merge identical consecutive events.
    long failures = 0;
    for (long i = 0; i <= limit + 16; ++i) {
        const auto& target = (i % 2 == 0) ? "p" : "q";
        failures += ::chmod((tmp / target).c_str(), (i % 4 < 2) ? 0600 : 0644) == 0 ? 0 : 1;
    }
    REQUIRE(failures == 0);
    std::vector<WatchEvent> events = drain(*w);
    REQUIRE(count_kind(events, WatchEventKind::Overflow) >= 1);
    for (const WatchEvent& e : events) {
        if (e.kind == WatchEventKind::Overflow) {
            CHECK(e.handle == kInvalidWatchHandle);
        }
    }
    touch(tmp / "after");
    events = drain(*w);
    CHECK(has_event(events, WatchEventKind::Created, h, "after"));
}

TEST_CASE("seam: synthetic IN_Q_OVERFLOW becomes one Overflow event", "[watch][seam]") {
    TempDir tmp;
    auto w = make_watcher();
    add_ok(*w, tmp.path());
    w->test_inject(RawInotifyEvent{-1, IN_Q_OVERFLOW, 0, {}});
    w->test_inject(RawInotifyEvent{-1, IN_Q_OVERFLOW, 0, {}});
    CHECK(w->wait(0ms));
    std::vector<WatchEvent> out;
    auto n = w->poll(out);
    REQUIRE(n.has_value());
    REQUIRE(*n == 1);
    CHECK(out[0].kind == WatchEventKind::Overflow);
    CHECK(out[0].handle == kInvalidWatchHandle);
}

TEST_CASE("seam: a read error is reported and the next poll starts with Overflow",
          "[watch][seam]") {
    TempDir tmp;
    auto w = make_watcher();
    const WatchHandle h = add_ok(*w, tmp.path());
    touch(tmp / "lost-or-not");
    w->test_fail_next_read(EIO);
    CHECK(w->wait(0ms));
    std::vector<WatchEvent> out;
    auto failed = w->poll(out);
    REQUIRE_FALSE(failed.has_value());
    CHECK(failed.error().code == navigator::ErrorCode::IoError);
    CHECK(out.empty());

    CHECK(w->wait(0ms)); // the pending Overflow counts as ready
    auto n = w->poll(out);
    REQUIRE(n.has_value());
    REQUIRE(*n >= 1);
    CHECK(out[0].kind == WatchEventKind::Overflow);

    out.clear();
    touch(tmp / "later");
    out = drain(*w);
    CHECK(has_event(out, WatchEventKind::Created, h, "later"));
    CHECK(count_kind(out, WatchEventKind::Overflow) == 0);
}

TEST_CASE("seam: unpaired MovedFrom is not delayed to the next poll", "[watch][seam]") {
    TempDir tmp;
    auto w = make_watcher();
    const WatchHandle h = add_ok(*w, tmp.path());
    const int wd = w->test_wd(h);
    REQUIRE(wd >= 0);

    w->test_inject(RawInotifyEvent{wd, IN_MOVED_FROM, 77, "a"});
    std::vector<WatchEvent> out;
    REQUIRE(w->poll(out).has_value());
    REQUIRE(out.size() == 1);
    CHECK(out[0].kind == WatchEventKind::MovedFrom);

    w->test_inject(RawInotifyEvent{wd, IN_MOVED_TO, 77, "b"});
    out.clear();
    REQUIRE(w->poll(out).has_value());
    REQUIRE(out.size() == 1);
    CHECK(out[0].kind == WatchEventKind::MovedTo);
    CHECK(out[0].name == "b");

    // Same poll: paired. Different cookie: not paired.
    w->test_inject(RawInotifyEvent{wd, IN_MOVED_FROM | IN_ISDIR, 5, "x"});
    w->test_inject(RawInotifyEvent{wd, IN_MOVED_TO | IN_ISDIR, 6, "y"});
    w->test_inject(RawInotifyEvent{wd, IN_MOVED_TO | IN_ISDIR, 5, "z"});
    out.clear();
    REQUIRE(w->poll(out).has_value());
    REQUIRE(out.size() == 2);
    CHECK(out[0].kind == WatchEventKind::Renamed);
    CHECK(out[0].old_name == "x");
    CHECK(out[0].name == "z");
    CHECK(out[0].is_directory);
    CHECK(out[1].kind == WatchEventKind::MovedTo);
    CHECK(out[1].name == "y");
}

TEST_CASE("seam: IN_IGNORED and IN_UNMOUNT end the handle exactly once", "[watch][seam]") {
    TempDir tmp;
    stdfs::create_directory(tmp / "a");
    stdfs::create_directory(tmp / "b");
    auto w = make_watcher();
    const WatchHandle ha = add_ok(*w, tmp / "a");
    const WatchHandle hb = add_ok(*w, tmp / "b");
    const int wda = w->test_wd(ha);
    const int wdb = w->test_wd(hb);

    w->test_inject(RawInotifyEvent{wda, IN_IGNORED, 0, {}});
    w->test_inject(RawInotifyEvent{wda, IN_CREATE, 0, "late"});
    w->test_inject(RawInotifyEvent{wdb, IN_UNMOUNT, 0, {}});
    w->test_inject(RawInotifyEvent{wdb, IN_IGNORED, 0, {}});
    std::vector<WatchEvent> out;
    REQUIRE(w->poll(out).has_value());
    REQUIRE(out.size() == 2);
    CHECK(out[0].kind == WatchEventKind::WatchedDirGone);
    CHECK(out[0].handle == ha);
    CHECK(out[1].kind == WatchEventKind::WatchedDirGone);
    CHECK(out[1].handle == hb);
    CHECK(w->test_live_handles() == 0);
    w->remove(ha);
    w->remove(hb);
}

TEST_CASE("seam: IN_CLOSE_WRITE and IN_MODIFY coalesce into one Modified", "[watch][seam]") {
    TempDir tmp;
    auto w = make_watcher();
    const WatchHandle h = add_ok(*w, tmp.path());
    const int wd = w->test_wd(h);
    w->test_inject(RawInotifyEvent{wd, IN_MODIFY, 0, "f"});
    w->test_inject(RawInotifyEvent{wd, IN_MODIFY, 0, "f"});
    w->test_inject(RawInotifyEvent{wd, IN_CLOSE_WRITE, 0, "f"});
    w->test_inject(RawInotifyEvent{wd, IN_MODIFY, 0, "g"});
    w->test_inject(RawInotifyEvent{wd, IN_MODIFY, 0, "f"});
    std::vector<WatchEvent> out;
    REQUIRE(w->poll(out).has_value());
    REQUIRE(out.size() == 3);
    CHECK(out[0].name == "f");
    CHECK(out[1].name == "g");
    CHECK(out[2].name == "f");
    CHECK(count_kind(out, WatchEventKind::Modified) == 3);
}
