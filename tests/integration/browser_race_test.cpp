// BrowserModel under concurrency, against the real kernel (owner: qa). Headless: the model, its
// ListingWorker and WatchWorker run without SDL or a window.
//
// - Rapid navigation (and history, sort, hidden, cursor commands) while a writer thread mutates
//   the folders: stale generations never commit, invariants hold after every step, the final
//   listing matches a fresh one.
// - Watch-driven incremental updates during a mutation storm (creates, deletes, writes, renames
//   inside the folder and across folders), also with Overflow injected and with tiny
//   incremental batches: the patched list ends equal to a fresh listing.
// - Selection, cursor and anchor survive re-lists, re-sorts, hidden toggles, Overflow and watch
//   updates by entry name.
// - The current folder being deleted: notice, the browser stays, Refresh/Up/Back behave.
// - [!mayfail] cases document real bugs found in Phase 2 (docs/qa-phase2.md).
//
// Files are created only inside fixture directories (CLAUDE.md rule 9). The writer thread never
// calls Catch2; it only touches the filesystem and a few atomics.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <navigator/commands.hpp>
#include <navigator/fs.hpp>
#include <navigator/watch.hpp>

#include "browser/browser_model.hpp"
#include "browser_support.hpp"
#include "fixture.hpp"

using namespace navigator;
using namespace navigator::integration_test;
using navigator::ui::BrowserModel;
using navigator::ui::kNoRow;
using navigator::ui::NoticeKind;
using namespace std::chrono_literals;

namespace {

// Files and folders the writer plays with. Mixed case, digits, a non-ASCII name and hidden names
// exercise the natural, case-insensitive sort and the hidden filter.
const std::vector<std::string>& file_pool() {
    static const std::vector<std::string> pool = {
        "a 1.txt", "A 2.txt", "b9.log", "b10.log", "\xc3\x87izelge.md", "zeta",
        ".gizli",  ".cfg",    "Notes",  "notes 2", "x.tar.gz",          "y.TXT"};
    return pool;
}
const std::vector<std::string>& dir_pool() {
    static const std::vector<std::string> pool = {"dir A", "dir b", ".hdir", "D10", "d9"};
    return pool;
}

// Mutates `dirs` until `stop`; raw syscalls, every error ignored (races are the point).
class Writer {
public:
    Writer(std::vector<Path> dirs, std::uint32_t seed) : dirs_(std::move(dirs)), seed_(seed) {}
    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;
    Writer(Writer&&) = delete;
    Writer& operator=(Writer&&) = delete;
    ~Writer() { stop(); }

    void start() {
        thread_ = std::thread([this] { run(); });
    }
    void stop() {
        stop_.store(true);
        if (thread_.joinable()) {
            thread_.join();
        }
    }
    [[nodiscard]] long ops() const noexcept { return ops_.load(); }

private:
    void run() {
        std::mt19937 rng(seed_);
        const auto pick = [&rng](std::size_t n) {
            return std::uniform_int_distribution<std::size_t>(0, n - 1)(rng);
        };
        const std::vector<std::string>& files = file_pool();
        const std::vector<std::string>& folders = dir_pool();
        std::string payload(512, 'x');
        while (!stop_.load(std::memory_order_relaxed)) {
            const Path& dir = dirs_[pick(dirs_.size())];
            const Path file = dir / files[pick(files.size())];
            const Path folder = dir / folders[pick(folders.size())];
            switch (pick(9)) {
            case 0:
            case 1: { // create or truncate + write
                const int fd = ::open(file.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
                if (fd >= 0) {
                    (void)!::write(fd, payload.data(), pick(payload.size()));
                    ::close(fd);
                }
                break;
            }
            case 2: { // append (Modified)
                const int fd = ::open(file.c_str(), O_WRONLY | O_APPEND | O_CLOEXEC);
                if (fd >= 0) {
                    (void)!::write(fd, payload.data(), 1 + pick(64));
                    ::close(fd);
                }
                break;
            }
            case 3: (void)::unlink(file.c_str()); break;
            case 4: // rename inside the folder (Renamed, possibly over an existing name)
                (void)::rename(file.c_str(), (dir / files[pick(files.size())]).c_str());
                break;
            case 5: { // move to another folder (MovedFrom here, MovedTo there)
                const Path& other = dirs_[pick(dirs_.size())];
                (void)::rename(file.c_str(), (other / file.filename()).c_str());
                break;
            }
            case 6: (void)::mkdir(folder.c_str(), 0755); break;
            case 7: (void)::rmdir(folder.c_str()); break;
            case 8: // rename a folder (empty folders only exist here)
                (void)::rename(folder.c_str(), (dir / folders[pick(folders.size())]).c_str());
                break;
            default: break;
            }
            ops_.fetch_add(1, std::memory_order_relaxed);
            if (pick(8) == 0) {
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
        }
    }

    std::vector<Path> dirs_;
    std::uint32_t seed_;
    std::atomic<bool> stop_{false};
    std::atomic<long> ops_{0};
    std::thread thread_;
};

WatcherFactory inotify_factory() {
    return [] { return create_inotify_watcher(); };
}

void settle(BrowserModel& m) {
    REQUIRE(m.wait_listing(kBrowserWait));
    (void)m.frame_start();
    REQUIRE_FALSE(m.busy());
}

// Waits until `pred` holds, pumping frame_start; false on timeout.
template <class Pred> bool pump_until(BrowserModel& m, Pred pred, std::chrono::milliseconds limit) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (!pred()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        (void)m.wait_listing(kBrowserWait);
        (void)m.wait_watch_batch(50ms);
        (void)m.frame_start();
    }
    return true;
}

// The current notice; an empty Notice (and a failed check) when there is none.
ui::Notice notice_of(const BrowserModel& m) {
    if (const std::optional<ui::Notice>& n = m.notice(); n.has_value()) {
        return *n;
    }
    FAIL_CHECK("expected a notice");
    return {};
}

void expect_matches_fresh_listing(const BrowserModel& m) {
    const FreshView fresh = fresh_view(m.current_dir(), m.show_hidden());
    REQUIRE(fresh.ok);
    CHECK(model_rows(m) == fresh.rows);
    CHECK(m.hidden_count() == fresh.hidden);
    CHECK(invariant_violation(m).empty());
}

void seed_folder(const Path& dir) {
    std::filesystem::create_directories(dir);
    for (const char* name : {"a 1.txt", "b9.log", ".gizli", "zeta"}) {
        (void)touch(dir / name);
    }
    std::filesystem::create_directories(dir / "dir A");
}

} // namespace

TEST_CASE("Back-to-back navigations commit only the newest request", "[browser][race]") {
    TempDir t;
    std::vector<Path> dirs;
    for (int i = 0; i < 6; ++i) {
        dirs.push_back(t / ("d" + std::to_string(i)));
        seed_folder(dirs.back());
        (void)touch(dirs.back() / ("only-in-" + std::to_string(i)));
    }
    BrowserModel m(BrowserModel::Options{});
    m.open_start(t.path());
    settle(m);

    std::mt19937 rng(7);
    Path last = t.path();
    for (int round = 0; round < 60; ++round) {
        // A burst of requests to distinct folders without waiting; frame_start() between them
        // may see results of earlier generations (dropped) or, rarely, commit one of the burst.
        const std::size_t burst = 1 + (static_cast<std::size_t>(round) % 5);
        std::vector<Path> targets;
        while (targets.size() < burst) {
            const Path& d =
                dirs[std::uniform_int_distribution<std::size_t>(0, dirs.size() - 1)(rng)];
            if (d != last && std::find(targets.begin(), targets.end(), d) == targets.end()) {
                targets.push_back(d);
            }
        }
        const std::size_t depth = m.back_stack().size();
        for (std::size_t k = 0; k < burst; ++k) {
            m.navigate_to(targets[k]);
            if (k % 2 == 1) {
                (void)m.frame_start();
            }
        }
        settle(m);
        (void)m.frame_start(); // a late stale result must not move us
        last = targets.back();
        REQUIRE(m.current_dir() == last);
        REQUIRE(m.back_stack().size() > depth);
        REQUIRE(m.back_stack().size() <= depth + burst);
        REQUIRE_FALSE(m.can_forward());
        const std::string marker = "only-in-" + last.filename().string().substr(1); // d3 -> 3
        REQUIRE(m.find(marker) != kNoRow);
        REQUIRE(invariant_violation(m).empty());
    }
}

TEST_CASE("Rapid navigation while a writer mutates the folders", "[browser][race]") {
    TempDir t;
    std::vector<Path> dirs;
    for (int i = 0; i < 4; ++i) {
        dirs.push_back(t / ("d" + std::to_string(i)));
        seed_folder(dirs.back());
    }
    NoSpawnLauncher launcher;
    std::atomic<int> wakes{0};
    BrowserModel::Options o;
    o.watcher_factory = inotify_factory();
    o.launcher = &launcher;
    o.wake = [&wakes] { wakes.fetch_add(1, std::memory_order_relaxed); };
    BrowserModel m(std::move(o));
    m.open_start(dirs[0]);
    settle(m);

    Writer writer(dirs, 42);
    writer.start();

    std::mt19937 rng(1234);
    const auto pick = [&rng](std::size_t n) {
        return std::uniform_int_distribution<std::size_t>(0, n - 1)(rng);
    };
    std::string violation;
    int steps = 0;
    const auto until = std::chrono::steady_clock::now() + 1500ms;
    while (std::chrono::steady_clock::now() < until && violation.empty()) {
        switch (pick(16)) {
        case 0:
        case 1:
        case 2: m.navigate_to(dirs[pick(dirs.size())]); break;
        case 3: m.back(); break;
        case 4: m.forward(); break;
        case 5:
            if (m.current_dir() != t.path()) {
                m.up(); // never above the fixture root
            }
            break;
        case 6: m.refresh(); break;
        case 7: m.toggle_hidden(); break;
        case 8: m.sort_by(static_cast<fs::SortKey>(pick(4))); break;
        case 9: m.cursor_by(static_cast<std::ptrdiff_t>(pick(5)) - 2, pick(2) == 0); break;
        case 10: m.select_all(); break;
        case 11:
            if (!m.entries().empty()) {
                m.click_row(pick(m.entries().size()),
                            pick(2) == 0 ? Modifiers::Ctrl : Modifiers::Shift);
            }
            break;
        case 12:
            // Files only (fake launcher). Entering the writer's folders can fail when an rmdir
            // wins the race, and a failed navigation trips BUG-P2-5 (sort/hidden change lost);
            // that path has its own deterministic [!mayfail] case below.
            if (m.cursor() < m.entries().size() &&
                !fs::is_directory_like(m.entries()[m.cursor()])) {
                m.activate();
            }
            break;
        case 13: m.cursor_end(true); break;
        default: break;
        }
        violation = invariant_violation(m);
        if (violation.empty() && pick(2) == 0) {
            if (pick(3) == 0) {
                (void)m.wait_listing(1ms);
            }
            (void)m.frame_start();
            violation = invariant_violation(m);
        }
        if (violation.empty() && m.loaded() &&
            !m.current_dir().string().starts_with(t.path().string())) {
            violation = "left the fixture: " + m.current_dir().string();
        }
        ++steps;
    }
    writer.stop();
    INFO("violation: " << violation);
    INFO("steps " << steps << ", writer ops " << writer.ops() << ", wakes " << wakes.load());
    REQUIRE(violation.empty());
    REQUIRE(steps > 100);
    REQUIRE(writer.ops() > 100);

    // Writer stopped: let the last request land (navigate_to(current) while a navigation is
    // pending trips BUG-P2-4), go to a known folder, let the watch settle, re-list, compare.
    REQUIRE(quiesce(m));
    m.navigate_to(dirs[2]);
    REQUIRE(quiesce(m));
    REQUIRE(m.wait_watch_ready(kBrowserWait));
    m.refresh();
    REQUIRE(quiesce(m));
    CHECK(m.current_dir() == dirs[2]);
    CHECK_FALSE(m.listing_error());
    expect_matches_fresh_listing(m);
    for (const platform::LaunchRequest& r : launcher.requests) {
        REQUIRE_FALSE(r.argv.empty());
        CHECK(r.argv.back().find(t.path().string()) == 0); // only fixture paths
    }
}

TEST_CASE("Watch-driven updates match a fresh listing after a mutation storm",
          "[browser][race][watch]") {
    struct Config {
        unsigned drop_percent;
        std::size_t max_incremental;
        bool show_hidden;
    };
    const Config cfg = GENERATE(Config{0, 256, false}, Config{0, 256, true}, Config{15, 256, false},
                                Config{0, 3, true});
    INFO("drop " << cfg.drop_percent << "% max_incremental " << cfg.max_incremental << " hidden "
                 << cfg.show_hidden);
    TempDir t;
    const Path watched = t / "watched";
    const Path outside = t / "outside"; // unwatched sibling: moves give MovedFrom / MovedTo
    seed_folder(watched);
    seed_folder(outside);

    auto overflows = std::make_shared<std::atomic<int>>(0);
    BrowserModel::Options o;
    o.max_incremental_changes = cfg.max_incremental;
    o.watcher_factory = [cfg, overflows]() -> Result<std::unique_ptr<IWatcher>> {
        Result<std::unique_ptr<IWatcher>> inner = create_inotify_watcher();
        if (!inner) {
            return inner;
        }
        return std::unique_ptr<IWatcher>(
            std::make_unique<LossyWatcher>(std::move(*inner), cfg.drop_percent, 99, overflows));
    };
    BrowserModel m(std::move(o));
    m.open_start(watched);
    settle(m);
    if (cfg.show_hidden) {
        m.toggle_hidden();
        settle(m);
    }
    REQUIRE(m.wait_watch_ready(kBrowserWait));
    m.refresh(); // listing taken with the watch in place
    settle(m);

    Writer writer({watched, outside}, 2024);
    writer.start();
    std::mt19937 rng(5);
    std::string violation;
    const auto until = std::chrono::steady_clock::now() + 1500ms;
    while (std::chrono::steady_clock::now() < until && violation.empty()) {
        (void)m.wait_watch_batch(5ms);
        (void)m.frame_start();
        violation = invariant_violation(m);
        switch (std::uniform_int_distribution<int>(0, 7)(rng)) {
        case 0: m.cursor_by(1, true); break;
        case 1: m.cursor_by(-1, false); break;
        case 2:
            if (!m.entries().empty()) {
                m.click_row(m.entries().size() / 2, Modifiers::Ctrl);
            }
            break;
        default: break;
        }
        if (violation.empty()) {
            violation = invariant_violation(m);
        }
    }
    writer.stop();
    INFO("writer ops " << writer.ops() << ", injected overflows " << overflows->load());
    REQUIRE(violation.empty());
    REQUIRE(quiesce(m));
    CHECK(m.current_dir() == watched);
    CHECK_FALSE(m.notice());
    expect_matches_fresh_listing(m);
    if (cfg.drop_percent > 0) {
        CHECK(overflows->load() > 0);
    }
}

TEST_CASE("Selection, cursor and anchor survive re-lists and watch updates by name",
          "[browser][race][watch]") {
    TempDir t;
    const Path dir = t / "sel";
    std::filesystem::create_directories(dir);
    for (const char* name : {"keep-1", "keep-2", "keep-3", "keep-4", "keep-5", "other-1", "other-2",
                             "other-3", ".hidden"}) {
        (void)touch(dir / name);
    }
    BrowserModel::Options o;
    o.watcher_factory = inotify_factory();
    BrowserModel m(std::move(o));
    m.open_start(dir);
    settle(m);
    REQUIRE(m.wait_watch_ready(kBrowserWait));
    m.refresh();
    settle(m);

    m.click_row(m.find("keep-1"), Modifiers::None);
    m.click_row(m.find("keep-5"), Modifiers::Ctrl);
    m.click_row(m.find("keep-3"), Modifiers::Ctrl); // cursor and anchor: keep-3
    std::set<std::string> want = {"keep-1", "keep-3", "keep-5"};
    REQUIRE(selected_set(m) == want);

    const auto expect_state = [&](const char* step) {
        INFO("after " << step);
        CHECK(selected_set(m) == want);
        CHECK(cursor_entry(m) == "keep-3");
        CHECK(m.anchor() == m.cursor());
        CHECK(invariant_violation(m).empty());
    };

    m.refresh();
    settle(m);
    expect_state("refresh");

    // Watch updates: a name sorted before everything, a delete, a rename.
    (void)touch(dir / "aaa-new");
    std::filesystem::remove(dir / "other-2");
    // An unselected entry: a rename the kernel splits across reads arrives as Remove + Upsert
    // and drops a selected entry's flag (BUG-P2-6, own [!mayfail] case below).
    std::filesystem::rename(dir / "other-3", dir / "zz-other");
    REQUIRE(pump_until(
        m,
        [&] {
            return m.find("aaa-new") != kNoRow && m.find("other-2") == kNoRow &&
                   m.find("zz-other") != kNoRow && m.find("other-3") == kNoRow;
        },
        kBrowserWait));
    expect_state("watch create/delete/rename");

    m.sort_by(fs::SortKey::Size);
    settle(m);
    expect_state("sort by size");
    m.sort_by(fs::SortKey::Name);
    settle(m);
    m.sort_by(fs::SortKey::Name); // descending
    settle(m);
    expect_state("sort by name, descending");
    m.toggle_hidden();
    settle(m);
    m.toggle_hidden();
    settle(m);
    expect_state("hidden on/off");

    ui::WatchBatch overflow;
    overflow.overflow = true;
    (void)m.apply_watch_batch(overflow);
    REQUIRE(m.busy());
    settle(m);
    expect_state("overflow re-list");

    // Noise storm on other names while refreshing repeatedly: the selection never drifts.
    std::atomic<bool> stop{false};
    std::thread noise([&] {
        int i = 0;
        while (!stop.load()) {
            const Path p = dir / ("noise-" + std::to_string(i % 17));
            if (i % 3 == 2) {
                (void)::unlink(p.c_str());
            } else {
                (void)touch(p);
            }
            ++i;
        }
    });
    for (int i = 0; i < 25; ++i) {
        m.refresh();
        (void)m.wait_listing(kBrowserWait);
        (void)m.wait_watch_batch(2ms);
        (void)m.frame_start();
        if (selected_set(m) != want || cursor_entry(m) != "keep-3") {
            break;
        }
    }
    stop.store(true);
    noise.join();
    REQUIRE(quiesce(m));
    expect_state("noise storm with refreshes");

    std::filesystem::remove(dir / "keep-1");
    want = {"keep-3", "keep-5"};
    REQUIRE(pump_until(m, [&] { return m.find("keep-1") == kNoRow; }, kBrowserWait));
    expect_state("selected entry deleted");
    expect_matches_fresh_listing(m);
}

TEST_CASE("Deleting the current folder raises a notice and the browser stays",
          "[browser][race][watch]") {
    TempDir t;
    const Path victim = t / "victim";
    std::filesystem::create_directories(victim / "inner");
    (void)touch(victim / "f1");
    (void)touch(victim / "f2");
    (void)touch(t / "sibling");
    BrowserModel::Options o;
    o.watcher_factory = inotify_factory();
    BrowserModel m(std::move(o));
    m.open_start(t.path());
    settle(m);
    m.navigate_to(victim);
    settle(m);
    REQUIRE(m.wait_watch_ready(kBrowserWait));
    m.refresh();
    settle(m);
    m.select_all();

    std::filesystem::remove_all(victim);
    REQUIRE(pump_until(
        m, [&] { return m.notice().has_value() && notice_of(m).kind == NoticeKind::FolderGone; },
        kBrowserWait));
    CHECK(notice_of(m).path == victim);
    CHECK(m.current_dir() == victim);
    CHECK(m.can_back());
    CHECK(invariant_violation(m).empty());
    REQUIRE(quiesce(m));
    CHECK(m.current_dir() == victim);

    m.execute(Command{CommandId::Refresh, 0});
    settle(m);
    REQUIRE(m.notice());
    CHECK(notice_of(m).kind == NoticeKind::RefreshFailed);
    CHECK(notice_of(m).code == ErrorCode::NotFound);
    CHECK(m.current_dir() == victim);
    CHECK(invariant_violation(m).empty());

    m.execute(Command{CommandId::NavigateUp, 0});
    settle(m);
    CHECK(m.current_dir() == t.path());
    CHECK_FALSE(m.notice());
    CHECK(m.find("victim") == kNoRow);
    CHECK(m.find("sibling") != kNoRow);

    // Back into the deleted folder fails and keeps history.
    const std::size_t back_depth = m.back_stack().size();
    m.execute(Command{CommandId::NavigateBack, 0});
    settle(m);
    CHECK(m.current_dir() == t.path());
    REQUIRE(m.notice());
    CHECK(notice_of(m).kind == NoticeKind::OpenFolderFailed);
    CHECK(m.back_stack().size() == back_depth);
}

TEST_CASE("A folder deleted while its navigation is pending fails cleanly", "[browser][race]") {
    TempDir t;
    std::filesystem::create_directories(t / "doomed");
    (void)touch(t / "stay");
    BrowserModel::Options o;
    o.watcher_factory = inotify_factory();
    BrowserModel m(std::move(o));
    m.open_start(t.path());
    settle(m);
    for (int i = 0; i < 50; ++i) {
        std::filesystem::create_directories(t / "doomed");
        m.navigate_to(t / "doomed");
        std::filesystem::remove(t / "doomed"); // races the listing
        settle(m);
        REQUIRE(invariant_violation(m).empty());
        if (m.current_dir() != t.path()) {
            // The listing won the race: the folder is shown (empty) and its watch reports Gone.
            CHECK(m.current_dir() == t / "doomed");
            m.up();
            settle(m);
        } else {
            REQUIRE(m.notice());
            CHECK(notice_of(m).kind == NoticeKind::OpenFolderFailed);
        }
        REQUIRE(m.current_dir() == t.path());
    }
    // The rmdir of "doomed" happens while the watch is on "doomed", so after a failed
    // navigation the parent may still list it (BUG-P2-2, seen here under ctest -j8). Re-list
    // before comparing; BUG-P2-2 has its own deterministic case.
    REQUIRE(quiesce(m));
    m.refresh();
    REQUIRE(quiesce(m));
    expect_matches_fresh_listing(m);
}

// ---------------------------------------------------------------------------------------------
// Bugs found in Phase 2 (docs/qa-phase2.md). Each fails today; [!mayfail] keeps the suite green
// until the ui fix lands, then the tag comes off.
// ---------------------------------------------------------------------------------------------

TEST_CASE("BUG-P2-1: changes made before the new folder's watch is added are lost",
          "[browser][watch][bug][!mayfail]") {
    // The WatchWorker applies retarget() only after its current wait() times out (up to
    // kPollInterval = 100 ms), while the listing of a small folder takes about 1 ms. A change in
    // that window is in neither the listing nor any watch batch. The gate makes the window
    // deterministic; in the real app it is "anything that changes during the first ~100 ms".
    TempDir t;
    const Path a = t / "a";
    const Path b = t / "b";
    std::filesystem::create_directories(a);
    std::filesystem::create_directories(b);
    (void)touch(b / "before");
    auto gate = std::make_shared<WatchGate>();
    BrowserModel::Options o;
    o.watcher_factory = [gate]() -> Result<std::unique_ptr<IWatcher>> {
        Result<std::unique_ptr<IWatcher>> inner = create_inotify_watcher();
        if (!inner) {
            return inner;
        }
        return std::unique_ptr<IWatcher>(std::make_unique<GatedWatcher>(std::move(*inner), gate));
    };
    BrowserModel m(std::move(o));
    m.open_start(a);
    settle(m);
    REQUIRE(m.wait_watch_ready(kBrowserWait));

    gate->close();
    m.navigate_to(b);
    settle(m); // listing committed; the watch on b is not in place yet
    REQUIRE(m.find("before") != kNoRow);
    REQUIRE(gate->wait_blocked(kBrowserWait));
    (void)touch(b / "during-gap");
    gate->release();
    REQUIRE(m.wait_watch_ready(kBrowserWait));
    REQUIRE(quiesce(m));
    CHECK(m.find("during-gap") != kNoRow); // fails: the entry never shows up
}

TEST_CASE("BUG-P2-2: a failed navigation loses changes made meanwhile in the current folder",
          "[browser][watch][bug][!mayfail]") {
    if (::geteuid() == 0) {
        SKIP("running as root: permissions are not enforced");
    }
    // navigate_to() moves the watch to the target before listing it. When the listing fails,
    // fail_request() moves the watch back but does not re-list, so changes to the current folder
    // while the watch was away are never shown.
    TempDir t;
    std::filesystem::create_directories(t / "locked");
    ::chmod((t / "locked").c_str(), 0);
    (void)touch(t / "old");
    BrowserModel::Options o;
    o.watcher_factory = inotify_factory();
    BrowserModel m(std::move(o));
    m.open_start(t.path());
    settle(m);
    REQUIRE(m.wait_watch_ready(kBrowserWait));

    m.navigate_to(t / "locked");
    REQUIRE(m.wait_watch_ready(kBrowserWait)); // watch moved off the current folder
    (void)touch(t / "made-while-away");
    settle(m); // listing failed: notice, watch moved back
    REQUIRE(m.notice());
    CHECK(m.current_dir() == t.path());
    REQUIRE(m.wait_watch_ready(kBrowserWait));
    REQUIRE(quiesce(m));
    ::chmod((t / "locked").c_str(), 0755);
    CHECK(m.find("made-while-away") != kNoRow); // fails
}

TEST_CASE("BUG-P2-3: Refresh after the folder was deleted and recreated never re-watches it",
          "[browser][watch][bug][!mayfail]") {
    // WatchedDirGone invalidates the worker's handle. Refresh of the same path keeps the old
    // watch tag and never calls retarget(), so after `rm -rf dir && mkdir dir` + F5 the list is
    // right once but live updates have stopped until the user navigates away and back.
    TempDir t;
    const Path dir = t / "build";
    std::filesystem::create_directories(dir);
    (void)touch(dir / "old.o");
    BrowserModel::Options o;
    o.watcher_factory = inotify_factory();
    BrowserModel m(std::move(o));
    m.open_start(dir);
    settle(m);
    REQUIRE(m.wait_watch_ready(kBrowserWait));

    std::filesystem::remove_all(dir);
    REQUIRE(pump_until(m, [&] { return m.notice().has_value(); }, kBrowserWait));
    std::filesystem::create_directories(dir);
    (void)touch(dir / "fresh.o");
    m.refresh();
    settle(m);
    REQUIRE(m.find("fresh.o") != kNoRow);
    REQUIRE(m.wait_watch_ready(kBrowserWait));
    (void)touch(dir / "live.o");
    (void)pump_until(m, [&] { return m.find("live.o") != kNoRow; }, 2000ms);
    CHECK(m.find("live.o") != kNoRow); // fails: no watch on the recreated folder
}

TEST_CASE("BUG-P2-4: navigate_to(current folder) while another navigation is pending",
          "[browser][bug][!mayfail]") {
    // navigate_to(current_dir) becomes refresh(); request() folds a Refresh into the pending
    // navigation, so the newest request (stay here) loses and the older one (go to b) commits.
    TempDir t;
    std::filesystem::create_directories(t / "a");
    std::filesystem::create_directories(t / "b");
    BrowserModel m(BrowserModel::Options{});
    m.open_start(t / "a");
    settle(m);
    m.navigate_to(t / "b");
    m.navigate_to(t / "a"); // the user changed their mind before b was listed
    settle(m);
    CHECK(m.current_dir() == t / "a"); // fails: b
}

TEST_CASE("BUG-P2-5: a sort or hidden toggle during a navigation that fails is never applied",
          "[browser][bug][!mayfail]") {
    // request() folds a Relist (sort_by, toggle_hidden) into a pending navigation. When that
    // navigation fails, fail_request() keeps the old entries, but sort() / show_hidden() already
    // changed: the list contradicts the header and the toggle until the next re-list, and later
    // watch inserts binary-search a list that is not in sort() order.
    TempDir t;
    (void)touch(t / ".hidden");
    (void)touch(t / "a-small");
    {
        const int fd = ::open((t / "b-big").c_str(), O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
        REQUIRE(fd >= 0);
        REQUIRE(::write(fd, "0123456789", 10) == 10);
        ::close(fd);
    }
    BrowserModel m(BrowserModel::Options{});
    m.open_start(t.path());
    settle(m);
    REQUIRE(entry_names(m) == std::vector<std::string>{"a-small", "b-big"});

    m.navigate_to(t / "missing"); // fails with NotFound
    m.toggle_hidden();
    m.sort_by(fs::SortKey::Size); // descending: b-big first
    settle(m);
    REQUIRE(m.notice());
    CHECK(m.current_dir() == t.path());
    REQUIRE(m.show_hidden());
    REQUIRE(m.sort().key == fs::SortKey::Size);
    CHECK(m.find(".hidden") != kNoRow); // fails: still filtered
    CHECK(cursor_entry(m).empty() == false);
    CHECK(invariant_violation(m).empty()); // fails: "not sorted at row 0"
}

// Delivers every paired Renamed as MovedFrom + MovedTo, which is what the inotify backend reports
// when the kernel's two rename events straddle a read() (watch.hpp: an unpaired MovedFrom is never
// held for a later poll()).
class SplitRenameWatcher final : public IWatcher {
public:
    explicit SplitRenameWatcher(std::unique_ptr<IWatcher> inner) : inner_(std::move(inner)) {}
    Result<WatchHandle> add(const Path& dir) override { return inner_->add(dir); }
    void remove(WatchHandle handle) noexcept override { inner_->remove(handle); }
    Result<std::size_t> poll(std::vector<WatchEvent>& out) override {
        std::vector<WatchEvent> got;
        Result<std::size_t> n = inner_->poll(got);
        if (!n) {
            return n;
        }
        const std::size_t before = out.size();
        for (WatchEvent& ev : got) {
            if (ev.kind == WatchEventKind::Renamed) {
                WatchEvent from = ev;
                from.kind = WatchEventKind::MovedFrom;
                from.name = ev.old_name;
                from.old_name.clear();
                ev.kind = WatchEventKind::MovedTo;
                ev.old_name.clear();
                out.push_back(std::move(from));
            }
            out.push_back(std::move(ev));
        }
        return out.size() - before;
    }
    bool wait(std::chrono::milliseconds timeout) override { return inner_->wait(timeout); }
    [[nodiscard]] int native_handle() const noexcept override { return inner_->native_handle(); }

private:
    std::unique_ptr<IWatcher> inner_;
};

TEST_CASE("BUG-P2-6: a rename reported as MovedFrom + MovedTo drops the selection and cursor",
          "[browser][watch][bug][!mayfail]") {
    // Seen under `ctest -j8`: the selection test lost "kept-5" from the selection after
    // rename(keep-5, kept-5). The batch holds Remove(keep-5) then Upsert(kept-5), and nothing
    // links them, although the Upsert's fresh stat has the same FileId (st_dev/st_ino) as the
    // removed row. Pairing by FileId within a batch would carry selection, cursor and anchor.
    TempDir t;
    for (const char* name : {"a", "b", "c"}) {
        (void)touch(t / name);
    }
    BrowserModel::Options o;
    o.watcher_factory = []() -> Result<std::unique_ptr<IWatcher>> {
        Result<std::unique_ptr<IWatcher>> inner = create_inotify_watcher();
        if (!inner) {
            return inner;
        }
        return std::unique_ptr<IWatcher>(std::make_unique<SplitRenameWatcher>(std::move(*inner)));
    };
    BrowserModel m(std::move(o));
    m.open_start(t.path());
    settle(m);
    REQUIRE(m.wait_watch_ready(kBrowserWait));
    m.click_row(m.find("a"), Modifiers::None);
    m.click_row(m.find("b"), Modifiers::Ctrl); // selection {a, b}, cursor b
    std::filesystem::rename(t / "b", t / "renamed");
    REQUIRE(pump_until(m, [&] { return m.find("renamed") != kNoRow; }, kBrowserWait));
    CHECK(cursor_entry(m) == "renamed");                             // fails
    CHECK(selected_set(m) == std::set<std::string>{"a", "renamed"}); // fails: {a}
}
