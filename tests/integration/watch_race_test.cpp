// inotify IWatcher + fs::list_directory together, with real writer threads (owner: qa).
//
// The watcher is only touched by the test thread (IWatcher is single-threaded by contract);
// writers only make syscalls.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <stop_token>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <navigator/fs.hpp>
#include <navigator/watch.hpp>

#include "fixture.hpp"

namespace {

using navigator::IWatcher;
using navigator::kInvalidWatchHandle;
using navigator::Path;
using navigator::Result;
using navigator::WatchEvent;
using navigator::WatchEventKind;
using navigator::WatchHandle;
using navigator::integration_test::DirModel;
using navigator::integration_test::list_names;
using navigator::integration_test::TempDir;
using navigator::integration_test::touch;
using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

std::unique_ptr<IWatcher> make_watcher() {
    Result<std::unique_ptr<IWatcher>> created = navigator::create_inotify_watcher();
    REQUIRE(created);
    return std::move(*created);
}

WatchHandle add_watch(IWatcher& watcher, const Path& dir) {
    Result<WatchHandle> handle = watcher.add(dir);
    REQUIRE(handle);
    REQUIRE(*handle != kInvalidWatchHandle);
    return *handle;
}

// Polls everything currently available into `out`. Returns the number appended.
std::size_t poll_into(IWatcher& watcher, std::vector<WatchEvent>& out) {
    Result<std::size_t> n = watcher.poll(out);
    REQUIRE(n);
    return *n;
}

// Polls until the watcher has been quiet for `quiet` (the writer must already have stopped).
void drain(IWatcher& watcher, std::vector<WatchEvent>& out, milliseconds quiet) {
    while (watcher.wait(quiet)) {
        (void)poll_into(watcher, out);
    }
    (void)poll_into(watcher, out);
}

// N random operations in `dir` (watched) and `outside` (unwatched sibling).
void random_ops(const Path& dir, const Path& outside, std::uint32_t seed, int count) {
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> pick(0, 63);
    std::uniform_int_distribution<int> op(0, 9);
    const auto name = [&] { return "f" + std::to_string(pick(rng)); };
    for (int i = 0; i < count; ++i) {
        const Path a = dir / name();
        switch (op(rng)) {
        case 0:
        case 1: (void)touch(a); break;
        case 2: ::unlink(a.c_str()); break;
        case 3: ::rename(a.c_str(), (dir / name()).c_str()); break; // Renamed (or overwrite)
        case 4: ::mkdir(a.c_str(), 0755); break;
        case 5: ::rmdir(a.c_str()); break;
        case 6: ::rename(a.c_str(), (outside / a.filename()).c_str()); break; // MovedFrom
        case 7: ::rename((outside / name()).c_str(), a.c_str()); break;       // MovedTo
        case 8: {
            const int fd = ::open(a.c_str(), O_WRONLY | O_APPEND | O_CLOEXEC);
            if (fd >= 0) {
                [[maybe_unused]] const ssize_t n = ::write(fd, "x", 1); // Modified
                ::close(fd);
            }
            break;
        }
        default: ::chmod(a.c_str(), 0600); break; // Attrib
        }
    }
}

// Fresh listing of `dir` that must succeed.
std::set<std::string> must_list(const Path& dir) {
    std::optional<std::set<std::string>> names = list_names(dir);
    REQUIRE(names.has_value());
    return names.value_or(std::set<std::string>{});
}

std::string describe(const std::set<std::string>& a, const std::set<std::string>& b) {
    std::string out;
    for (const std::string& n : a) {
        if (!b.contains(n)) {
            out += " model-only:" + n;
        }
    }
    for (const std::string& n : b) {
        if (!a.contains(n)) {
            out += " disk-only:" + n;
        }
    }
    return out;
}

} // namespace

TEST_CASE("watch + relist stays in sync with the directory under a concurrent writer",
          "[integration][race][watch]") {
    const std::uint32_t seed = GENERATE(1U, 2U, 3U, 4U, 5U);
    // Eager: polls as soon as events are ready (no Overflow expected). Lazy: sleeps between
    // polls while a longer writer runs, so the kernel queue overflows mid-stream and the model
    // must recover by re-listing while the writer is still going.
    const bool lazy = GENERATE(false, true);
    const int ops = lazy ? 80000 : 3000;
    const TempDir tmp;
    const Path dir = tmp / "watched";
    const Path outside = tmp / "outside";
    std::filesystem::create_directory(dir);
    std::filesystem::create_directory(outside);
    for (int i = 0; i < 64; i += 3) {
        (void)touch(dir / ("f" + std::to_string(i)));
        (void)touch(outside / ("f" + std::to_string(i + 1)));
    }

    const auto watcher = make_watcher();
    const WatchHandle handle = add_watch(*watcher, dir); // before the first listing
    DirModel model(dir);
    REQUIRE(model.relist());

    std::atomic<bool> done{false};
    std::vector<WatchEvent> events;
    std::size_t total = 0;
    {
        const std::jthread writer([&] {
            random_ops(dir, outside, seed, ops);
            done.store(true, std::memory_order_release);
        });
        // Consume concurrently with the writer.
        while (!done.load(std::memory_order_acquire)) {
            if (lazy) {
                std::this_thread::sleep_for(milliseconds(150));
            }
            if (watcher->wait(milliseconds(5))) {
                events.clear();
                total += poll_into(*watcher, events);
                for (const WatchEvent& e : events) {
                    REQUIRE(model.apply(e, handle));
                }
            }
        }
    }
    events.clear();
    drain(*watcher, events, milliseconds(100));
    total += events.size();
    for (const WatchEvent& e : events) {
        REQUIRE(model.apply(e, handle));
    }

    const std::set<std::string> disk = must_list(dir);
    INFO("seed=" << seed << " lazy=" << lazy << " events=" << total << " overflows="
                 << model.overflows() << " diff:" << describe(model.names(), disk));
    REQUIRE(model.names() == disk);
    REQUIRE_FALSE(model.gone());
    CHECK(total > 0);
    WARN("sync: seed=" << seed << " lazy=" << lazy << " events=" << total
                       << " overflows=" << model.overflows());
}

TEST_CASE("burst of creates then deletes: every event delivered or an Overflow",
          "[integration][race][watch][burst]") {
    // 5000: CREATE + CLOSE_WRITE + DELETE = 15000 records, fits the default 16384 queue.
    // 12000: 36000 records, overflows the default queue (all delivered if it was raised).
    const int count = GENERATE(5000, 12000);
    const TempDir tmp;
    const Path dir = tmp / "burst";
    std::filesystem::create_directory(dir);

    const auto watcher = make_watcher();
    const WatchHandle handle = add_watch(*watcher, dir);
    DirModel model(dir);
    REQUIRE(model.relist());

    // Created and deleted without polling in between.
    for (int i = 0; i < count; ++i) {
        REQUIRE(touch(dir / ("b" + std::to_string(i))));
    }
    for (int i = 0; i < count; ++i) {
        REQUIRE(::unlink((dir / ("b" + std::to_string(i))).c_str()) == 0);
    }

    std::vector<WatchEvent> events;
    drain(*watcher, events, milliseconds(100));

    std::map<std::string, int> state; // 0 unseen, 1 created, 2 deleted
    int created = 0;
    int deleted = 0;
    int overflows = 0;
    bool order_ok = true;
    for (const WatchEvent& e : events) {
        REQUIRE(model.apply(e, handle));
        if (e.kind == WatchEventKind::Overflow) {
            ++overflows;
            continue;
        }
        REQUIRE(e.handle == handle);
        if (e.kind == WatchEventKind::Created) {
            ++created;
            order_ok = order_ok && state[e.name] == 0;
            state[e.name] = 1;
        } else if (e.kind == WatchEventKind::Deleted) {
            ++deleted;
            order_ok = order_ok && state[e.name] == 1;
            state[e.name] = 2;
        }
    }
    INFO("count=" << count << " events=" << events.size() << " created=" << created
                  << " deleted=" << deleted << " overflows=" << overflows);
    if (overflows == 0) {
        // No Overflow: nothing may be missing.
        REQUIRE(created == count);
        REQUIRE(deleted == count);
        REQUIRE(order_ok);
    } else {
        REQUIRE(model.relists() >= 2);
    }
    WARN("burst: count=" << count << " events=" << events.size() << " overflows=" << overflows);
    const std::set<std::string> disk = must_list(dir);
    REQUIRE(disk.empty());
    REQUIRE(model.names() == disk);

    // The watch survives an Overflow.
    REQUIRE(touch(dir / "after"));
    events.clear();
    drain(*watcher, events, milliseconds(100));
    const bool seen = std::any_of(events.begin(), events.end(), [&](const WatchEvent& e) {
        return e.kind == WatchEventKind::Created && e.handle == handle && e.name == "after";
    });
    REQUIRE(seen);
}

TEST_CASE("watched directory removed while busy: WatchedDirGone, other watches unaffected",
          "[integration][race][watch]") {
    const bool by_move = GENERATE(false, true);
    const TempDir tmp;
    const Path a = tmp / "a";
    const Path b = tmp / "b";
    std::filesystem::create_directory(a);
    std::filesystem::create_directory(b);

    const auto watcher = make_watcher();
    const WatchHandle ha = add_watch(*watcher, a);
    const WatchHandle hb = add_watch(*watcher, b);
    REQUIRE(ha != hb);
    DirModel model_b(b);
    REQUIRE(model_b.relist());

    std::vector<WatchEvent> events;
    {
        // Both directories busy; `a` disappears underneath its writer.
        const auto churn = [](const Path& dir, const std::stop_token& stop) {
            std::uint64_t i = 0;
            while (!stop.stop_requested()) {
                const Path p = dir / ("c" + std::to_string(i % 32U));
                if (touch(p)) {
                    ::unlink(p.c_str());
                }
                ++i;
            }
        };
        std::jthread writer_a([&](const std::stop_token& stop) { churn(a, stop); });
        std::jthread writer_b([&](const std::stop_token& stop) { churn(b, stop); });

        const auto busy_until = Clock::now() + milliseconds(100);
        while (Clock::now() < busy_until) {
            if (watcher->wait(milliseconds(5))) {
                (void)poll_into(*watcher, events);
            }
        }
        if (by_move) {
            REQUIRE(::rename(a.c_str(), (tmp / "a-moved").c_str()) == 0);
        } else {
            // writer_a keeps creating files until the rmdir wins.
            for (int attempt = 0; attempt < 1000 && std::filesystem::exists(a); ++attempt) {
                std::error_code ec;
                std::filesystem::remove_all(a, ec);
            }
            REQUIRE_FALSE(std::filesystem::exists(a));
        }
        const auto settle_until = Clock::now() + milliseconds(50);
        while (Clock::now() < settle_until) {
            if (watcher->wait(milliseconds(5))) {
                (void)poll_into(*watcher, events);
            }
        }
    }
    drain(*watcher, events, milliseconds(100));

    int gone_a = 0;
    int after_gone_a = 0;
    int gone_b = 0;
    bool overflow = false;
    for (const WatchEvent& e : events) {
        if (e.kind == WatchEventKind::Overflow) {
            overflow = true;
        }
        if (e.handle == ha) {
            if (gone_a > 0) {
                ++after_gone_a;
            }
            if (e.kind == WatchEventKind::WatchedDirGone) {
                ++gone_a;
            }
        }
        if (e.handle == hb && e.kind == WatchEventKind::WatchedDirGone) {
            ++gone_b;
        }
        REQUIRE(model_b.apply(e, hb));
    }
    INFO("by_move=" << by_move << " events=" << events.size() << " overflow=" << overflow);
    if (overflow) {
        // The kernel may drop IN_DELETE_SELF/IN_IGNORED with the rest of a full queue; the
        // contract then says "re-list every watched directory", and that re-list fails.
        REQUIRE(gone_a <= 1);
        if (gone_a == 0) {
            REQUIRE_FALSE(list_names(a));
        }
    } else {
        REQUIRE(gone_a == 1); // at most once per handle, and it does arrive
    }
    REQUIRE(after_gone_a == 0); // the handle is dead afterwards
    WARN("gone: by_move=" << by_move << " events=" << events.size() << " overflow=" << overflow
                          << " gone_a=" << gone_a);
    REQUIRE(gone_b == 0);
    REQUIRE(model_b.names() == must_list(b));

    watcher->remove(ha); // no-op on a gone handle

    // b still delivers; a new watch works too.
    const Path c = tmp / "c";
    std::filesystem::create_directory(c);
    const WatchHandle hc = add_watch(*watcher, c);
    REQUIRE(hc != ha);
    REQUIRE(hc != hb);
    REQUIRE(touch(b / "still-b"));
    REQUIRE(touch(c / "new-c"));
    events.clear();
    drain(*watcher, events, milliseconds(100));
    const auto has = [&](WatchHandle h, const char* name) {
        return std::any_of(events.begin(), events.end(), [&](const WatchEvent& e) {
            return e.kind == WatchEventKind::Created && e.handle == h && e.name == name;
        });
    };
    REQUIRE(has(hb, "still-b"));
    REQUIRE(has(hc, "new-c"));
    REQUIRE(std::none_of(events.begin(), events.end(),
                         [&](const WatchEvent& e) { return e.handle == ha; }));

    // Re-creating `a` and watching it again yields a fresh handle.
    std::filesystem::create_directory(a);
    const WatchHandle ha2 = add_watch(*watcher, a);
    REQUIRE(ha2 != ha);
    watcher->remove(ha2);
    watcher->remove(hb);
    watcher->remove(hc);
}
