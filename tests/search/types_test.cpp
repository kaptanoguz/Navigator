// Type visibility filter, types popup summary and filter_entries (navigator/search.hpp).
#include <cstdint>
#include <stop_token>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <navigator/fs.hpp>
#include <navigator/search.hpp>
#include <navigator/walk.hpp>

using namespace navigator;
using namespace navigator::search;

namespace {

fs::DirEntry file(std::string name) {
    fs::DirEntry e;
    e.hidden = fs::is_hidden_name(name);
    e.name = std::move(name);
    e.kind = fs::EntryKind::File;
    return e;
}

fs::DirEntry folder(std::string name) {
    fs::DirEntry e = file(std::move(name));
    e.kind = fs::EntryKind::Directory;
    return e;
}

fs::DirEntry link_to_dir(std::string name) {
    fs::DirEntry e = file(std::move(name));
    e.kind = fs::EntryKind::Symlink;
    e.target_kind = fs::EntryKind::Directory;
    return e;
}

const TypeKey kFolder{TypeKind::Folder, {}};
const TypeKey kNoExt{TypeKind::NoExtension, {}};
TypeKey ext(std::string e) { return TypeKey{TypeKind::Extension, std::move(e)}; }

} // namespace

TEST_CASE("type_key_of classifies folders, no-extension and folded extensions", "[search][types]") {
    CHECK(type_key_of(folder("photos.2024")) == kFolder);
    CHECK(type_key_of(link_to_dir("link.d")) == kFolder);
    CHECK(type_key_of(file(".bashrc")) == kNoExt);
    CHECK(type_key_of(file("Makefile")) == kNoExt);
    CHECK(type_key_of(file("x.")) == kNoExt);
    CHECK(type_key_of(file("a.tar.gz")) == ext("gz"));
    CHECK(type_key_of(file("IMG.JPG")) == ext("jpg"));
    CHECK(type_key_of(file("img.jpg")) == ext("jpg"));
    CHECK(type_key_of(file("Belge.İÇ")) == ext("iç"));
    fs::DirEntry fifo = file("pipe.fifo");
    fifo.kind = fs::EntryKind::Other;
    CHECK(type_key_of(fifo) == ext("fifo"));
    fs::DirEntry dangling = file("broken.lnk");
    dangling.kind = fs::EntryKind::Symlink;
    dangling.dangling = true;
    CHECK(type_key_of(dangling) == ext("lnk"));
}

TEST_CASE("set_visible, is_visible, allows and normalize", "[search][types]") {
    TypeFilter f;
    CHECK_FALSE(f.active());
    set_visible(f, ext("jpg"), false);
    set_visible(f, ext("aaa"), false);
    set_visible(f, ext("jpg"), false); // idempotent
    CHECK(f.hidden_extensions == std::vector<std::string>{"aaa", "jpg"});
    CHECK(f.active());
    CHECK_FALSE(allows(f, file("A.JPG")));
    CHECK(allows(f, file("a.png")));
    CHECK(allows(f, folder("dir.jpg"))); // a folder is never an extension
    set_visible(f, kFolder, false);
    CHECK_FALSE(allows(f, folder("dir")));
    CHECK_FALSE(allows(f, link_to_dir("l")));
    set_visible(f, kNoExt, false);
    CHECK_FALSE(allows(f, file("Makefile")));
    CHECK_FALSE(is_visible(f, kNoExt));
    set_visible(f, ext("jpg"), true);
    set_visible(f, kFolder, true);
    set_visible(f, kNoExt, true);
    CHECK(f.hidden_extensions == std::vector<std::string>{"aaa"});
    CHECK(allows(f, file("x.jpg")));

    TypeFilter raw;
    raw.hidden_extensions = {"PNG", "jpg", "", "a/b", std::string("n\0l", 3), "png", "JPG"};
    normalize(raw);
    CHECK(raw.hidden_extensions == std::vector<std::string>{"jpg", "png"});
    TypeFilter again = raw;
    normalize(again);
    CHECK(again == raw);
}

TEST_CASE("summarize_types order, counts and hidden policy", "[search][types]") {
    const std::vector<fs::DirEntry> entries = {
        file("a.txt"),    file("b.TXT"),       file("c.md"),   file("d.png"),
        file("e.png"),    file("f.png"),       folder("src"),  link_to_dir("lnk"),
        file("Makefile"), file(".hidden.cfg"), folder(".git"), file(".bashrc"),
    };
    TypeFilter f;
    set_visible(f, ext("png"), false);
    const std::vector<TypeCount> shown = summarize_types(entries, false, f);
    REQUIRE(shown.size() == 5);
    CHECK(shown[0] == TypeCount{kFolder, 2, true}); // .git is hidden
    CHECK(shown[1] == TypeCount{kNoExt, 1, true});  // .bashrc is hidden
    CHECK(shown[2] == TypeCount{ext("png"), 3, false});
    CHECK(shown[3] == TypeCount{ext("txt"), 2, true});
    CHECK(shown[4] == TypeCount{ext("md"), 1, true});

    const std::vector<TypeCount> all = summarize_types(entries, true, f);
    REQUIRE(all.size() == 6);
    CHECK(all[0].count == 3);
    CHECK(all[1].count == 2);
    // Ties by count are ordered by extension bytes: cfg < md.
    CHECK(all[4].key == ext("cfg"));
    CHECK(all[5].key == ext("md"));

    CHECK(summarize_types({}, true, f).empty());
}

TEST_CASE("toggle_all over the present types", "[search][types]") {
    const std::vector<fs::DirEntry> entries = {file("a.txt"), folder("d"), file("Makefile")};
    TypeFilter f;
    set_visible(f, ext("zip"), false); // not present: left alone
    std::vector<TypeCount> present = summarize_types(entries, false, f);
    toggle_all(f, present); // all visible -> hide all present
    CHECK(f.hide_folders);
    CHECK(f.hide_no_extension);
    CHECK(f.hidden_extensions == std::vector<std::string>{"txt", "zip"});
    present = summarize_types(entries, false, f);
    toggle_all(f, present); // some hidden -> show all present
    CHECK_FALSE(f.hide_folders);
    CHECK_FALSE(f.hide_no_extension);
    CHECK(f.hidden_extensions == std::vector<std::string>{"zip"});
    // Mixed: one hidden -> all visible.
    set_visible(f, kFolder, false);
    present = summarize_types(entries, false, f);
    toggle_all(f, present);
    CHECK_FALSE(f.hide_folders);
}

TEST_CASE("filter_entries applies types then query, in input order", "[search][filter]") {
    const std::vector<fs::DirEntry> entries = {file("report.txt"), folder("reports"),
                                               file("notes.md"), file("REPORT.PNG"),
                                               file("unreported")};
    Matcher m(compile_query("rep", MatchMode::Substring));
    TypeFilter types;
    std::vector<FilterHit> out = {FilterHit{99, 99}};
    REQUIRE(filter_entries(entries, m, types, out));
    REQUIRE(out.size() == 4);
    CHECK(out[0].index == 0);
    CHECK(out[0].score == 128);
    CHECK(out[1].index == 1);
    CHECK(out[2].index == 3);
    CHECK(out[3].index == 4);

    set_visible(types, ext("png"), false);
    set_visible(types, kFolder, false);
    REQUIRE(filter_entries(entries, m, types, out));
    REQUIRE(out.size() == 2);
    CHECK(out[0].index == 0);
    CHECK(out[1].index == 4);

    Matcher empty;
    REQUIRE(filter_entries(entries, empty, TypeFilter{}, out));
    CHECK(out.size() == entries.size());
    CHECK(out[2].score == 0);

    std::vector<fs::WalkEntry> walked;
    walked.reserve(entries.size());
    for (const fs::DirEntry& e : entries) {
        walked.push_back(fs::WalkEntry{e, 0, 1, 0});
    }
    REQUIRE(filter_entries(walked, m, TypeFilter{}, out));
    CHECK(out.size() == 4);
}

TEST_CASE("filter_entries is cancellable", "[search][filter]") {
    std::vector<fs::DirEntry> entries;
    entries.reserve(20000);
    for (int i = 0; i < 20000; ++i) {
        entries.push_back(file("file" + std::to_string(i) + ".txt"));
    }
    Matcher m(compile_query("file", MatchMode::Fuzzy));
    std::stop_source src;
    src.request_stop();
    std::vector<FilterHit> out = {FilterHit{1, 1}};
    const Result<void> r = filter_entries(entries, m, TypeFilter{}, out, src.get_token());
    REQUIRE_FALSE(r);
    CHECK(r.error().code == ErrorCode::Cancelled);
    CHECK(out.empty());

    std::vector<fs::WalkEntry> walked(10, fs::WalkEntry{file("a"), 0, 1, 0});
    const Result<void> r2 = filter_entries(walked, m, TypeFilter{}, out, src.get_token());
    REQUIRE_FALSE(r2);
    CHECK(r2.error().code == ErrorCode::Cancelled);

    // Not cancelled: all hits.
    REQUIRE(filter_entries(entries, m, TypeFilter{}, out, std::stop_token{}));
    CHECK(out.size() == entries.size());
}
