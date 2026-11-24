// Pure helpers of ops.hpp (no I/O) and UndoStack / make_label.
#include <string>

#include <catch2/catch_test_macros.hpp>

#include <navigator/ops.hpp>

using namespace navigator;
using namespace navigator::ops;

namespace {
std::string stem(std::string_view n, bool d = false) {
    return std::string(split_extension(n, d).stem);
}
std::string ext(std::string_view n, bool d = false) {
    return std::string(split_extension(n, d).extension);
}
} // namespace

TEST_CASE("split_extension follows the documented rules", "[names]") {
    CHECK(stem("a.txt") == "a");
    CHECK(ext("a.txt") == ".txt");
    CHECK(stem("a.b.txt") == "a.b");
    CHECK(ext("a.b.txt") == ".txt");
    CHECK(stem("my.folder", true) == "my.folder");
    CHECK(ext("my.folder", true).empty());
    CHECK(stem(".bashrc") == ".bashrc");
    CHECK(ext(".bashrc").empty());
    CHECK(stem(".config.json") == ".config");
    CHECK(ext(".config.json") == ".json");
    CHECK(stem("a.tar.gz") == "a");
    CHECK(ext("a.tar.gz") == ".tar.gz");
    CHECK(ext("a.TAR.XZ") == ".TAR.XZ");
    CHECK(ext("b.tar.zst") == ".tar.zst");
    CHECK(ext("b.tar.Z") == ".tar.Z");
    CHECK(ext("b.tar.br") == ".tar.br");
    CHECK(ext("b.tar.lz4") == ".tar.lz4");
    CHECK(ext("b.tar.lzma") == ".tar.lzma");
    CHECK(ext("b.tar.bz2") == ".tar.bz2");
    CHECK(ext("b.tar.lz") == ".tar.lz");
    CHECK(ext("b.zip.gz") == ".gz");
    CHECK(ext(".tar.gz") == ".gz"); // dot-file stem ".tar"
    CHECK(stem("notes.") == "notes.");
    CHECK(ext("notes.").empty());
    CHECK(ext("x.0123456789abcdef") == ".0123456789abcdef"); // 16 bytes: an extension
    CHECK(ext("x.0123456789abcdefg").empty());               // 17 bytes: not one
    CHECK(stem("noext") == "noext");
    CHECK(ext("ğüş.şık") == ".şık");
    CHECK(stem_length("photo.jpeg", false) == 5);
    CHECK(stem_length("archive.tar.gz", false) == 7);
    CHECK(stem_length("dir.name", true) == 8);
}

TEST_CASE("keep_both_name counts up and keeps compound extensions", "[names]") {
    CHECK(keep_both_name("a.txt", 2, false) == "a (2).txt");
    CHECK(keep_both_name("a.txt", 0, false) == "a (2).txt");
    CHECK(keep_both_name("a (2).txt", 3, false) == "a (3).txt");
    CHECK(keep_both_name("a (2).txt", 2, false) == "a (2).txt");
    CHECK(keep_both_name("a (x).txt", 2, false) == "a (x) (2).txt");
    CHECK(keep_both_name("a.tar.gz", 2, false) == "a (2).tar.gz");
    CHECK(keep_both_name("my.folder", 4, true) == "my.folder (4)");
    CHECK(keep_both_name(".bashrc", 2, false) == ".bashrc (2)");
    CHECK(keep_both_name("Yeni Klasör", 2, true) == "Yeni Klasör (2)");
    CHECK(keep_both_name("x (12)", 13, true) == "x (13)");
}

TEST_CASE("keep_both_name shortens at a UTF-8 boundary to fit NAME_MAX", "[names]") {
    std::string longname;
    while (longname.size() < 250) {
        longname += "ğ"; // 2 bytes each
    }
    longname += ".txt";
    const std::string r = keep_both_name(longname, 2, false);
    CHECK(r.size() <= kNameMaxBytes);
    CHECK(r.ends_with(" (2).txt"));
    // No split sequence: every 0xC4 lead byte is followed by its continuation byte.
    const std::string stem_part = r.substr(0, r.size() - 8);
    CHECK(stem_part.size() % 2 == 0);
    const std::string ascii(255, 'a');
    CHECK(keep_both_name(ascii, 7, true).size() == kNameMaxBytes);
}

TEST_CASE("validate_new_name", "[names]") {
    CHECK(validate_new_name("") == NameError::Empty);
    CHECK(validate_new_name(".") == NameError::Dot);
    CHECK(validate_new_name("..") == NameError::DotDot);
    CHECK(validate_new_name("a/b") == NameError::ContainsSlash);
    CHECK(validate_new_name(std::string_view("a\0b", 3)) == NameError::ContainsNul);
    CHECK(validate_new_name(std::string(256, 'x')) == NameError::TooLong);
    CHECK(validate_new_name(std::string(255, 'x')) == NameError::None);
    std::string tr;
    for (int i = 0; i < 128; ++i) {
        tr += "ş"; // 256 bytes
    }
    CHECK(validate_new_name(tr) == NameError::TooLong);
    CHECK(validate_new_name(" leading space") == NameError::None);
    CHECK(validate_new_name(".hidden") == NameError::None);
    CHECK(validate_new_name("\xff\xfe") == NameError::None);
    CHECK(validate_new_name("a:b") == NameError::None);
}

TEST_CASE("is_same_or_inside is component-wise and lexical", "[names]") {
    CHECK(is_same_or_inside("/a/b", "/a"));
    CHECK(is_same_or_inside("/a", "/a"));
    CHECK(is_same_or_inside("/a/", "/a"));
    CHECK(is_same_or_inside("/a/./b", "/a/b"));
    CHECK(is_same_or_inside("/a/c/../b", "/a/b"));
    CHECK_FALSE(is_same_or_inside("/ab", "/a"));
    CHECK_FALSE(is_same_or_inside("/a", "/a/b"));
    CHECK(is_same_or_inside("/anything", "/"));
    CHECK_FALSE(is_same_or_inside("rel/a", "/rel"));
}

TEST_CASE("protection_of classifies in enum order", "[names]") {
    ProtectedPaths pp;
    pp.home = "/home/oguz";
    pp.mount_points = {"/", "/media/usb", "/home/oguz/mnt"};
    pp.trash_roots = {"/home/oguz/.local/share/Trash", "/media/usb/.Trash-1000"};
    CHECK(protection_of("/", pp) == Protection::Root);
    CHECK(protection_of("/home/oguz", pp) == Protection::Home);
    CHECK(protection_of("/home/oguz/", pp) == Protection::Home);
    CHECK(protection_of("/home", pp) == Protection::AncestorOfHome);
    CHECK(protection_of("/media/usb", pp) == Protection::MountPoint);
    CHECK(protection_of("/home/oguz/mnt", pp) == Protection::MountPoint);
    CHECK(protection_of("/home/oguz/.local/share/Trash", pp) == Protection::Trash);
    CHECK(protection_of("/home/oguz/.local/share/Trash/files/x", pp) == Protection::Trash);
    CHECK(protection_of("/media/usb/.Trash-1000/info", pp) == Protection::Trash);
    CHECK(protection_of("relative/path", pp) == Protection::Relative);
    CHECK(protection_of("", pp) == Protection::Relative);
    CHECK(protection_of("/home/oguz/Documents", pp) == Protection::None);
    CHECK(protection_of("/media/usb/photos", pp) == Protection::None);
    CHECK(protection_of("/home/oguzhan", pp) == Protection::None);
}

TEST_CASE("make_label picks key, count, name and folder", "[names][undo]") {
    UndoRecord rec;
    rec.kind = OpKind::Move;
    rec.destination = "/x/Photos";
    UndoStep s;
    s.kind = OpKind::Move;
    s.before = "/x/a/ğüş.jpg";
    s.after.path = "/x/Photos/ğüş.jpg";
    rec.steps = {s, s, s};
    UndoLabel l = make_label(rec);
    CHECK(l.key == "undo.label.move");
    CHECK(l.count == 3);
    CHECK(l.name == "ğüş.jpg");
    CHECK(l.folder == "Photos");

    UndoRecord t;
    t.kind = OpKind::Trash;
    UndoStep ts;
    ts.kind = OpKind::Trash;
    ts.before = std::string("/x/bad\xff.txt");
    ts.after.path = "/trash/files/bad.txt";
    t.steps = {ts};
    l = make_label(t);
    CHECK(l.key == "undo.label.trash");
    CHECK(l.name == "bad\xEF\xBF\xBD.txt"); // U+FFFD for the invalid byte
    CHECK(l.folder.empty());

    UndoRecord m;
    m.kind = OpKind::MakeDirectory;
    m.destination = "/x";
    UndoStep ms;
    ms.kind = OpKind::MakeDirectory;
    ms.after.path = "/x/Yeni Klasör";
    m.steps = {ms};
    l = make_label(m);
    CHECK(l.key == "undo.label.make_directory");
    CHECK(l.name == "Yeni Klasör");
    for (OpKind k : {OpKind::Copy, OpKind::Rename, OpKind::CreateSymlink, OpKind::Restore}) {
        m.kind = k;
        CHECK(make_label(m).key == std::string("undo.label.") + std::string(to_string(k)));
    }
}

TEST_CASE("UndoStack: origins, redo clearing, depth, restore", "[undo]") {
    UndoStack st(3);
    CHECK_FALSE(st.can_undo());
    CHECK(st.take_undo() == nullptr);
    UndoRecord r;
    r.kind = OpKind::Copy;
    const UndoId a = st.record_done(r, OpOrigin::User);
    const UndoId b = st.record_done(r, OpOrigin::User);
    CHECK(a.valid());
    CHECK_FALSE(a == b);
    CHECK(st.undo_size() == 2);
    auto taken = st.take_undo();
    REQUIRE(taken);
    CHECK(taken->id == b);
    CHECK(st.undo_size() == 1); // in flight: in neither list
    CHECK(st.redo_size() == 0);
    st.record_done(*taken, OpOrigin::Undo); // the undo's own result
    CHECK(st.redo_size() == 1);
    CHECK(st.can_redo());
    auto redo = st.take_redo();
    REQUIRE(redo);
    st.record_done(*redo, OpOrigin::Redo);
    CHECK(st.undo_size() == 2);
    CHECK(st.redo_size() == 0);
    // A failed submit puts the record back.
    auto again = st.take_undo();
    st.restore(again, OpOrigin::Undo);
    CHECK(st.undo_size() == 2);
    // A user operation clears the redo side.
    auto u = st.take_undo();
    st.record_done(*u, OpOrigin::Undo);
    CHECK(st.redo_size() == 1);
    st.record_done(r, OpOrigin::User);
    CHECK(st.redo_size() == 0);
    // Depth bound: oldest dropped.
    for (int i = 0; i < 5; ++i) {
        st.record_done(r, OpOrigin::User);
    }
    CHECK(st.undo_size() == 3);
    CHECK(st.depth() == 3);
    REQUIRE(st.peek_undo() != nullptr);
    st.clear();
    CHECK(st.undo_size() == 0);
    CHECK(st.peek_undo() == nullptr);
    CHECK(UndoStack().depth() == kDefaultUndoDepth);
}

TEST_CASE("to_string(OpKind) is stable", "[names]") {
    CHECK(to_string(OpKind::DeletePermanently) == "delete_permanently");
    CHECK(to_string(OpKind::MakeDirectory) == "make_directory");
}
