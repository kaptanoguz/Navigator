// Engine <-> REAL platform trash (owner: qa, Phase 7). tests/ops uses a fake trash; here the engine
// runs against trash::create_trash() with the home trash inside a fixture and a fake mount
// table, wrapped in GuardedTrash (aborts on any path outside a fixture). Covers trash -> list ->
// restore round trips with Turkish / space / % names, undo and redo of trash and copy (never an
// unlink), replace-via-trash, restore conflicts, untrashable volumes, cross-device refusal, name
// collisions inside the trash, and "empty trash only through an explicit confirmed operation".
// The byte layout of our .trashinfo is compared with what `gio trash` writes (GIO_USE_VFS=local,
// HOME and XDG_DATA_HOME inside a fixture).
#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <sys/wait.h>

#include <navigator/platform.hpp>

#include "ops_support.hpp"

using namespace navigator;
using namespace navigator::opsit;
using navigator::ops::ConflictAction;
using navigator::ops::ConflictDecision;
using navigator::ops::ItemStatus;
using navigator::ops::OpKind;

namespace {

const std::vector<std::string>& awkward_names() {
    static const std::vector<std::string> names{
        "plain.txt",
        "rapor son%.pdf",
        "100% real %41 %2F.txt",
        "Çalışma Belgesi ğüşıöç İĞÜŞÖÇ.txt",
        "a  b   c .txt",
        "  leading and trailing  ",
        ".hidden file",
        "semi;colon&amp=eq+plus#hash?q.txt",
        "tab\there.txt",
        std::string("ünï") + "\xCC\x88" + "code-nfd.txt", // decomposed
    };
    return names;
}

ops::ConflictResolver always(ConflictAction action) {
    return [action](const ops::Conflict&) {
        ConflictDecision d;
        d.action = action;
        return d;
    };
}

// Home trash name -> .trashinfo text with the DeletionDate line removed.
std::map<std::string, std::string> info_texts(const Path& trash_root) {
    std::map<std::string, std::string> out;
    std::error_code ec;
    for (auto it = stdfs::directory_iterator(trash_root / "info", ec);
         !ec && it != stdfs::directory_iterator(); it.increment(ec)) {
        std::string text = read_file(it->path());
        const std::size_t at = text.find("DeletionDate=");
        if (at != std::string::npos) {
            const std::size_t end = text.find('\n', at);
            text.replace(at, end == std::string::npos ? std::string::npos : end - at + 1,
                         "DeletionDate=<date>\n");
        }
        out[it->path().filename().native()] = std::move(text);
    }
    return out;
}

// Runs `gio trash` on `paths` with a fixture-only environment. nullopt: gio missing or unusable.
std::optional<bool> gio_trash(const Path& home, const Path& data, const std::vector<Path>& paths) {
    const Result<Path> gio = platform::find_program("gio");
    if (!gio) {
        return std::nullopt;
    }
    require_inside(home);
    require_inside(data);
    for (const Path& p : paths) {
        require_inside(p);
    }
    const pid_t pid = ::fork();
    if (pid < 0) {
        return std::nullopt;
    }
    if (pid == 0) {
        const std::string home_env = "HOME=" + home.native();
        const std::string data_env = "XDG_DATA_HOME=" + data.native();
        std::vector<std::string> argv_s{"gio", "trash"};
        for (const Path& p : paths) {
            argv_s.push_back(p.native());
        }
        std::vector<char*> argv;
        argv.reserve(argv_s.size() + 1);
        for (std::string& s : argv_s) {
            argv.push_back(s.data());
        }
        argv.push_back(nullptr);
        const char* envp[] = {home_env.c_str(),
                              data_env.c_str(),
                              "GIO_USE_VFS=local",
                              "GIO_USE_VOLUME_MONITOR=unix",
                              "PATH=/usr/bin:/bin",
                              "LC_ALL=C.UTF-8",
                              nullptr};
        const int devnull = ::open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            ::dup2(devnull, 1);
            ::dup2(devnull, 2);
        }
        ::execve(gio->c_str(), argv.data(), const_cast<char* const*>(envp));
        ::_exit(127);
    }
    int status = 0;
    if (::waitpid(pid, &status, 0) != pid) {
        return std::nullopt;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

} // namespace

TEST_CASE("trash -> list -> restore keeps names, bytes, modes and links (Turkish, space, %)",
          "[ops][trash][e2e]") {
    Fixture fx;
    Rig rig(fx);
    const Path dir = fx.dir("Masaüstü/Belgeler");
    std::map<Path, std::string> before;
    std::vector<Path> sources;
    for (const std::string& name : awkward_names()) {
        const Path p = fx.file("Masaüstü/Belgeler/" + name, "içerik of " + name);
        ::chmod(p.c_str(), 0640);
        sources.push_back(p);
    }
    // A folder tree, a dangling link and a link to a sibling (moved as links, never followed).
    fx.file("Masaüstü/Belgeler/Klasör ğ/iç/dosya.bin", std::string(100000, 'x'));
    fx.file("Masaüstü/Belgeler/Klasör ğ/ikinci.txt", "2");
    fx.link("Masaüstü/Belgeler/Klasör ğ/dangling", "/nonexistent/target-ğ");
    fx.link("Masaüstü/Belgeler/to sibling", "plain.txt");
    sources.push_back(dir / "Klasör ğ");
    sources.push_back(dir / "to sibling");
    for (const Path& s : sources) {
        before[s] = item_digest(s);
        REQUIRE_FALSE(before[s].empty());
    }

    auto trashed = run(rig, trash_req(sources));
    REQUIRE(trashed);
    CHECK(trashed->failed == 0);
    CHECK(trashed->done == sources.size());
    CHECK(trashed->untrashable.empty());
    for (const Path& s : sources) {
        CHECK_FALSE(present(s)); // each one left the folder (one rename)
    }
    CHECK(part_files(fx.root()).empty());
    REQUIRE(trashed->record.has_value());
    CHECK(rec(*trashed).steps.size() == sources.size());

    // The trash lists exactly these items with their original paths and escaped Path= lines.
    auto listing = rig.trash->list(std::stop_token{});
    REQUIRE(listing);
    CHECK(listing->orphans == 0);
    REQUIRE(listing->items.size() == sources.size());
    std::set<Path> originals;
    for (const trash::TrashedItem& i : listing->items) {
        originals.insert(i.original_path);
        const std::string info = read_file(i.info_path());
        CHECK(info.starts_with("[Trash Info]\nPath=" +
                               trash::escape_path(i.original_path.native()) + "\nDeletionDate="));
        CHECK(info.ends_with("\n"));
    }
    CHECK(originals == std::set<Path>(sources.begin(), sources.end()));
    // A Path= value never contains a raw space, a raw non-ASCII byte or a raw %-less escape.
    for (const auto& [name, text] : info_texts(rig.home_trash)) {
        const std::size_t p = text.find("Path=");
        const std::string value = text.substr(p + 5, text.find('\n', p) - p - 5);
        INFO(name << ": " << value);
        CHECK(value.find(' ') == std::string::npos);
        for (const char c : value) {
            CHECK(static_cast<unsigned char>(c) < 0x80);
        }
        auto decoded = trash::unescape_path(value);
        REQUIRE(decoded.has_value());
    }

    // Restore all through the engine: names, bytes, modes and links are exactly as before.
    auto restored = run(rig, restore_req(listing->items));
    REQUIRE(restored);
    CHECK(restored->failed == 0);
    CHECK(restored->done == sources.size());
    for (const Path& s : sources) {
        INFO(s.native());
        CHECK(item_digest(s) == before[s]);
    }
    CHECK(rig.trash_names().empty());
    CHECK(rig.info_names().empty());

    // Undo of the restore puts them back in the trash; the redo record (the undo's own result,
    // ops.hpp "Undo / redo") repeats the restore.
    REQUIRE(restored->record.has_value());
    auto again = run(rig, undo_req(rec(*restored)));
    REQUIRE(again);
    CHECK(again->failed == 0);
    CHECK(rig.trash_names().size() == sources.size());
    for (const Path& s : sources) {
        CHECK_FALSE(present(s));
    }
    REQUIRE(again->record.has_value());
    auto back = run(rig, redo_req(rec(*again)));
    REQUIRE(back);
    CHECK(back->failed == 0);
    for (const Path& s : sources) {
        CHECK(item_digest(s) == before[s]);
    }
}

TEST_CASE("Restore recreates missing parents, records them, and undo rmdirs them only while empty",
          "[ops][trash][e2e][restore]") {
    Fixture fx;
    Rig rig(fx);
    const Path a = fx.file("deep/er/still/file.txt", "f");
    auto t = run(rig, trash_req({a}));
    REQUIRE(t);
    // The whole parent chain disappears (we remove it ourselves in the fixture).
    stdfs::remove_all(fx / "deep");
    auto items = list_trash(rig);
    REQUIRE(items.size() == 1);
    auto r = run(rig, restore_req(items));
    REQUIRE(r);
    CHECK(r->failed == 0);
    CHECK(read_file(a) == "f");
    REQUIRE(r->record.has_value());
    REQUIRE(rec(*r).steps.size() == 1);
    CHECK(rec(*r).steps[0].created_parents.size() == 3);
    // A new file appears in the middle folder: undo must not rmdir that folder.
    auto undone = run(rig, undo_req(rec(*r)));
    REQUIRE(undone);
    CHECK_FALSE(present(a));
    CHECK(rig.trash_names().size() == 1);

    // Second round: restore again, then drop an extra file into the created parent and undo.
    auto items2 = list_trash(rig);
    auto r2 = run(rig, restore_req(items2));
    REQUIRE(r2);
    fx.file("deep/er/stranger.txt", "keep me");
    auto u2 = run(rig, undo_req(rec(*r2)));
    REQUIRE(u2);
    CHECK(read_file(fx / "deep/er/stranger.txt") == "keep me"); // never deleted
    CHECK_FALSE(present(a));
}

TEST_CASE("Our .trashinfo bytes and trash names equal what gio trash writes", "[ops][trash][gio]") {
    Fixture fx;
    const Path home = fx.dir("home-gio");
    const Path data = fx.dir("data-gio");
    // Two identical sets of files in the same folder, one after the other: gio first, then us.
    const Path dir = fx.dir("src dir/Üst Klasör");
    std::vector<Path> paths;
    const auto make = [&] {
        paths.clear();
        for (const std::string& name : awkward_names()) {
            if (name.find('\t') != std::string::npos) {
                continue; // gio escapes it too, but GLib's local trash rejects some control chars
            }
            paths.push_back(fx.file("src dir/Üst Klasör/" + name, "gio"));
        }
        paths.push_back(fx.file("src dir/Üst Klasör/sub/inner.txt", "i"));
        paths.back() = dir / "sub";
    };
    make();
    const std::optional<bool> ran = gio_trash(home, data, paths);
    if (!ran.has_value() || !*ran) {
        SKIP("gio trash is not usable here");
    }
    for (const Path& p : paths) {
        REQUIRE_FALSE(present(p));
    }
    const std::map<std::string, std::string> gio_infos = info_texts(data / "Trash");
    std::vector<std::string> gio_names;
    gio_names.reserve(gio_infos.size());
    for (const auto& [k, v] : gio_infos) {
        gio_names.push_back(k);
    }

    // Ours, into a second home trash inside the fixture, the same paths again.
    make();
    Rig rig(fx);
    auto r = run(rig, trash_req(paths));
    REQUIRE(r);
    REQUIRE(r->failed == 0);
    const std::map<std::string, std::string> our_infos = info_texts(rig.home_trash);
    CHECK(our_infos == gio_infos); // same file names AND the same bytes (apart from the date)
    // The date format is the spec's local time, like gio's.
    for (const trash::TrashedItem& i : list_trash(rig)) {
        CHECK(i.deletion_date.has_value());
    }
    // Directory entry names in files/ agree too.
    std::vector<std::string> gio_files;
    std::error_code ec;
    for (auto it = stdfs::directory_iterator(data / "Trash" / "files", ec);
         !ec && it != stdfs::directory_iterator(); it.increment(ec)) {
        gio_files.push_back(it->path().filename().native());
    }
    std::sort(gio_files.begin(), gio_files.end());
    CHECK(rig.trash_names() == gio_files);
}

TEST_CASE(
    "Name collisions inside the trash: gio-style suffixes, every item restores to its own path",
    "[ops][trash][e2e][collision]") {
    Fixture fx;
    Rig rig(fx);
    std::vector<Path> originals;
    for (int i = 0; i < 6; ++i) {
        const Path p =
            fx.file("d" + std::to_string(i) + "/same.txt", "content " + std::to_string(i));
        originals.push_back(p);
    }
    // First three one by one, the rest in one operation.
    for (int i = 0; i < 3; ++i) {
        REQUIRE(run(rig, trash_req({originals[static_cast<std::size_t>(i)]})));
    }
    REQUIRE(run(rig, trash_req({originals[3], originals[4], originals[5]})));
    const std::vector<std::string> names = rig.trash_names();
    CHECK(names == std::vector<std::string>{"same.txt", "same.txt.2", "same.txt.3", "same.txt.4",
                                            "same.txt.5", "same.txt.6"});
    CHECK(rig.info_names().size() == 6);
    // A folder with a very long (near NAME_MAX) multi-byte name collides and still fits.
    std::string long_name;
    while (long_name.size() < 250) {
        long_name += "ğ";
    }
    const Path l1 = fx.file("e1/" + long_name, "1");
    const Path l2 = fx.file("e2/" + long_name, "2");
    auto lr = run(rig, trash_req({l1, l2}));
    REQUIRE(lr);
    CHECK(lr->failed == 0);
    for (const std::string& n : rig.info_names()) {
        CHECK(n.size() <= 255);
    }
    // Restore everything: each content returns to the path it came from.
    auto items = list_trash(rig);
    REQUIRE(items.size() == 8);
    auto r = run(rig, restore_req(items));
    REQUIRE(r);
    CHECK(r->failed == 0);
    for (int i = 0; i < 6; ++i) {
        CHECK(read_file(originals[static_cast<std::size_t>(i)]) == "content " + std::to_string(i));
    }
    CHECK(read_file(l1) == "1");
    CHECK(read_file(l2) == "2");
    CHECK(rig.trash_names().empty());
}

TEST_CASE(
    "Undo of a copy trashes the copy (never unlinks); redo copies again; undo of trash restores",
    "[ops][trash][e2e][undo]") {
    Fixture fx;
    Rig rig(fx);
    fx.file("src/a.txt", "A");
    fx.file("src/tree/b.txt", "B");
    fx.link("src/tree/l", "b.txt");
    const Path dst = fx.dir("dst");
    const std::string src_digest = tree_digest(fx / "src");
    auto c = run(rig, copy_req({fx / "src/a.txt", fx / "src/tree"}, dst));
    REQUIRE(c);
    REQUIRE(c->record.has_value());
    CHECK(tree_digest(fx / "src") == src_digest);
    const std::string copy_digest = tree_digest(dst);

    auto u = run(rig, undo_req(rec(*c)));
    REQUIRE(u);
    CHECK(u->failed == 0);
    CHECK(count_entries(dst) == 0);
    // The copies are in the trash with their data, not gone.
    CHECK(rig.trash_names() == std::vector<std::string>{"a.txt", "tree"});
    CHECK(read_file(rig.home_trash / "files/a.txt") == "A");
    CHECK(read_file(rig.home_trash / "files/tree/b.txt") == "B");
    CHECK(tree_digest(fx / "src") == src_digest);
    CHECK(rig.trash->trash_calls == 2);

    // Redo (the undo's own result record): the copy is made again from `before`, the trashed
    // copies stay in the trash; undoing the redo trashes the new copies under fresh names.
    REQUIRE(u->record.has_value());
    auto redo = run(rig, redo_req(rec(*u)));
    REQUIRE(redo);
    CHECK(redo->failed == 0);
    CHECK(tree_digest(dst) == copy_digest);
    CHECK(rig.trash_names() == std::vector<std::string>{"a.txt", "tree"});
    REQUIRE(redo->record.has_value());
    auto undo2 = run(rig, undo_req(rec(*redo)));
    REQUIRE(undo2);
    CHECK(count_entries(dst) == 0);
    CHECK(rig.trash_names() == std::vector<std::string>{"a.txt", "a.txt.2", "tree", "tree.2"});
    CHECK(part_files(fx.root()).empty());
    CHECK(tree_digest(fx / "src") == src_digest);
}

TEST_CASE("Replace goes through the trash, undo brings the replaced file back",
          "[ops][trash][e2e][replace]") {
    Fixture fx;
    Rig rig(fx);
    fx.file("src/doc.txt", "NEW");
    fx.file("dst/doc.txt", "OLD");
    fx.file("src/dirx/n.txt", "n");
    fx.file("dst/dirx/old.txt", "o");
    auto r = run(rig, copy_req({fx / "src/doc.txt"}, fx / "dst", ops::ConflictPolicy::Replace));
    REQUIRE(r);
    CHECK(r->failed == 0);
    CHECK(read_file(fx / "dst/doc.txt") == "NEW");
    CHECK(read_file(rig.home_trash / "files/doc.txt") == "OLD"); // the old one was trashed
    REQUIRE(r->record.has_value());
    CHECK_FALSE(rec(*r).lossy);
    REQUIRE(rec(*r).steps.size() == 1);
    CHECK(rec(*r).steps[0].replaced.has_value());

    auto u = run(rig, undo_req(rec(*r)));
    REQUIRE(u);
    CHECK(u->failed == 0);
    CHECK(read_file(fx / "dst/doc.txt") == "OLD");
    CHECK(read_file(fx / "src/doc.txt") == "NEW");
    // The NEW copy went to the trash (not unlinked), the old one is back and the info is gone
    // for it.
    CHECK(rig.trash_names().size() == 1);
    CHECK(read_file(rig.home_trash / ("files/" + rig.trash_names()[0])) == "NEW");

    // A folder onto a folder with Replace: the existing folder is trashed whole, not merged away.
    auto d = run(rig, copy_req({fx / "src/dirx"}, fx / "dst", ops::ConflictPolicy::Replace));
    REQUIRE(d);
    CHECK(read_file(fx / "dst/dirx/n.txt") == "n");
    CHECK_FALSE(present(fx / "dst/dirx/old.txt"));
    bool found_old = false;
    for (const std::string& n : rig.trash_names()) {
        found_old = found_old || read_file(rig.home_trash / "files" / n / "old.txt") == "o";
    }
    CHECK(found_old);
}

TEST_CASE("Replace without any trash: a file is replaced atomically and the record is lossy; "
          "a folder is never replaced permanently",
          "[ops][trash][e2e][replace]") {
    Fixture fx;
    Rig rig(fx, {}, /*with_trash=*/false);
    fx.file("src/doc.txt", "NEW");
    fx.file("dst/doc.txt", "OLD");
    fx.file("src/dirx/n.txt", "n");
    fx.file("dst/dirx/old.txt", "o");
    std::vector<ops::Conflict> seen;
    auto answer = [&](const ops::Conflict& c) {
        seen.push_back(c);
        ConflictDecision d;
        d.action = ConflictAction::Skip;
        return d;
    };
    auto r = run(rig, copy_req({fx / "src/doc.txt", fx / "src/dirx"}, fx / "dst"), answer);
    REQUIRE(r);
    REQUIRE(seen.size() == 2);
    for (const ops::Conflict& c : seen) {
        if (c.existing.kind == fs::EntryKind::Directory) {
            CHECK_FALSE(c.replace_allowed); // never replaced permanently
        } else {
            CHECK(c.replace_allowed);
            CHECK(c.replace_is_permanent); // the sheet must warn before the decision
        }
    }
    CHECK(read_file(fx / "dst/doc.txt") == "OLD"); // Skip changed nothing
    CHECK(read_file(fx / "dst/dirx/old.txt") == "o");
    // Now agree to the permanent file replace: atomic, lossy record.
    auto r2 = run(rig, copy_req({fx / "src/doc.txt"}, fx / "dst"), always(ConflictAction::Replace));
    REQUIRE(r2);
    CHECK(read_file(fx / "dst/doc.txt") == "NEW");
    REQUIRE(r2->record.has_value());
    CHECK(rec(*r2).lossy);
    CHECK(part_files(fx.root()).empty());
}

TEST_CASE("Restore conflict: Skip leaves the item in the trash, Keep both places it beside, "
          "Cancel stops",
          "[ops][trash][e2e][restore][conflict]") {
    Fixture fx;
    Rig rig(fx);
    const Path p = fx.file("w/Rapor ğ.txt", "original");
    REQUIRE(run(rig, trash_req({p})));
    fx.file("w/Rapor ğ.txt", "squatter"); // appeared since
    auto items = list_trash(rig);
    REQUIRE(items.size() == 1);

    SECTION("Skip") {
        auto r = run(rig, restore_req(items), always(ConflictAction::Skip));
        REQUIRE(r);
        CHECK(r->skipped == 1);
        CHECK(read_file(p) == "squatter");
        CHECK(list_trash(rig).size() == 1); // still trashed, info intact
        CHECK(rig.info_names().size() == 1);
    }
    SECTION("Keep both") {
        auto r = run(rig, restore_req(items), always(ConflictAction::KeepBoth));
        REQUIRE(r);
        CHECK(r->failed == 0);
        CHECK(read_file(p) == "squatter");
        CHECK(read_file(fx / "w/Rapor ğ (2).txt") == "original");
        CHECK(rig.trash_names().empty());
    }
    SECTION("Cancel (Escape on the sheet)") {
        auto r = run(rig, restore_req(items), always(ConflictAction::Cancel));
        REQUIRE(r);
        CHECK(r->cancelled);
        CHECK(read_file(p) == "squatter");
        CHECK(list_trash(rig).size() == 1);
    }
    SECTION("Replace trashes the squatter and undo brings it back") {
        auto r = run(rig, restore_req(items), always(ConflictAction::Replace));
        REQUIRE(r);
        CHECK(read_file(p) == "original");
        CHECK(rig.trash_names().size() == 1);
        REQUIRE(r->record.has_value());
        auto u = run(rig, undo_req(rec(*r)));
        REQUIRE(u);
        CHECK(read_file(p) == "squatter");
    }
    CHECK(part_files(fx.root()).empty());
}

TEST_CASE("Undo of a trash whose original name is taken becomes a conflict, never an overwrite",
          "[ops][trash][e2e][undo][conflict]") {
    Fixture fx;
    Rig rig(fx);
    const Path p = fx.file("u/doc.txt", "mine");
    auto t = run(rig, trash_req({p}));
    REQUIRE(t);
    fx.file("u/doc.txt", "someone else");
    int asked = 0;
    auto u = run(rig, undo_req(rec(*t)), [&](const ops::Conflict&) {
        ++asked;
        ConflictDecision d;
        d.action = ConflictAction::Skip;
        return d;
    });
    REQUIRE(u);
    CHECK(asked == 1);
    CHECK(read_file(p) == "someone else");
    CHECK(rig.trash_names().size() == 1);
}

TEST_CASE("Untrashable volume: Unsupported per item, listed, never deleted; permanent delete needs "
          "the confirmed flag",
          "[ops][trash][e2e][untrashable]") {
    Fixture fx;
    Fixture shm(Fixture::Where::Shm);
    if (!Fixture::shm_is_other_filesystem(fx.root())) {
        SKIP("/dev/shm is not a separate writable filesystem here");
    }
    // The volume exists in the fake mount table, but its .Trash-$uid is a symlink: unusable (T4).
    const Path vol = shm.dir("vol");
    const std::string u = std::to_string(::getuid());
    const Path elsewhere = fx.dir("elsewhere");
    REQUIRE(::symlink(elsewhere.c_str(), (vol / (".Trash-" + u)).c_str()) == 0);
    require_inside(vol / (".Trash-" + u));
    Rig rig(fx, mount_line(70, vol));
    const Path a = shm.file("vol/a.txt", "keep a");
    const Path b = shm.file("vol/tree/b.txt", "keep b");
    const std::string vol_digest = tree_digest(vol);

    auto r = run(rig, trash_req({a, vol / "tree"}));
    REQUIRE(r);
    CHECK(r->failed == 2);
    CHECK(r->untrashable.size() == 2);
    for (const ops::ItemOutcome& o : r->items) {
        CHECK(o.status == ItemStatus::Failed);
        CHECK(code_of(o) == ErrorCode::Unsupported);
    }
    CHECK_FALSE(r->record.has_value()); // nothing happened, nothing to undo
    CHECK(tree_digest(vol) == vol_digest);
    CHECK(count_entries(elsewhere) == 0);
    CHECK_FALSE(present(rig.home_trash)); // no fallback to the home trash (cross-device)
    CHECK(rig.trash->trash_calls == 2);

    // Permanent delete without the confirmation flag is rejected outright and by the queue too.
    auto rejected = run(rig, delete_req({a}, /*confirmed=*/false));
    CHECK_FALSE(rejected);
    CHECK(present(a));
    ops::QueueOptions qo;
    qo.engine = rig.engine;
    auto queue = ops::create_operation_queue(qo);
    REQUIRE(queue);
    auto bad = (*queue)->submit(delete_req({a}, false));
    CHECK_FALSE(bad);
    CHECK(present(a));
    // Undo and conflict decisions never produce a permanent delete: only a request that carries
    // confirmed removes the file, and then only the requested one.
    auto del = run(rig, delete_req({a}, true));
    REQUIRE(del);
    CHECK(del->done == 1);
    CHECK_FALSE(present(a));
    CHECK(read_file(b) == "keep b");
    CHECK_FALSE(del->record.has_value()); // permanent deletes are not undoable
    (*queue)->shutdown();
}

TEST_CASE("Cross-device trash refusal: an item on /dev/shm is never copied into the home trash",
          "[ops][trash][e2e][xdev]") {
    Fixture fx;
    Fixture shm(Fixture::Where::Shm);
    if (!Fixture::shm_is_other_filesystem(fx.root())) {
        SKIP("/dev/shm is not a separate writable filesystem here");
    }
    const Path f = shm.file("loose/file.txt", "on tmpfs");
    SECTION("no per-volume trash allowed: Unsupported, nothing moved") {
        Rig rig(fx, {}, true, /*topdir=*/false);
        auto r = run(rig, trash_req({f}));
        REQUIRE(r);
        CHECK(r->failed == 1);
        CHECK(code_of(r->items[0]) == ErrorCode::Unsupported);
        CHECK(r->untrashable == std::vector<Path>{f});
        CHECK(read_file(f) == "on tmpfs");
        CHECK(rig.trash_names().empty());
    }
    SECTION("fixture volume in the table: its own .Trash-$uid, restore from there") {
        const Path vol = shm.dir("vol");
        const Path g = shm.file("vol/Çekmece/g.txt", "volume item");
        Rig rig(fx, mount_line(71, vol));
        auto r = run(rig, trash_req({g}));
        REQUIRE(r);
        CHECK(r->failed == 0);
        CHECK_FALSE(present(g));
        const std::string u = std::to_string(::getuid());
        CHECK(present(vol / (".Trash-" + u) / "files/g.txt"));
        CHECK(rig.trash_names().empty()); // the home trash got nothing
        // Spec: Path= is relative to the topdir for per-volume trashes.
        CHECK(read_file(vol / (".Trash-" + u) / "info/g.txt.trashinfo")
                  .starts_with("[Trash Info]\nPath=%C3%87ekmece/g.txt\n"));
        auto items = list_trash(rig);
        REQUIRE(items.size() == 1);
        auto back = run(rig, restore_req(items));
        REQUIRE(back);
        CHECK(read_file(g) == "volume item");
    }
}

TEST_CASE("The trash is emptied only by an explicit, confirmed DeletePermanently of trashed items",
          "[ops][trash][e2e][empty]") {
    Fixture fx;
    Rig rig(fx);
    std::vector<Path> originals;
    originals.reserve(4);
    for (int i = 0; i < 4; ++i) {
        originals.push_back(fx.file("e/f" + std::to_string(i) + ".txt", "v" + std::to_string(i)));
    }
    auto t = run(rig, trash_req(originals));
    REQUIRE(t);
    const std::vector<std::string> in_trash = rig.trash_names();
    REQUIRE(in_trash.size() == 4);

    // A battery of operations that must never reduce the trash's content.
    fx.file("e/g.txt", "g");
    auto c = run(rig, copy_req({fx / "e/g.txt"}, fx.dir("elsewhere")));
    REQUIRE(c);
    auto cu = run(rig, undo_req(rec(*c))); // trashes the copy: the trash only grows
    REQUIRE(cu);
    auto ru = run(rig, redo_req(rec(*cu)));
    REQUIRE(ru);
    auto mv = run(rig, move_req({fx / "e/g.txt"}, fx.dir("moved")));
    REQUIRE(mv);
    auto un = run(rig, undo_req(rec(*mv)));
    REQUIRE(un);
    auto rn = run(rig, rename_req(fx / "e/g.txt", "h.txt"));
    REQUIRE(rn);
    for (const std::string& n : in_trash) {
        CHECK(present(rig.home_trash / "files" / n));
        CHECK(present(rig.home_trash / "info" / (n + ".trashinfo")));
    }

    auto items = list_trash(rig);
    REQUIRE(items.size() >= 4);
    // Not confirmed: rejected, nothing changes.
    auto no = run(rig, delete_trashed_req(items, /*confirmed=*/false));
    CHECK_FALSE(no);
    CHECK(list_trash(rig).size() == items.size());
    // Delete one of them (a "Delete" in the Trash view) with confirmation; the rest stays.
    const std::vector<trash::TrashedItem> one{items.front()};
    auto del1 = run(rig, delete_trashed_req(one));
    REQUIRE(del1);
    CHECK(del1->done == 1);
    CHECK_FALSE(del1->record.has_value());
    CHECK_FALSE(present(one[0].files_path()));
    CHECK_FALSE(present(one[0].info_path())); // forgotten: no orphan info file
    CHECK(list_trash(rig).size() == items.size() - 1);
    // Empty everything.
    auto rest = list_trash(rig);
    auto all = run(rig, delete_trashed_req(rest));
    REQUIRE(all);
    CHECK(all->failed == 0);
    CHECK(rig.trash_names().empty());
    CHECK(rig.info_names().empty());
    // Undo of the earlier trash now finds nothing to restore: reported, no crash, nothing made up.
    auto stale = run(rig, undo_req(rec(*t)));
    REQUIRE(stale);
    CHECK(stale->done == 0);
    for (const Path& o : originals) {
        CHECK_FALSE(present(o));
    }
}

TEST_CASE("A stale trash listing never restores a different item (identity check)",
          "[ops][trash][e2e][restore]") {
    Fixture fx;
    Rig rig(fx);
    const Path p = fx.file("s/x.txt", "first");
    REQUIRE(run(rig, trash_req({p})));
    auto items = list_trash(rig);
    REQUIRE(items.size() == 1);
    // Another program empties the trash and trashes a different "x.txt" under the same name.
    // (kept alive under another name: a freed inode could be reused by the next file and then
    // legitimately look like the same item)
    stdfs::rename(items[0].files_path(), fx / "aside.txt");
    stdfs::remove(items[0].info_path());
    fx.file("s/x.txt", "second");
    REQUIRE(run(rig, trash_req({p})));
    auto r = run(rig, restore_req(items)); // the old listing entry
    REQUIRE(r);
    CHECK(r->done == 0);
    CHECK_FALSE(present(p));              // nothing was put back from the wrong identity
    CHECK(rig.trash_names().size() == 1); // the new item is untouched
    CHECK(read_file(rig.home_trash / "files/x.txt") == "second");
}
