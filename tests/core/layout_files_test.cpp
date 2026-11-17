// navigator::layout session and named-layout files, all under a fresh temporary root:
// save/load, permissions, atomicity (no temporary files left), .bad move-aside, size cap,
// named layout CRUD, invalid names.
#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <sys/stat.h>

#include <navigator/layout.hpp>

#include "layout_test_util.hpp"
#include "temp_dir.hpp"

using namespace navigator;
using navigator::test::running_as_root;
using navigator::test::split_of;
using navigator::test::tab_at;
using navigator::test::TempDir;
namespace lay = navigator::layout;
namespace stdfs = std::filesystem;

namespace {

lay::Session sample_session() {
    lay::Session session;
    session.window.width = 1024;
    session.layout = lay::Layout(tab_at("/home/u"));
    const lay::PaneId a = session.layout.active_pane();
    const lay::PaneId b = session.layout.split_pane(a, lay::SplitEdge::Right, tab_at("/tmp"))->pane;
    REQUIRE(session.layout.split_pane(b, lay::SplitEdge::Bottom, tab_at("/srv")));
    REQUIRE(session.layout.add_tab(a, tab_at("/etc"), 1, false));
    session.closed_tabs.push(lay::ClosedTab{tab_at("/opt"), {}, 0});
    return session;
}

unsigned mode_of(const stdfs::path& path) {
    struct stat st {};
    REQUIRE(::stat(path.c_str(), &st) == 0);
    return static_cast<unsigned>(st.st_mode) & 0777U;
}

std::vector<std::string> names_in(const stdfs::path& dir) {
    std::vector<std::string> names;
    for (const auto& entry : stdfs::directory_iterator(dir)) {
        names.push_back(entry.path().filename().native());
    }
    std::sort(names.begin(), names.end());
    return names;
}

std::string read_all(const stdfs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void write_all(const stdfs::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

} // namespace

TEST_CASE("files: save and reopen a three-pane session", "[layout][files]") {
    const TempDir tmp;
    const stdfs::path root = tmp / "config" / "navigator"; // neither exists yet
    const stdfs::path file = lay::session_file(root);
    CHECK(file == root / "session.json");

    const Result<lay::Session> missing = lay::load_session(file);
    REQUIRE_FALSE(missing);
    CHECK(missing.error().code == ErrorCode::NotFound);

    const lay::Session session = sample_session();
    REQUIRE(lay::save_session(file, session));
    CHECK(mode_of(file) == 0600U);
    CHECK(mode_of(root) == 0700U);
    CHECK(mode_of(tmp / "config") == 0700U);
    CHECK(names_in(root) == std::vector<std::string>{"session.json"}); // no temp file left

    const Result<lay::Session> back = lay::load_session(file);
    REQUIRE(back);
    CHECK(back->layout.pane_count() == 3);
    CHECK(back->layout.tab_count() == 4);
    CHECK(back->window.width == 1024);
    CHECK(back->closed_tabs.size() == 1);
    CHECK(*lay::to_json(*back) == *lay::to_json(session));
    CHECK(read_all(file) == *lay::to_json(session));

    // Overwrite with a different session: replaced atomically, still no stray files.
    lay::Session other;
    other.layout = lay::Layout(tab_at("/only"));
    REQUIRE(lay::save_session(file, other));
    CHECK(lay::load_session(file)->layout.pane_count() == 1);
    CHECK(names_in(root) == std::vector<std::string>{"session.json"});
}

TEST_CASE("files: argument errors", "[layout][files]") {
    const TempDir tmp;
    CHECK(lay::load_session(stdfs::path()).error().code == ErrorCode::InvalidArgument);
    CHECK(lay::save_session(stdfs::path(), lay::Session{}).error().code ==
          ErrorCode::InvalidArgument);
    // A directory where the file should be.
    const stdfs::path dir = tmp.dir("session.json");
    CHECK(lay::load_session(dir).error().code == ErrorCode::IoError);
    CHECK_FALSE(lay::save_session(dir, lay::Session{}));
    // A regular file where a parent directory should be.
    const stdfs::path blocker = tmp.file("blocker");
    CHECK(lay::save_session(blocker / "session.json", lay::Session{}).error().code ==
          ErrorCode::IoError);
    CHECK(lay::load_session(blocker / "session.json").error().code == ErrorCode::NotFound);
}

TEST_CASE("files: a file that is not a JSON object is moved aside to .bad", "[layout][files]") {
    const TempDir tmp;
    const stdfs::path file = tmp / "session.json";
    const stdfs::path bad = tmp / "session.json.bad";
    write_all(bad, "older bad file");
    write_all(file, "{ this is not json");
    const Result<lay::Session> r = lay::load_session(file);
    REQUIRE_FALSE(r);
    CHECK(r.error().code == ErrorCode::InvalidArgument);
    CHECK_FALSE(stdfs::exists(file));
    CHECK(read_all(bad) == "{ this is not json"); // replaced the older one

    write_all(file, "[1, 2, 3]");
    CHECK(lay::load_session(file).error().code == ErrorCode::InvalidArgument);
    CHECK(read_all(bad) == "[1, 2, 3]");

    // A lenient-but-odd object is not bad: it loads and stays in place.
    write_all(file, R"({"layout": 5, "unknown": true})");
    REQUIRE(lay::load_session(file));
    CHECK(stdfs::exists(file));

    // The next save works normally.
    REQUIRE(lay::save_session(file, sample_session()));
    REQUIRE(lay::load_session(file));
}

TEST_CASE("files: oversized files are rejected without parsing or renaming", "[layout][files]") {
    const TempDir tmp;
    const stdfs::path file = tmp.file("session.json", lay::kMaxDocumentBytes + 1);
    const Result<lay::Session> r = lay::load_session(file);
    REQUIRE_FALSE(r);
    CHECK(r.error().code == ErrorCode::InvalidArgument);
    CHECK(stdfs::exists(file));
    CHECK_FALSE(stdfs::exists(tmp / "session.json.bad"));

    const stdfs::path layouts = tmp.dir("layouts");
    tmp.file("layouts/big.json", lay::kMaxDocumentBytes + 1);
    CHECK(lay::load_named_layout(layouts, "big").error().code == ErrorCode::InvalidArgument);

    // Exactly at the cap is read (and is then just not JSON).
    tmp.file("layouts/edge.json", lay::kMaxDocumentBytes);
    CHECK(lay::load_named_layout(layouts, "edge").error().code == ErrorCode::InvalidArgument);
    CHECK(stdfs::exists(layouts / "edge.json")); // named layouts are never renamed
}

TEST_CASE("files: permission errors leave the previous file untouched", "[layout][files]") {
    if (running_as_root()) {
        SKIP("permission checks do not apply to root");
    }
    const TempDir tmp;
    const stdfs::path dir = tmp.dir("ro");
    const stdfs::path file = dir / "session.json";
    REQUIRE(lay::save_session(file, sample_session()));
    const std::string before = read_all(file);
    REQUIRE(::chmod(dir.c_str(), 0500) == 0);
    lay::Session other;
    other.layout = lay::Layout(tab_at("/x"));
    const Result<void> r = lay::save_session(file, other);
    REQUIRE_FALSE(r);
    CHECK(r.error().code == ErrorCode::PermissionDenied);
    REQUIRE(::chmod(dir.c_str(), 0700) == 0);
    CHECK(read_all(file) == before);
    CHECK(names_in(dir) == std::vector<std::string>{"session.json"});

    REQUIRE(::chmod(file.c_str(), 0000) == 0);
    CHECK(lay::load_session(file).error().code == ErrorCode::PermissionDenied);
    REQUIRE(::chmod(file.c_str(), 0600) == 0);

    REQUIRE(::chmod(dir.c_str(), 0000) == 0);
    CHECK(lay::list_named_layouts(dir).error().code == ErrorCode::PermissionDenied);
    CHECK(lay::save_named_layout(dir / "sub", "x", other.layout).error().code ==
          ErrorCode::PermissionDenied);
    REQUIRE(::chmod(dir.c_str(), 0700) == 0);
}

TEST_CASE("files: concurrent saves never tear the file", "[layout][files]") {
    const TempDir tmp;
    const stdfs::path file = tmp / "session.json";
    std::vector<std::thread> writers;
    writers.reserve(4);
    for (int w = 0; w < 4; ++w) {
        writers.emplace_back([&file, w] {
            lay::Session session;
            session.layout = lay::Layout(tab_at("/writer" + std::to_string(w)));
            for (int i = 0; i < 25; ++i) {
                (void)lay::save_session(file, session);
            }
        });
    }
    for (std::thread& t : writers) {
        t.join();
    }
    const Result<lay::Session> back = lay::load_session(file);
    REQUIRE(back);
    CHECK(back->layout.tab_count() == 1);
    CHECK(names_in(tmp.path()) == std::vector<std::string>{"session.json"});
}

TEST_CASE("files: layout name validation", "[layout][files]") {
    for (const std::string& name :
         std::vector<std::string>{"a", "Three panes", "Çalışma Alanı", "İş 📁", "a.json", "x..y",
                                  std::string(64, 'n')}) {
        INFO(name);
        CHECK(lay::is_valid_layout_name(name));
    }
    for (const std::string& name : std::vector<std::string>{
             std::string(), "../x", "..", ".", ".hidden", "a/b", "/abs", "x/", " lead", "trail ",
             std::string(65, 'n'), std::string("nul\0x", 5), "tab\there", "new\nline",
             std::string("del\x7f"), std::string("bad\xff utf8"), std::string("\xc0\xaf"),
             std::string("\xed\xa0\x80")}) {
        INFO(name);
        CHECK_FALSE(lay::is_valid_layout_name(name));
    }
    // 64 bytes of multi-byte UTF-8 is fine, 65+ is not; a code point cut at the limit is invalid.
    CHECK(lay::is_valid_layout_name(std::string(32, 'x').append(16, 'x') + "ğğğğğğğğ"));
    CHECK_FALSE(lay::is_valid_layout_name(std::string(63, 'x') + "ğ"));
}

TEST_CASE("files: named layout save, list, load, delete", "[layout][files]") {
    const TempDir tmp;
    const stdfs::path dir = lay::layouts_dir(tmp / "navigator");
    CHECK(dir == tmp / "navigator" / "layouts");

    // Missing directory: empty list, not an error.
    const Result<std::vector<std::string>> none = lay::list_named_layouts(dir);
    REQUIRE(none);
    CHECK(none->empty());

    const lay::Session session = sample_session();
    REQUIRE(lay::save_named_layout(dir, "Çalışma", session.layout));
    REQUIRE(lay::save_named_layout(dir, "file10", lay::Layout(tab_at("/b"))));
    REQUIRE(lay::save_named_layout(dir, "file9", lay::Layout(tab_at("/c"))));
    REQUIRE(lay::save_named_layout(dir, "Alpha", lay::Layout(tab_at("/d"))));
    REQUIRE(lay::save_named_layout(dir, "alpha", lay::Layout(tab_at("/e"))));
    CHECK(mode_of(dir) == 0700U);
    CHECK(mode_of(dir / "file9.json") == 0600U);

    // Noise that must not be listed.
    tmp.file("navigator/layouts/notes.txt");
    tmp.file("navigator/layouts/.hidden.json");
    tmp.file("navigator/layouts/.json");
    tmp.dir("navigator/layouts/folder.json");
    tmp.link("navigator/layouts/linked.json", "file9.json");
    tmp.link("navigator/layouts/dangling.json", "missing.json");

    const Result<std::vector<std::string>> names = lay::list_named_layouts(dir);
    REQUIRE(names);
    CHECK(*names ==
          std::vector<std::string>{"Alpha", "alpha", "file9", "file10", "linked", "Çalışma"});

    const Result<lay::Layout> loaded = lay::load_named_layout(dir, "Çalışma");
    REQUIRE(loaded);
    CHECK(loaded->pane_count() == 3);
    CHECK(loaded->tab_count() == 4);
    CHECK(*lay::layout_to_json(*loaded) == *lay::layout_to_json(session.layout));

    // Overwrite replaces.
    REQUIRE(lay::save_named_layout(dir, "Çalışma", lay::Layout(tab_at("/new"))));
    CHECK(lay::load_named_layout(dir, "Çalışma")->tab_count() == 1);

    REQUIRE(lay::delete_named_layout(dir, "file9"));
    CHECK(lay::delete_named_layout(dir, "file9").error().code == ErrorCode::NotFound);
    CHECK(lay::load_named_layout(dir, "file9").error().code == ErrorCode::NotFound);
    CHECK(lay::load_named_layout(dir, "nothing").error().code == ErrorCode::NotFound);

    // Garbage in a named layout file: InvalidArgument and not renamed.
    write_all(dir / "broken.json", "nope");
    CHECK(lay::load_named_layout(dir, "broken").error().code == ErrorCode::InvalidArgument);
    CHECK(stdfs::exists(dir / "broken.json"));

    // No stray temporary files after all of the above.
    for (const std::string& name : names_in(dir)) {
        CHECK(name.find(".tmp-") == std::string::npos);
    }
}

TEST_CASE("files: invalid names never touch the filesystem", "[layout][files]") {
    const TempDir tmp;
    const stdfs::path dir = tmp.dir("layouts");
    const lay::Layout layout(tab_at("/a"));
    tmp.file("x.json");
    for (const std::string& name :
         std::vector<std::string>{"../x", "", "a/b", std::string(200, 'z'), ".x"}) {
        INFO(name);
        CHECK(lay::save_named_layout(dir, name, layout).error().code == ErrorCode::InvalidArgument);
        CHECK(lay::load_named_layout(dir, name).error().code == ErrorCode::InvalidArgument);
        CHECK(lay::delete_named_layout(dir, name).error().code == ErrorCode::InvalidArgument);
    }
    CHECK(stdfs::exists(tmp / "x.json")); // "../x" did not delete the sibling
    CHECK(names_in(dir).empty());
    CHECK(lay::save_named_layout(stdfs::path(), "ok", layout).error().code ==
          ErrorCode::InvalidArgument);
    CHECK(lay::load_named_layout(stdfs::path(), "ok").error().code == ErrorCode::InvalidArgument);
    CHECK(lay::delete_named_layout(stdfs::path(), "ok").error().code == ErrorCode::InvalidArgument);

    // A layouts "directory" that is a regular file.
    const stdfs::path file = tmp.file("not-a-dir");
    CHECK(lay::list_named_layouts(file).error().code == ErrorCode::IoError);
}
