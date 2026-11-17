// Bookmarks and recents end to end (owner: qa): ui::PlacesModel with its real VolumeWorker
// (IVolumeMonitor over a fake, thread-safe mount source and fake sysfs/devfs trees), its real
// probe worker (fs::stat on temporary folders), the bookmarks / recents JobWriters and the files
// in a temporary config dir, across three "restarts" (a new PlacesModel on the same dir).
//
//   - unmount: entries on the volume turn Offline and are never dropped (nor Missing: offline
//     locations are not probed); the files keep them; edits while offline save and reload;
//   - remount at another mount point: Online again, paths rebased, the new paths saved; a recent
//     entry that lands on an existing entry's path is merged (summed visit count);
//   - network volumes are never probed (a folder that does not exist on a share stays Online),
//     while a deleted folder on a local volume turns Missing.
// Plus the three config-dir writers (bookmarks on edit, recents debounced, session debounced)
// racing while the UI thread mutates everything: no torn or invalid file for readers, each file
// == its last snapshot, the quit flush writes all three, no temp files left.
//
// Nothing outside the mkdtemp fixture is read or written (CLAUDE.md rule 9).
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <navigator/layout.hpp>
#include <navigator/places.hpp>
#include <navigator/volume.hpp>

#include "browser_support.hpp"
#include "fixture.hpp"
#include "sidebar/places_model.hpp"
#include "sidebar/sidebar_model.hpp"
#include "workspace/session_writer.hpp"
#include "workspace/workspace.hpp"

using namespace navigator;
using namespace navigator::integration_test;
using navigator::places::LocationStatus;
using navigator::ui::PlacesModel;

namespace {

constexpr std::chrono::milliseconds kWait{5000};

Timestamp wall_now() {
    return std::chrono::time_point_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now());
}

std::string read_file(const Path& file) {
    std::ifstream in(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::set<std::string> dir_names(const Path& dir) {
    std::set<std::string> out;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        out.insert(e.path().filename().string());
    }
    return out;
}

// The volume worker reads the table on its own thread; the test thread swaps it.
class LockedMountSource final : public IMountSource {
public:
    explicit LockedMountSource(std::string text) : text_(std::move(text)) {}
    Result<std::string> read_mountinfo() override {
        const std::scoped_lock lock(mutex_);
        return text_;
    }
    Result<SpaceInfo> query_space(const Path& mount_point) override {
        (void)mount_point;
        return SpaceInfo{1000, 400, 300};
    }
    void set(std::string text) {
        const std::scoped_lock lock(mutex_);
        text_ = std::move(text);
    }

private:
    std::mutex mutex_;
    std::string text_;
};

// A fake machine: "/" on nvme, a USB stick (uuid 1234-ABCD) at media/STICK, an NFS share at
// net/share. Mount points are real folders inside the fixture so the probe can stat them.
struct Machine {
    TempDir t;
    Path stick;
    Path stick2;
    Path share;
    Path parked; // where the stick's files go while it is "unmounted"
    Path cfg;
    std::shared_ptr<LockedMountSource> source;

    Machine()
        : stick(t / "media" / "STICK"), stick2(t / "media" / "STICK2"), share(t / "net" / "share"),
          parked(t / "parked"), cfg(t / "cfg") {
        std::filesystem::create_directories(stick / "data");
        std::filesystem::create_directories(stick / "other");
        std::filesystem::create_directories(share); // share/x is never created
        std::filesystem::create_directories(t / "home");
        std::filesystem::create_directories(t / "local" / "kept");
        std::filesystem::create_directories(t / "sys" / "block");
        std::filesystem::create_directories(t / "dev" / "disk" / "by-uuid");
        std::filesystem::create_directories(t / "dev" / "disk" / "by-label");
        std::filesystem::create_symlink("../../sdb1", t / "dev" / "disk" / "by-uuid" / "1234-ABCD");
        std::filesystem::create_symlink("../../sdb1", t / "dev" / "disk" / "by-label" / "STICK");
        source = std::make_shared<LockedMountSource>(table(true, false, true));
    }

    [[nodiscard]] std::string table(bool stick_at_1, bool stick_at_2, bool nfs) const {
        std::string s = "22 1 259:2 / / rw - ext4 /dev/nvme0n1p2 rw\n"
                        "23 22 0:5 / /proc rw - proc proc rw\n";
        if (stick_at_1) {
            s += "29 22 8:17 / " + stick.string() + " rw - vfat /dev/sdb1 rw\n";
        }
        if (stick_at_2) {
            s += "31 22 8:17 / " + stick2.string() + " rw - vfat /dev/sdb1 rw\n";
        }
        if (nfs) {
            s += "30 22 0:50 / " + share.string() + " rw - nfs4 server:/export rw,vers=4.2\n";
        }
        return s;
    }

    [[nodiscard]] PlacesModel::Options options() const {
        PlacesModel::Options o;
        o.home = t / "home";
        o.config_root = cfg;
        o.restore = true;
        o.persist = true;
        o.volume_worker = true;
        o.volume_query.source = source;
        o.volume_query.sysfs_root = t / "sys";
        o.volume_query.devfs_root = t / "dev";
        o.volume_query.home_dir = t / "home";
        o.probe_worker = true;
        return o;
    }
};

bool has_volume(const PlacesModel& m, const Path& mount_point) {
    return std::any_of(m.volumes().begin(), m.volumes().end(),
                       [&](const VolumeInfo& v) { return v.mount_point == mount_point; });
}

// Applies volume snapshots until `done` holds (the worker polls every 250 ms).
template <typename Pred> void wait_volumes_until(PlacesModel& m, Pred done) {
    const auto deadline = std::chrono::steady_clock::now() + kWait;
    (void)m.frame_start();
    while (!done(m)) {
        REQUIRE(std::chrono::steady_clock::now() < deadline);
        (void)m.wait_volumes(std::chrono::milliseconds(300));
        (void)m.frame_start();
    }
}

void probe(PlacesModel& m) {
    (void)m.frame_start(); // posts the probe if anything changed
    REQUIRE(m.wait_probe(kWait));
    (void)m.frame_start(); // applies it
    REQUIRE_FALSE(m.probe_pending());
}

LocationStatus bookmark_status(const PlacesModel& m, const Path& stored,
                               Path* effective = nullptr) {
    const places::Bookmark* b = m.bookmarks().find(stored);
    REQUIRE(b != nullptr);
    return m.status(b->path, b->volume, effective);
}

LocationStatus recent_status(const PlacesModel& m, const Path& stored) {
    const places::RecentEntry* r = m.recents().find(stored);
    REQUIRE(r != nullptr);
    return m.status(r->path, r->volume);
}

const ui::SidebarRow* row_for(const std::vector<ui::SidebarRow>& rows, ui::SidebarRowKind kind,
                              const Path& stored) {
    for (const ui::SidebarRow& r : rows) {
        if (r.kind == kind && r.stored_path == stored) {
            return &r;
        }
    }
    return nullptr;
}

} // namespace

TEST_CASE("Bookmarks and recents survive an unmount, restarts and a remount elsewhere",
          "[places][e2e][volume]") {
    Machine mc;
    const Path data = mc.stick / "data";
    const Path other = mc.stick / "other";
    const Path on_share = mc.share / "x";
    const Path gone = mc.t / "local" / "gone";
    const Path kept = mc.t / "local" / "kept";
    const std::string renamed = "USB verileri — ğüşöç İı";

    // ---- Run 1: the stick is mounted ----------------------------------------------------------
    {
        PlacesModel m(mc.options());
        m.load();
        wait_volumes_until(m, [&](const PlacesModel& pm) { return has_volume(pm, mc.stick); });
        CHECK(has_volume(m, mc.share));
        std::filesystem::create_directories(gone);
        for (const Path& p : {data, on_share, gone, kept}) {
            INFO(p);
            REQUIRE(m.add_bookmark(p, wall_now()));
        }
        std::filesystem::remove(gone); // deleted after bookmarking: Missing, never removed
        for (const Path& p : {data, other, on_share}) {
            m.record_visit(p, wall_now(), 0.0);
        }
        const places::Bookmark* b = m.bookmarks().find(data);
        REQUIRE(b != nullptr);
        REQUIRE(b->volume.has_value());
        CHECK(b->volume.value_or(VolumeIdentity{}).uuid == "1234-ABCD");
        CHECK(b->volume.value_or(VolumeIdentity{}).mount_point == mc.stick);
        const places::Bookmark* n = m.bookmarks().find(on_share);
        REQUIRE(n != nullptr);
        REQUIRE(n->volume.has_value());
        CHECK(n->volume.value_or(VolumeIdentity{}).uuid.empty());
        CHECK(n->volume.value_or(VolumeIdentity{}).device == "server:/export");
        CHECK_FALSE(m.bookmarks().find(kept)->volume.has_value()); // "/" is untracked
        probe(m);
        CHECK(bookmark_status(m, data) == LocationStatus::Online);
        CHECK(bookmark_status(m, kept) == LocationStatus::Online);
        CHECK(bookmark_status(m, gone) == LocationStatus::Missing);
        // share/x does not exist, but a network volume is never probed: Online.
        CHECK(bookmark_status(m, on_share) == LocationStatus::Online);
        CHECK(recent_status(m, on_share) == LocationStatus::Online);

        // Unmount: the stick's folder disappears with it.
        std::filesystem::rename(mc.stick, mc.parked);
        mc.source->set(mc.table(false, false, true));
        wait_volumes_until(m, [&](const PlacesModel& pm) { return !has_volume(pm, mc.stick); });
        probe(m);
        CHECK(m.bookmarks().size() == 4);
        CHECK(m.recents().size() == 3);
        CHECK(bookmark_status(m, data) == LocationStatus::Offline); // not Missing
        CHECK(recent_status(m, data) == LocationStatus::Offline);
        CHECK(recent_status(m, other) == LocationStatus::Offline);
        CHECK(bookmark_status(m, on_share) == LocationStatus::Online);
        CHECK(bookmark_status(m, gone) == LocationStatus::Missing);
        const std::vector<ui::SidebarRow> rows =
            ui::build_sidebar_rows(m, layout::SidebarState{}, kept);
        const ui::SidebarRow* row = row_for(rows, ui::SidebarRowKind::Bookmark, data);
        REQUIRE(row != nullptr);
        CHECK(row->status == LocationStatus::Offline);
        CHECK_FALSE(row->highlighted);
        CHECK(row_for(rows, ui::SidebarRowKind::Bookmark, kept)->highlighted);
        for (const ui::SidebarRow& r : rows) {
            CHECK_FALSE((r.kind == ui::SidebarRowKind::Volume && r.path == mc.stick));
        }
        m.final_save();
    }
    {
        const Result<places::BookmarkStore> b =
            places::load_bookmarks(places::bookmarks_file(mc.cfg));
        REQUIRE(b);
        CHECK(b->size() == 4);
        REQUIRE(b->find(data) != nullptr);
        CHECK(b->find(data)->volume.value_or(VolumeIdentity{}).uuid == "1234-ABCD");
        CHECK(b->find(gone) != nullptr);
        const Result<places::RecentsStore> r = places::load_recents(places::recents_file(mc.cfg));
        REQUIRE(r);
        CHECK(r->size() == 3);
    }

    // ---- Run 2: restart while the stick is unmounted; edits save and reload -------------------
    std::filesystem::create_directories(mc.stick2 / "data"); // a plain folder on "/" for now
    {
        PlacesModel m(mc.options());
        m.load();
        wait_volumes_until(m, [&](const PlacesModel& pm) { return pm.volumes_ready(); });
        probe(m);
        REQUIRE(m.bookmarks().size() == 4);
        CHECK(bookmark_status(m, data) == LocationStatus::Offline);
        CHECK(recent_status(m, other) == LocationStatus::Offline);
        CHECK(bookmark_status(m, gone) == LocationStatus::Missing);
        const places::BookmarkId id = m.bookmarks().find(data)->id;
        REQUIRE(m.rename_bookmark(id, renamed));
        CHECK(m.move_bookmark(id, +1));
        // A visit to the folder the stick will be mounted over (untracked "/" volume).
        m.record_visit(mc.stick2 / "data", wall_now(), 100.0);
        m.tick(100.0 + ui::SaveScheduler::kDebounceMs); // debounced recents save
        m.final_save();
    }
    {
        const Result<places::BookmarkStore> b =
            places::load_bookmarks(places::bookmarks_file(mc.cfg));
        REQUIRE(b);
        REQUIRE(b->find(data) != nullptr);
        CHECK(b->find(data)->name == renamed);
        CHECK(b->index_of(b->find(data)->id) == 1U);
        CHECK(b->find(data)->volume.value_or(VolumeIdentity{}).mount_point ==
              mc.stick); // untouched while offline
        const Result<places::RecentsStore> r = places::load_recents(places::recents_file(mc.cfg));
        REQUIRE(r);
        CHECK(r->size() == 4);
        CHECK(r->entries()[0].path == mc.stick2 / "data");
    }

    // ---- Run 3: the stick comes back at media/STICK2 ------------------------------------------
    std::filesystem::remove_all(mc.stick2);
    std::filesystem::rename(mc.parked, mc.stick2);
    mc.source->set(mc.table(false, true, true));
    {
        PlacesModel m(mc.options());
        m.load();
        wait_volumes_until(m, [&](const PlacesModel& pm) { return has_volume(pm, mc.stick2); });
        probe(m);
        // Rebased and saved; name and order kept.
        CHECK(m.bookmarks().find(data) == nullptr);
        const places::Bookmark* b = m.bookmarks().find(mc.stick2 / "data");
        REQUIRE(b != nullptr);
        CHECK(b->name == renamed);
        CHECK(b->volume.value_or(VolumeIdentity{}).mount_point == mc.stick2);
        CHECK(m.bookmarks().index_of(b->id) == 1U);
        CHECK(bookmark_status(m, mc.stick2 / "data") == LocationStatus::Online);
        CHECK(m.bookmarks().size() == 4);
        // Recents: STICK/data rebased onto the existing STICK2/data entry and merged.
        CHECK(m.recents().find(data) == nullptr);
        CHECK(m.recents().find(other) == nullptr);
        const places::RecentEntry* r = m.recents().find(mc.stick2 / "data");
        REQUIRE(r != nullptr);
        CHECK(r->visit_count == 2);
        CHECK(m.recents().entries()[0].path == mc.stick2 / "data"); // the newer entry's place
        CHECK(m.recents().find(mc.stick2 / "other") != nullptr);
        CHECK(recent_status(m, mc.stick2 / "other") == LocationStatus::Online);
        CHECK(m.recents().size() == 3);
        CHECK(bookmark_status(m, on_share) == LocationStatus::Online);
        CHECK(bookmark_status(m, gone) == LocationStatus::Missing);
        // The share goes away: Offline (identity by device + mount point), still listed.
        mc.source->set(mc.table(false, true, false));
        wait_volumes_until(m, [&](const PlacesModel& pm) { return !has_volume(pm, mc.share); });
        CHECK(bookmark_status(m, on_share) == LocationStatus::Offline);
        CHECK(m.bookmarks().size() == 4);
        m.final_save();
    }
    {
        const Result<places::BookmarkStore> b =
            places::load_bookmarks(places::bookmarks_file(mc.cfg));
        REQUIRE(b);
        CHECK(b->size() == 4);
        CHECK(b->find(mc.stick2 / "data") != nullptr);
        CHECK(b->find(data) == nullptr);
        const Result<places::RecentsStore> r = places::load_recents(places::recents_file(mc.cfg));
        REQUIRE(r);
        CHECK(r->size() == 3);
        REQUIRE(r->find(mc.stick2 / "data") != nullptr);
        CHECK(r->find(mc.stick2 / "data")->visit_count == 2);
    }
    CHECK(dir_names(mc.cfg) == std::set<std::string>{"bookmarks.json", "recents.json"});
}

TEST_CASE("Bookmark, recents and session writers race into one config dir without tearing",
          "[places][e2e][race]") {
    TempDir t;
    for (int d = 1; d <= 6; ++d) {
        std::filesystem::create_directories(t / ("d" + std::to_string(d)) / "sub");
        (void)touch(t / ("d" + std::to_string(d)) / "a.txt");
    }
    std::filesystem::create_directories(t / "media" / "STICK" / "photos");
    const Path cfg = t / "cfg";
    std::filesystem::create_directories(cfg);
    const Path bfile = places::bookmarks_file(cfg);
    const Path rfile = places::recents_file(cfg);
    const Path sfile = layout::session_file(cfg);

    struct ReaderStats {
        std::uint64_t reads = 0;
        std::uint64_t invalid = 0;
        std::string first_invalid;
    };
    ReaderStats stats[3];
    const Path files[3] = {bfile, rfile, sfile};
    std::vector<std::jthread> readers;
    readers.reserve(3);
    for (int i = 0; i < 3; ++i) {
        readers.emplace_back([&st = stats[i], file = files[i], i](const std::stop_token& stop) {
            while (!stop.stop_requested()) {
                std::ifstream in(file, std::ios::binary);
                if (!in) {
                    continue; // not written yet
                }
                const std::string text{std::istreambuf_iterator<char>(in),
                                       std::istreambuf_iterator<char>()};
                ++st.reads;
                bool ok = text.size() >= 2 && text.compare(text.size() - 2, 2, "}\n") == 0;
                if (i == 0) {
                    ok = ok && places::bookmarks_from_json(text).has_value();
                } else if (i == 1) {
                    ok = ok && places::recents_from_json(text).has_value();
                } else {
                    const Result<layout::Session> s = layout::session_from_json(text);
                    ok = ok && s.has_value() && s->layout.validate().has_value();
                }
                if (!ok && st.invalid++ == 0) {
                    st.first_invalid = text.substr(0, 200);
                }
            }
        });
    }

    std::uint64_t session_submits = 0;
    std::uint64_t place_writes = 0;
    layout::Session last_session;
    std::string want_bookmarks;
    std::string want_recents;
    {
        NoSpawnLauncher launcher;
        ui::Workspace::Options wo;
        wo.model.launcher = &launcher;
        wo.home = t.path();
        wo.config_root = cfg;
        ui::Workspace ws(std::move(wo));
        ws.start(std::nullopt, t / "d1");
        ui::SessionWriter writer(sfile);
        ui::SaveScheduler scheduler;

        PlacesModel::Options po;
        po.home = t.path();
        po.config_root = cfg;
        po.persist = true;
        po.probe_worker = true; // one more thread in the mix
        PlacesModel places(po);
        VolumeInfo usb;
        usb.mount_point = t / "media" / "STICK";
        usb.device = "/dev/sdb1";
        usb.uuid = "1234-ABCD";
        usb.root = "/";
        usb.kind = VolumeKind::Removable;
        places.set_volumes({usb});

        std::mt19937 rng(20261117U);
        std::uniform_int_distribution<int> pick(0, 99);
        double now_ms = 0.0;
        std::uint64_t seen = ws.persist_serial();
        const auto folder = [&](int n) {
            return n == 6 ? t / "media" / "STICK" / "photos" : t / ("d" + std::to_string(n + 1));
        };
        for (int step = 0; step < 800; ++step) {
            const int p = pick(rng);
            const int f = p % 7;
            if (p < 25) {
                (void)places.toggle_bookmark(folder(f), wall_now());
            } else if (p < 35 && !places.bookmarks().empty()) {
                const places::Bookmark& b =
                    places.bookmarks()
                        .list()[static_cast<std::size_t>(p) % places.bookmarks().size()];
                (void)places.rename_bookmark(b.id, "Yer imi " + std::to_string(step) + " ğİı");
            } else if (p < 42 && !places.bookmarks().empty()) {
                (void)places.move_bookmark(places.bookmarks().list()[0].id, +1);
            } else if (p < 70) {
                ws.active_model().navigate_to(folder(f));
            } else if (p < 78) {
                (void)ws.execute(Command{p % 2 == 0 ? CommandId::NewTab : CommandId::CloseTab, 0});
            } else if (p < 80) {
                places.clear_recents(now_ms);
            } else if (p < 82) {
                (void)places.toggle_tracking(now_ms);
            } else {
                (void)ws.execute(Command{CommandId::NextTab, 0});
            }
            (void)ws.frame_start();
            (void)places.frame_start();
            for (const Path& v : ws.take_visits()) {
                places.record_visit(v, wall_now(), now_ms);
            }
            places.tick(now_ms);
            (void)ws.sync();
            if (ws.persist_serial() != seen) {
                seen = ws.persist_serial();
                scheduler.mark(now_ms);
            }
            now_ms += 150.0;
            if (scheduler.due(now_ms) || step % 4 == 0) {
                scheduler.taken();
                last_session = ws.snapshot();
                writer.submit(last_session);
                ++session_submits;
            }
        }
        // A visit inside the debounce window right before quit: only the quit flush writes it.
        if (!places.recents().options().enabled) {
            (void)places.toggle_tracking(now_ms);
        }
        places.record_visit(t / "d6" / "sub", wall_now(), now_ms);
        REQUIRE(places.save_remaining(now_ms).has_value());
        // Quit, as App does: places final_save, session flush + synchronous save.
        places.final_save();
        place_writes = places.writes();
        for (int round = 0; round < 3; ++round) {
            for (const layout::TabId tab : ws.visible_tabs()) {
                REQUIRE(ws.model(tab)->wait_listing(kBrowserWait));
            }
            (void)ws.frame_start();
        }
        (void)ws.sync();
        last_session = ws.snapshot();
        REQUIRE(writer.flush());
        REQUIRE(writer.save_now(last_session));
        const Result<std::string> b = places::bookmarks_to_json(places.bookmarks());
        const Result<std::string> r = places::recents_to_json(places.recents());
        REQUIRE(b);
        REQUIRE(r);
        want_bookmarks = *b;
        want_recents = *r;
        CHECK(places.recents().find(t / "d6" / "sub") != nullptr);
    }
    readers.clear(); // request_stop + join

    CHECK(session_submits > 100);
    CHECK(place_writes > 10);
    for (int i = 0; i < 3; ++i) {
        INFO(files[i] << ": first invalid read: " << stats[i].first_invalid);
        CHECK(stats[i].reads > 0);
        CHECK(stats[i].invalid == 0);
    }
    CHECK(read_file(bfile) == want_bookmarks);
    CHECK(read_file(rfile) == want_recents);
    const Result<std::string> s = layout::to_json(last_session);
    REQUIRE(s);
    CHECK(read_file(sfile) == *s);
    CHECK(dir_names(cfg) ==
          std::set<std::string>{"bookmarks.json", "recents.json", "session.json"});
}

TEST_CASE("PlacesModel without persist or restore never touches the config dir", "[places][e2e]") {
    // AppConfig::save_session = false / restore_session = false (and headless --frames without
    // --config-dir, where the App clears both): no file is read or written.
    TempDir t;
    const Path cfg = t / "cfg";
    std::filesystem::create_directories(cfg);
    { std::ofstream(places::bookmarks_file(cfg)) << "{not json"; }
    PlacesModel::Options po;
    po.home = t.path();
    po.config_root = cfg;
    po.restore = false;
    po.persist = false;
    po.probe_worker = false;
    {
        PlacesModel m(po);
        m.load();
        CHECK(m.bookmarks().empty());
        REQUIRE(m.add_bookmark(t.path(), wall_now()));
        m.record_visit(t.path(), wall_now(), 0.0);
        m.tick(1e9);
        m.final_save();
        CHECK(m.writes() == 0);
    }
    CHECK(dir_names(cfg) == std::set<std::string>{"bookmarks.json"}); // no .bad, no recents
    CHECK(read_file(places::bookmarks_file(cfg)) == "{not json");
}
