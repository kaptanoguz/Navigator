// list_directory / stat on temporary fixture directories (navigator/fs.hpp).
#include <algorithm>
#include <filesystem>
#include <map>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <sys/stat.h>
#include <unistd.h>

#include <navigator/fs.hpp>

#include "temp_dir.hpp"

namespace fs = navigator::fs;
using fs::DirEntry;
using fs::EntryKind;
using navigator::ErrorCode;
using navigator::test::TempDir;

namespace {

std::map<std::string, DirEntry> by_name(const std::vector<DirEntry>& v) {
    std::map<std::string, DirEntry> out;
    for (const DirEntry& e : v) {
        out.emplace(e.name, e);
    }
    return out;
}

std::map<std::string, DirEntry> list_map(const TempDir& t, fs::ListOptions o = {}) {
    auto r = fs::list_directory(t.path(), o);
    REQUIRE(r.has_value());
    return by_name(*r);
}

fs::ListOptions with(bool show_hidden, bool follow) {
    fs::ListOptions o;
    o.show_hidden = show_hidden;
    o.follow_symlinks = follow;
    return o;
}

// Permission tests are meaningless for root (euid 0 bypasses mode bits).
#define SKIP_IF_ROOT()                                                                             \
    do {                                                                                           \
        if (navigator::test::running_as_root()) {                                                  \
            SKIP("running as root: permission bits are not enforced");                             \
        }                                                                                          \
    } while (false)

} // namespace

TEST_CASE("empty directory lists nothing", "[list]") {
    const TempDir t;
    auto r = fs::list_directory(t.path());
    REQUIRE(r.has_value());
    CHECK(r->empty());
    auto h = fs::list_directory(t.path(), with(true, false));
    REQUIRE(h.has_value());
    CHECK(h->empty()); // "." and ".." are never reported
}

TEST_CASE("files, directories and metadata", "[list]") {
    const TempDir t;
    t.file("a.txt", 123);
    t.dir("sub");
    ::mkfifo((t / "pipe").c_str(), 0600);
    const auto m = list_map(t);
    REQUIRE(m.size() == 3);

    const DirEntry& f = m.at("a.txt");
    CHECK(f.kind == EntryKind::File);
    CHECK(f.size == 123);
    CHECK(S_ISREG(f.mode));
    CHECK((f.mode & 0777U) == 0644U);
    CHECK(f.uid == ::geteuid());
    CHECK(f.readable);
    CHECK_FALSE(f.hidden);
    CHECK_FALSE(f.metadata_error.has_value());
    CHECK(f.target_kind == EntryKind::Unknown);
    CHECK_FALSE(f.dangling);
    struct stat st {};
    REQUIRE(::stat((t / "a.txt").c_str(), &st) == 0);
    CHECK(f.id.inode == st.st_ino);
    CHECK(f.id.device == st.st_dev);
    CHECK(f.mtime.time_since_epoch().count() ==
          static_cast<long long>(st.st_mtim.tv_sec) * 1'000'000'000LL + st.st_mtim.tv_nsec);
    CHECK(f.mtime != navigator::Timestamp{});

    const DirEntry& d = m.at("sub");
    CHECK(d.kind == EntryKind::Directory);
    CHECK(d.size == 0); // st_size not exposed for directories
    CHECK(fs::is_directory_like(d));

    const DirEntry& p = m.at("pipe");
    CHECK(p.kind == EntryKind::Other);
    CHECK(S_ISFIFO(p.mode));
    CHECK(p.size == 0);
}

TEST_CASE("Turkish and other Unicode names round-trip byte-exactly", "[list][unicode]") {
    const TempDir t;
    const std::vector<std::string> names = {"ğüşıöçİ.txt", "İstanbul",      "ıspanak",
                                            "ĞÜŞIÖÇ",      "日本語",        "Ωmega",
                                            "emoji-😀",    "çok uzun ad şş"};
    for (const auto& n : names) {
        t.file(n);
    }
    const auto m = list_map(t);
    REQUIRE(m.size() == names.size());
    for (const auto& n : names) {
        CHECK(m.count(n) == 1);
        CHECK(fs::to_display_name(n) == n);
    }
}

TEST_CASE("NFC and NFD names are distinct entries", "[list][unicode]") {
    const TempDir t;
    const std::string nfc = "caf\xC3\xA9";  // café, U+00E9
    const std::string nfd = "cafe\xCC\x81"; // cafe + U+0301
    t.file(nfc, 1);
    t.file(nfd, 2);
    const auto m = list_map(t);
    REQUIRE(m.size() == 2);
    CHECK(m.at(nfc).size == 1);
    CHECK(m.at(nfd).size == 2);
    CHECK(fs::compare_names(nfc, nfd, fs::SortOptions{}) != 0);
}

TEST_CASE("invalid UTF-8 names are listed raw and displayed with U+FFFD", "[list][unicode]") {
    const TempDir t;
    const std::string bad = std::string("bad\xFF\xFE") + "name";
    const std::string latin1 = "caf\xE9"; // ISO-8859-1 é
    t.file(bad);
    t.file(latin1);
    const auto m = list_map(t);
    REQUIRE(m.size() == 2);
    REQUIRE(m.count(bad) == 1);
    REQUIRE(m.count(latin1) == 1);
    CHECK(fs::to_display_name(bad) == "bad\xEF\xBF\xBD\xEF\xBF\xBDname");
    CHECK(fs::to_display_name(latin1) == "caf\xEF\xBF\xBD");
    CHECK_FALSE(m.at(bad).metadata_error.has_value());
    // The raw name addresses the file.
    auto s = fs::stat(t / bad);
    REQUIRE(s.has_value());
    CHECK(s->name == bad);
}

TEST_CASE("hidden entries are filtered unless show_hidden", "[list][hidden]") {
    const TempDir t;
    t.file(".bashrc");
    t.dir(".config");
    t.file("visible");
    t.file("not.hidden.");
    const auto shown = list_map(t);
    CHECK(shown.size() == 2);
    CHECK(shown.count("visible") == 1);
    CHECK_FALSE(shown.at("visible").hidden);
    const auto all = list_map(t, with(true, false));
    REQUIRE(all.size() == 4);
    CHECK(all.at(".bashrc").hidden);
    CHECK(all.at(".config").hidden);
    CHECK(all.at(".config").kind == EntryKind::Directory);
    CHECK_FALSE(all.at("not.hidden.").hidden);
}

TEST_CASE("symlinks: not followed by default", "[list][symlink]") {
    const TempDir t;
    t.file("target.txt", 42);
    t.dir("target_dir");
    t.link("to_file", "target.txt");
    t.link("to_dir", "target_dir");
    t.link("dangling", "does-not-exist");
    t.link("dangling_notdir", "target.txt/below");
    t.link("loop_a", "loop_b");
    t.link("loop_b", "loop_a");
    t.link("self", "self");
    const auto m = list_map(t);
    REQUIRE(m.size() == 9);

    const DirEntry& lf = m.at("to_file");
    CHECK(lf.kind == EntryKind::Symlink);
    CHECK(lf.target_kind == EntryKind::File);
    CHECK_FALSE(lf.dangling);
    CHECK(lf.size == std::string("target.txt").size()); // link text length
    CHECK(S_ISLNK(lf.mode));
    CHECK_FALSE(fs::is_directory_like(lf));

    const DirEntry& ld = m.at("to_dir");
    CHECK(ld.kind == EntryKind::Symlink);
    CHECK(ld.target_kind == EntryKind::Directory);
    CHECK(fs::is_directory_like(ld));
    CHECK(S_ISLNK(ld.mode));

    for (const char* name : {"dangling", "dangling_notdir"}) {
        const DirEntry& e = m.at(name);
        CHECK(e.kind == EntryKind::Symlink);
        CHECK(e.dangling);
        CHECK(e.target_kind == EntryKind::Unknown);
        CHECK_FALSE(e.metadata_error.has_value());
    }
    for (const char* name : {"loop_a", "loop_b", "self"}) {
        const DirEntry& e = m.at(name);
        CHECK(e.kind == EntryKind::Symlink);
        CHECK_FALSE(e.dangling); // ELOOP is not dangling
        CHECK(e.target_kind == EntryKind::Unknown);
        CHECK_FALSE(e.metadata_error.has_value());
        CHECK(S_ISLNK(e.mode));
    }

    // folders_first groups the link to a directory with directories.
    std::vector<DirEntry> v;
    v.reserve(m.size());
    for (const auto& [name, e] : m) {
        v.push_back(e);
    }
    fs::sort_entries(v, fs::SortOptions{});
    REQUIRE(v.size() >= 2);
    CHECK(v[0].name == "target_dir");
    CHECK(v[1].name == "to_dir");
}

TEST_CASE("symlinks: follow_symlinks reports target metadata", "[list][symlink]") {
    const TempDir t;
    t.file("target.txt", 42);
    t.dir("target_dir");
    t.link("to_file", "target.txt");
    t.link("to_dir", "target_dir");
    t.link("dangling", "nope");
    t.link("self", "self");
    const auto m = list_map(t, with(false, true));
    struct stat target {};
    REQUIRE(::stat((t / "target.txt").c_str(), &target) == 0);

    const DirEntry& lf = m.at("to_file");
    CHECK(lf.kind == EntryKind::Symlink); // kind always describes the entry itself
    CHECK(lf.target_kind == EntryKind::File);
    CHECK(lf.size == 42);
    CHECK(S_ISREG(lf.mode));
    CHECK(lf.id.inode == target.st_ino);

    const DirEntry& ld = m.at("to_dir");
    CHECK(S_ISDIR(ld.mode));
    CHECK(ld.size == 0);
    CHECK(fs::is_directory_like(ld));

    const DirEntry& dl = m.at("dangling");
    CHECK(dl.dangling);
    CHECK(S_ISLNK(dl.mode)); // keeps lstat metadata
    CHECK(dl.size == 4);

    const DirEntry& self = m.at("self");
    CHECK_FALSE(self.dangling);
    CHECK(S_ISLNK(self.mode));
}

TEST_CASE("list_directory follows a symlink given as the directory", "[list][symlink]") {
    const TempDir t;
    t.dir("real");
    t.file("real/inside");
    t.link("alias", "real");
    auto r = fs::list_directory(t / "alias");
    REQUIRE(r.has_value());
    REQUIRE(r->size() == 1);
    CHECK(r->front().name == "inside");
}

TEST_CASE("list_directory error mapping", "[list][errors]") {
    const TempDir t;
    t.file("plain");
    t.link("loop", "loop");
    t.link("dangling", "missing");

    auto empty = fs::list_directory(navigator::Path{});
    REQUIRE_FALSE(empty.has_value());
    CHECK(empty.error().code == ErrorCode::InvalidArgument);

    auto missing = fs::list_directory(t / "missing");
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().code == ErrorCode::NotFound);
    CHECK(missing.error().message.find("missing") != std::string::npos);

    auto notdir = fs::list_directory(t / "plain");
    REQUIRE_FALSE(notdir.has_value());
    CHECK(notdir.error().code == ErrorCode::InvalidArgument);

    auto loop = fs::list_directory(t / "loop");
    REQUIRE_FALSE(loop.has_value());
    CHECK(loop.error().code == ErrorCode::InvalidArgument);

    auto dangling = fs::list_directory(t / "dangling");
    REQUIRE_FALSE(dangling.has_value());
    CHECK(dangling.error().code == ErrorCode::NotFound);

    auto toolong = fs::list_directory(t / std::string(300, 'x'));
    REQUIRE_FALSE(toolong.has_value());
    CHECK(toolong.error().code == ErrorCode::InvalidArgument);
}

TEST_CASE("permission denied directory", "[list][errors][permissions]") {
    SKIP_IF_ROOT();
    const TempDir t;
    const auto locked = t.dir("locked");
    t.file("locked/secret");
    REQUIRE(::chmod(locked.c_str(), 0000) == 0);
    auto r = fs::list_directory(locked);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == ErrorCode::PermissionDenied);

    const auto m = list_map(t);
    CHECK_FALSE(m.at("locked").readable);
    CHECK(m.at("locked").kind == EntryKind::Directory);

    auto inner = fs::stat(locked / "secret");
    REQUIRE_FALSE(inner.has_value());
    CHECK(inner.error().code == ErrorCode::PermissionDenied);
    ::chmod(locked.c_str(), 0700);
}

TEST_CASE("per-entry metadata failure keeps the entry", "[list][errors][permissions]") {
    SKIP_IF_ROOT();
    // r-- directory: readdir works, fstatat of children fails with EACCES.
    const TempDir t;
    const auto dir = t.dir("noexec");
    t.file("noexec/file");
    t.dir("noexec/child");
    REQUIRE(::chmod(dir.c_str(), 0400) == 0);
    auto r = fs::list_directory(dir);
    ::chmod(dir.c_str(), 0700);
    REQUIRE(r.has_value());
    const auto m = by_name(*r);
    REQUIRE(m.size() == 2);
    for (const auto& [name, e] : m) {
        CHECK(e.metadata_error == std::optional<ErrorCode>{ErrorCode::PermissionDenied});
        CHECK(e.size == 0);
        CHECK(e.mode == 0);
        CHECK(e.mtime == navigator::Timestamp{});
        CHECK(e.id == navigator::FileId{});
        CHECK_FALSE(e.readable);
    }
    // Kind comes from d_type when the filesystem provides it.
    const EntryKind fk = m.at("file").kind;
    const EntryKind dk = m.at("child").kind;
    CHECK((fk == EntryKind::File || fk == EntryKind::Unknown));
    CHECK((dk == EntryKind::Directory || dk == EntryKind::Unknown));
}

TEST_CASE("readable hint from mode bits", "[list][permissions]") {
    SKIP_IF_ROOT();
    const TempDir t;
    const auto open_file = t.file("open");
    const auto closed = t.file("closed");
    const auto wonly = t.file("write-only");
    REQUIRE(::chmod(closed.c_str(), 0000) == 0);
    REQUIRE(::chmod(wonly.c_str(), 0200) == 0);
    REQUIRE(::chmod(open_file.c_str(), 0400) == 0);
    const auto m = list_map(t);
    CHECK(m.at("open").readable);
    CHECK_FALSE(m.at("closed").readable);
    CHECK_FALSE(m.at("write-only").readable);
    CHECK((m.at("closed").mode & 07777U) == 0U);
}

TEST_CASE("cancellation", "[list][cancel]") {
    const TempDir t;
    for (int i = 0; i < 3000; ++i) {
        t.file("f" + std::to_string(i));
    }
    SECTION("stop requested before the call") {
        std::stop_source src;
        src.request_stop();
        auto r = fs::list_directory(t.path(), {}, src.get_token());
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().code == ErrorCode::Cancelled);
    }
    SECTION("unrequested stop token lists everything") {
        std::stop_source src;
        auto r = fs::list_directory(t.path(), {}, src.get_token());
        REQUIRE(r.has_value());
        CHECK(r->size() == 3000);
    }
    SECTION("default token (no stop state)") {
        auto r = fs::list_directory(t.path(), {}, std::stop_token{});
        REQUIRE(r.has_value());
        CHECK(r->size() == 3000);
    }
}

TEST_CASE("stat()", "[stat]") {
    const TempDir t;
    t.file("f.txt", 7);
    t.dir("d");
    t.file(".hidden");
    t.link("l", "f.txt");
    t.link("dl", "nowhere");
    t.link("loop", "loop");

    auto f = fs::stat(t / "f.txt");
    REQUIRE(f.has_value());
    CHECK(f->name == "f.txt");
    CHECK(f->kind == EntryKind::File);
    CHECK(f->size == 7);
    CHECK_FALSE(f->metadata_error.has_value());

    auto d = fs::stat(t.path() / "d" / "");
    REQUIRE(d.has_value());
    CHECK(d->name == "d");
    CHECK(d->kind == EntryKind::Directory);

    auto h = fs::stat(t / ".hidden");
    REQUIRE(h.has_value());
    CHECK(h->hidden);

    auto root = fs::stat("/");
    REQUIRE(root.has_value());
    CHECK(root->name == "/");
    CHECK(root->kind == EntryKind::Directory);
    CHECK_FALSE(root->hidden);

    auto l = fs::stat(t / "l");
    REQUIRE(l.has_value());
    CHECK(l->kind == EntryKind::Symlink);
    CHECK(l->target_kind == EntryKind::File);
    CHECK(l->size == 5);
    auto lf = fs::stat(t / "l", true);
    REQUIRE(lf.has_value());
    CHECK(lf->kind == EntryKind::Symlink);
    CHECK(lf->size == 7);
    CHECK(S_ISREG(lf->mode));

    auto dl = fs::stat(t / "dl", true);
    REQUIRE(dl.has_value());
    CHECK(dl->dangling);
    CHECK(S_ISLNK(dl->mode));

    auto loop = fs::stat(t / "loop");
    REQUIRE(loop.has_value());
    CHECK(loop->kind == EntryKind::Symlink);
    CHECK_FALSE(loop->dangling);
    CHECK(loop->target_kind == EntryKind::Unknown);
}

TEST_CASE("stat() error mapping", "[stat][errors]") {
    const TempDir t;
    t.file("plain");
    t.link("loop", "loop");
    auto e = fs::stat(navigator::Path{});
    REQUIRE_FALSE(e.has_value());
    CHECK(e.error().code == ErrorCode::InvalidArgument);
    auto nf = fs::stat(t / "missing");
    REQUIRE_FALSE(nf.has_value());
    CHECK(nf.error().code == ErrorCode::NotFound);
    auto nd = fs::stat(t.path() / "plain" / "below");
    REQUIRE_FALSE(nd.has_value());
    CHECK(nd.error().code == ErrorCode::NotFound); // ENOTDIR on a prefix
    auto lp = fs::stat(t.path() / "loop" / "x");
    REQUIRE_FALSE(lp.has_value());
    CHECK(lp.error().code == ErrorCode::InvalidArgument); // ELOOP
    auto tl = fs::stat(t / std::string(300, 'y'));
    REQUIRE_FALSE(tl.has_value());
    CHECK(tl.error().code == ErrorCode::InvalidArgument);
}

TEST_CASE("stat() agrees with the listed entry", "[stat][list]") {
    const TempDir t;
    t.file("x", 3);
    t.link("y", "x");
    const auto m = list_map(t);
    for (const char* name : {"x", "y"}) {
        auto s = fs::stat(t / name);
        REQUIRE(s.has_value());
        const DirEntry& e = m.at(name);
        CHECK(s->kind == e.kind);
        CHECK(s->target_kind == e.target_kind);
        CHECK(s->size == e.size);
        CHECK(s->mode == e.mode);
        CHECK(s->mtime == e.mtime);
        CHECK(s->id == e.id);
        CHECK(s->readable == e.readable);
    }
}
