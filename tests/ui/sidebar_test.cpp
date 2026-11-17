// Phase 4 sidebar: PlacesModel (stores, injected volumes, Missing probe, persistence wiring with
// a temporary config dir), sidebar rows (sections, highlight, offline / missing, collapsed
// sections, empty states), keyboard focus helpers, the F6 cycle including the sidebar, and
// recents recorded on navigation commits only. Files only inside mkdtemp fixtures.
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <navigator/i18n.hpp>
#include <navigator/layout.hpp>
#include <navigator/places.hpp>
#include <navigator/volume.hpp>

#include "browser/browser_model.hpp"
#include "sidebar/places_model.hpp"
#include "sidebar/sidebar_model.hpp"
#include "ui_fixture.hpp"
#include "workspace/workspace.hpp"

using namespace navigator;
using namespace navigator::ui;
using navigator::test::TempDir;

namespace {

constexpr std::chrono::milliseconds kWait{5000};

Timestamp now() {
    return std::chrono::time_point_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now());
}

VolumeInfo volume(const Path& mount, VolumeKind kind, const std::string& uuid,
                  std::string label = {}) {
    VolumeInfo v;
    v.mount_point = mount;
    v.kind = kind;
    v.device = "/dev/sd" + (uuid.empty() ? std::string("x") : uuid.substr(0, 1));
    if (!uuid.empty()) {
        v.uuid = uuid;
    }
    v.root = "/";
    v.label = std::move(label);
    v.removable = kind == VolumeKind::Removable;
    v.has_space = true;
    v.space.total_bytes = 1000;
    v.space.free_bytes = 400;
    v.space.available_bytes = 400;
    return v;
}

PlacesModel::Options options(const TempDir& t, const Path& config = {}) {
    PlacesModel::Options o;
    o.home = t.path() / "home";
    o.user_dirs.downloads = t.path() / "home" / "Downloads";
    o.user_dirs.music = t.path() / "home" / "Music"; // does not exist: hidden after the probe
    o.config_root = config;
    o.restore = !config.empty();
    o.persist = !config.empty();
    o.probe_worker = true;
    return o;
}

void probe(PlacesModel& m) {
    (void)m.frame_start(); // posts the probe
    REQUIRE(m.wait_probe(kWait));
    (void)m.frame_start(); // applies it
}

const SidebarRow* find_row(const std::vector<SidebarRow>& rows, SidebarRowKind kind,
                           const std::string& label) {
    for (const SidebarRow& r : rows) {
        if (r.kind == kind && r.label == label) {
            return &r;
        }
    }
    return nullptr;
}

std::size_t count_kind(const std::vector<SidebarRow>& rows, SidebarRowKind kind) {
    std::size_t n = 0;
    for (const SidebarRow& r : rows) {
        n += r.kind == kind ? 1 : 0;
    }
    return n;
}

} // namespace

TEST_CASE("Sidebar rows: sections from volumes, bookmarks, recents and places", "[sidebar]") {
    set_language(Language::English);
    TempDir t;
    t.dir("home/Downloads/isos");
    t.dir("home/work");
    PlacesModel m(options(t));
    m.set_volumes({volume("/", VolumeKind::Root, "r"),
                   volume("/media/u/STICK", VolumeKind::Removable, "5A1C", "STICK"),
                   volume("/mnt/nas", VolumeKind::Network, "")});
    REQUIRE(m.add_bookmark(t.path() / "home/work", now()));
    m.record_visit(t.path() / "home/Downloads/isos", now(), 0.0);
    probe(m);

    const std::vector<SidebarRow> rows = build_sidebar_rows(m, layout::SidebarState{}, Path());
    REQUIRE(rows.front().kind == SidebarRowKind::Header);
    CHECK(rows.front().section == layout::SidebarSection::Bookmarks);
    CHECK(count_kind(rows, SidebarRowKind::Header) == 4);
    CHECK(find_row(rows, SidebarRowKind::Bookmark, "work") != nullptr);
    CHECK(find_row(rows, SidebarRowKind::Recent, "isos") != nullptr);
    // Places: Home, Downloads, File System; Music is configured but missing -> hidden.
    CHECK(find_row(rows, SidebarRowKind::Place, "Home") != nullptr);
    CHECK(find_row(rows, SidebarRowKind::Place, "Downloads") != nullptr);
    CHECK(find_row(rows, SidebarRowKind::Place, "Music") == nullptr);
    const SidebarRow* fsys = find_row(rows, SidebarRowKind::Place, "File System");
    REQUIRE(fsys != nullptr);
    REQUIRE(fsys->usage.has_value()); // Root's usage on the File System row
    CHECK(fsys->usage.value_or(-1.0) == used_fraction(volume("/", VolumeKind::Root, "r").space));
    // Storage: the stick (label, removable) and the share (device name, network); not Root.
    const SidebarRow* stick = find_row(rows, SidebarRowKind::Volume, "STICK");
    REQUIRE(stick != nullptr);
    CHECK(stick->removable);
    CHECK(stick->usage.has_value());
    const SidebarRow* nas = find_row(rows, SidebarRowKind::Volume, "/dev/sdx");
    REQUIRE(nas != nullptr);
    CHECK(nas->network);
    CHECK(count_kind(rows, SidebarRowKind::Volume) == 2);
}

TEST_CASE("Sidebar highlight: the deepest visible row containing the active folder", "[sidebar]") {
    TempDir t;
    t.dir("home/Downloads/isos/deep");
    PlacesModel m(options(t));
    m.set_volumes({volume("/", VolumeKind::Root, "r")});
    REQUIRE(m.add_bookmark(t.path() / "home/Downloads/isos", now()));
    probe(m);
    std::vector<SidebarRow> rows =
        build_sidebar_rows(m, layout::SidebarState{}, t.path() / "home/Downloads/isos/deep");
    std::size_t lit = 0;
    for (const SidebarRow& r : rows) {
        if (r.highlighted) {
            ++lit;
            CHECK(r.kind == SidebarRowKind::Bookmark); // deeper than Downloads and Home
        }
    }
    CHECK(lit == 1);
    // Collapsing Bookmarks hides the row: the next deepest (Downloads) wins.
    layout::SidebarState st;
    st.set_collapsed(layout::SidebarSection::Bookmarks, true);
    rows = build_sidebar_rows(m, st, t.path() / "home/Downloads/isos/deep");
    const SidebarRow* dl = find_row(rows, SidebarRowKind::Place, "Downloads");
    REQUIRE(dl != nullptr);
    CHECK(dl->highlighted);
    CHECK(count_kind(rows, SidebarRowKind::Bookmark) == 0);
    // Outside everything but "/": File System.
    rows = build_sidebar_rows(m, layout::SidebarState{}, Path("/usr/share"));
    const SidebarRow* fs = find_row(rows, SidebarRowKind::Place, "File System");
    REQUIRE(fs != nullptr);
    CHECK(fs->highlighted);
}

TEST_CASE("Bookmark on an unmounted volume stays listed and shows offline", "[sidebar]") {
    TempDir t;
    const Path cfg = t.dir("cfg");
    {
        PlacesModel m(options(t, cfg));
        const VolumeInfo stick = volume("/media/u/STICK", VolumeKind::Removable, "5A1C", "STICK");
        m.set_volumes({volume("/", VolumeKind::Root, "r"), stick});
        REQUIRE(m.add_bookmark("/media/u/STICK/photos", now()));
        m.final_save();
    }
    // Next start: the stick is not plugged in.
    PlacesModel m(options(t, cfg));
    m.load();
    m.set_volumes({volume("/", VolumeKind::Root, "r")});
    probe(m);
    std::vector<SidebarRow> rows = build_sidebar_rows(m, layout::SidebarState{}, Path("/"));
    const SidebarRow* b = find_row(rows, SidebarRowKind::Bookmark, "photos");
    REQUIRE(b != nullptr);
    CHECK(b->status == places::LocationStatus::Offline);
    CHECK_FALSE(b->highlighted);
    // It comes back at another mount point: online again, rebased.
    m.set_volumes({volume("/", VolumeKind::Root, "r"),
                   volume("/media/u/STICK1", VolumeKind::Removable, "5A1C", "STICK")});
    rows = build_sidebar_rows(m, layout::SidebarState{}, Path("/"));
    b = find_row(rows, SidebarRowKind::Bookmark, "photos");
    REQUIRE(b != nullptr);
    CHECK(b->status != places::LocationStatus::Offline);
    CHECK(b->path == Path("/media/u/STICK1/photos"));
}

TEST_CASE("Missing bookmarks are marked by the probe, never removed", "[sidebar]") {
    TempDir t;
    PlacesModel m(options(t));
    m.set_volumes({volume("/", VolumeKind::Root, "r")});
    REQUIRE(m.add_bookmark(t.path() / "gone", now()));
    probe(m);
    const std::vector<SidebarRow> rows = build_sidebar_rows(m, layout::SidebarState{}, Path());
    const SidebarRow* b = find_row(rows, SidebarRowKind::Bookmark, "gone");
    REQUIRE(b != nullptr);
    CHECK(b->status == places::LocationStatus::Missing);
    CHECK(m.bookmarks().size() == 1);
}

TEST_CASE("Empty Bookmarks / Recents show hint rows; collapsed sections have none", "[sidebar]") {
    TempDir t;
    PlacesModel m(options(t));
    std::vector<SidebarRow> rows = build_sidebar_rows(m, layout::SidebarState{}, Path());
    CHECK(count_kind(rows, SidebarRowKind::Empty) == 2);
    CHECK_FALSE(rows[1].focusable());
    layout::SidebarState st;
    st.set_collapsed(layout::SidebarSection::Bookmarks, true);
    st.set_collapsed(layout::SidebarSection::Recents, true);
    rows = build_sidebar_rows(m, st, Path());
    CHECK(count_kind(rows, SidebarRowKind::Empty) == 0);
    CHECK(rows[0].collapsed);
    // Tracking off: the Recents hint says so.
    (void)m.toggle_tracking(0.0);
    rows = build_sidebar_rows(m, layout::SidebarState{}, Path());
    bool off = false;
    for (const SidebarRow& r : rows) {
        off = off || (r.kind == SidebarRowKind::Empty && r.tracking_off);
    }
    CHECK(off);
}

TEST_CASE("Recents show at most eight rows, newest first", "[sidebar]") {
    TempDir t;
    PlacesModel m(options(t));
    for (int i = 0; i < 12; ++i) {
        m.record_visit(t.dir("r" + std::to_string(i)), now(), 0.0);
    }
    const std::vector<SidebarRow> rows = build_sidebar_rows(m, layout::SidebarState{}, Path());
    CHECK(count_kind(rows, SidebarRowKind::Recent) == kMaxRecentRows);
    const SidebarRow* first = nullptr;
    for (const SidebarRow& r : rows) {
        if (r.kind == SidebarRowKind::Recent) {
            first = &r;
            break;
        }
    }
    REQUIRE(first != nullptr);
    CHECK(first->label == "r11");
}

TEST_CASE("Bookmark edits: toggle, rename, move, reorder index", "[sidebar]") {
    TempDir t;
    PlacesModel m(options(t));
    const Path a = t.dir("a");
    const Path b = t.dir("b");
    CHECK(m.toggle_bookmark(a, now()) == PlacesModel::Toggled::Added);
    CHECK(m.toggle_bookmark(b, now()) == PlacesModel::Toggled::Added);
    CHECK(m.toggle_bookmark(a, now()) == PlacesModel::Toggled::Removed);
    REQUIRE(m.toggle_bookmark(a, now()) == PlacesModel::Toggled::Added); // now b, a
    const places::BookmarkId ida = m.bookmarks().find(a)->id;
    CHECK(m.move_bookmark(ida, -1));
    CHECK(m.bookmarks().list()[0].path == a);
    CHECK_FALSE(m.move_bookmark(ida, -1)); // no wrap
    REQUIRE(m.rename_bookmark(ida, "Alpha"));
    std::vector<SidebarRow> rows = build_sidebar_rows(m, layout::SidebarState{}, Path());
    const std::size_t row = sidebar_row_of(rows, ida);
    REQUIRE(row < rows.size());
    CHECK(rows[row].label == "Alpha");
    CHECK(sidebar_bookmark_index(rows, row) == 0);
    CHECK(sidebar_bookmark_index(rows, row + 1) == 1);
}

TEST_CASE("Sidebar keyboard focus skips hint rows and clamps", "[sidebar]") {
    TempDir t;
    PlacesModel m(options(t));
    const std::vector<SidebarRow> rows = build_sidebar_rows(m, layout::SidebarState{}, Path());
    // Row 0 Bookmarks header, row 1 empty hint, row 2 Recents header.
    CHECK(sidebar_focus_step(rows, 0, 1) == 2);
    CHECK(sidebar_focus_step(rows, 2, -1) == 0);
    CHECK(sidebar_focus_step(rows, 0, -1) == 0);
    constexpr auto npos = static_cast<std::size_t>(-1);
    CHECK(sidebar_focus_step(rows, npos, 1) == 0);
    CHECK(sidebar_focus_step(rows, npos, -1) == rows.size() - 1);
    CHECK(sidebar_initial_focus(rows) == 0);
}

TEST_CASE("Keyboard owner: one focus ring at a time", "[sidebar]") {
    // Design review Phase 4 B1: the list draws its cursor ring only when it owns the keyboard.
    CHECK(keyboard_owner(false, false, false) == KeyboardOwner::List);
    CHECK(keyboard_owner(false, false, true) == KeyboardOwner::Sidebar);
    CHECK(keyboard_owner(false, true, false) == KeyboardOwner::TextField); // address bar
    CHECK(keyboard_owner(false, true, true) == KeyboardOwner::TextField);  // bookmark rename
    CHECK(keyboard_owner(true, false, true) == KeyboardOwner::Popup);      // GoTo, menus
    CHECK(keyboard_owner(true, true, false) == KeyboardOwner::Popup);
}

TEST_CASE("Network volumes are named like GNOME: share on host", "[sidebar]") {
    set_language(Language::English);
    VolumeInfo v;
    v.kind = VolumeKind::Network;
    v.mount_point = "/mnt/nas";
    v.device = "nas.local:/export/media/";
    CHECK(volume_label(v) == "media on nas.local");
    v.device = "//server/share";
    CHECK(volume_label(v) == "share on server");
    v.device = "weird";
    CHECK(volume_label(v) == "weird");
    v.label = "NAS";
    CHECK(volume_label(v) == "NAS");
    set_language(Language::Turkish);
    v.label.clear();
    v.device = "nas.local:/export/media";
    CHECK(volume_label(v) == "nas.local üzerinde media");
    set_language(Language::English);
}

TEST_CASE("F6 cycles sidebar -> panes -> sidebar", "[sidebar]") {
    // Two panes, sidebar visible: pane 0 -> pane 1 -> sidebar -> pane 0.
    CHECK(cycle_focus(2, true, false, 0, 1) == FocusStop{false, 1});
    CHECK(cycle_focus(2, true, false, 1, 1).sidebar);
    CHECK(cycle_focus(2, true, true, 1, 1) == FocusStop{false, 0});
    // Shift+F6 from the sidebar goes to the last pane; from pane 0 to the sidebar.
    CHECK(cycle_focus(2, true, true, 0, -1) == FocusStop{false, 1});
    CHECK(cycle_focus(2, true, false, 0, -1).sidebar);
    // Hidden sidebar: the Phase 3 cycle.
    CHECK(cycle_focus(2, false, false, 1, 1) == FocusStop{false, 0});
    CHECK(cycle_focus(1, false, false, 0, 1) == FocusStop{false, 0});
    // Icon-only threshold.
    CHECK(sidebar_icon_only(900.0F));
    CHECK_FALSE(sidebar_icon_only(1366.0F));
}

TEST_CASE("Collapsed sections persist in the session JSON", "[sidebar]") {
    layout::Session s;
    s.sidebar.width = 300.0;
    s.sidebar.set_collapsed(layout::SidebarSection::Recents, true);
    const Result<std::string> json = layout::to_json(s);
    REQUIRE(json);
    const Result<layout::Session> back = layout::session_from_json(*json);
    REQUIRE(back);
    CHECK(back->sidebar == s.sidebar);
}

TEST_CASE("Persistence: bookmarks saved on edit, recents debounced, nothing without persist",
          "[sidebar]") {
    TempDir t;
    const Path cfg = t.path() / "cfg";
    {
        PlacesModel m(options(t, cfg));
        REQUIRE(m.add_bookmark(t.dir("a"), now()));
        m.record_visit(t.dir("b"), now(), 1000.0);
        m.tick(1500.0); // debounce not due
        // The bookmark write is on its way; recents not yet.
        m.final_save(); // quit: both written synchronously
    }
    CHECK(std::filesystem::exists(places::bookmarks_file(cfg)));
    CHECK(std::filesystem::exists(places::recents_file(cfg)));
    PlacesModel again(options(t, cfg));
    again.load();
    CHECK(again.bookmarks().size() == 1);
    CHECK(again.recents().size() == 1);

    // restore = persist = false (headless runs, --no-session): nothing read or written.
    const Path other = t.path() / "cfg2";
    PlacesModel::Options o = options(t, other);
    o.restore = false;
    o.persist = false;
    {
        PlacesModel m(o);
        m.load();
        REQUIRE(m.add_bookmark(t.dir("c"), now()));
        m.final_save();
    }
    CHECK_FALSE(std::filesystem::exists(other));
}

TEST_CASE("Recents are recorded on navigation commits only", "[sidebar]") {
    TempDir t;
    t.dir("a/b");
    Workspace::Options wo;
    wo.home = t.path();
    Workspace ws(wo);
    layout::Session session;
    layout::TabState st;
    st.path = t.path() / "a";
    session.layout = layout::Layout(st);
    ws.start(session, Path());
    const auto settle = [&] {
        for (int i = 0; i < 4; ++i) {
            for (const layout::TabId tab : ws.visible_tabs()) {
                REQUIRE(ws.model(tab)->wait_listing(kWait));
            }
            (void)ws.frame_start();
        }
    };
    settle();
    CHECK(ws.take_visits().empty()); // session restore does not record
    ws.navigate_active(t.path() / "a" / "b");
    settle();
    std::vector<Path> v = ws.take_visits();
    REQUIRE(v.size() == 1);
    CHECK(v[0] == t.path() / "a" / "b");
    REQUIRE(ws.execute(Command{CommandId::Refresh, 0}));
    settle();
    CHECK(ws.take_visits().empty()); // refresh does not record
    REQUIRE(ws.execute(Command{CommandId::NavigateUp, 0}));
    settle();
    CHECK(ws.take_visits().size() == 1);
    // A new tab records its first listing.
    REQUIRE(ws.open_tab(t.path() / "a" / "b", true));
    settle();
    CHECK(ws.take_visits().size() == 1);
}
