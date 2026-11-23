// transfer.hpp: file URIs, text/uri-list, GNOME / Nautilus / KDE clipboard formats, the
// clipboard offer and read order, and the drop action table. Pure; no I/O.
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <navigator/transfer.hpp>

#include "transfer_internal.hpp"

namespace {

using namespace std::string_literals;
using navigator::ErrorCode;
using navigator::Path;
using namespace navigator::transfer;

// A clipboard owned by another program: mime -> bytes.
ClipboardGetter clipboard(std::map<std::string, std::string> data) {
    return [data = std::move(data)](std::string_view mime) -> std::optional<std::string> {
        auto it = data.find(std::string(mime));
        if (it == data.end()) {
            return std::nullopt;
        }
        return it->second;
    };
}

} // namespace

TEST_CASE("transfer file_uri follows g_filename_to_uri", "[transfer]") {
    CHECK(file_uri("/home/oğuz/a b.txt").value() == "file:///home/o%C4%9Fuz/a%20b.txt");
    CHECK(file_uri("/").value() == "file:///");
    CHECK(file_uri("/a/b/").value() == "file:///a/b");
    CHECK(file_uri("/a/./b/../c").value() == "file:///a/c");
    CHECK(file_uri("/100%/x#y?z").value() == "file:///100%25/x%23y%3Fz");
    // Sub-delims, ':' and '@' stay (GLib's path-safe set); '[' ']' ';' '"' are escaped.
    CHECK(file_uri("/!$&'()*+,:=@-._~").value() == "file:///!$&'()*+,:=@-._~");
    CHECK(file_uri("/a;b[c]\"d").value() == "file:///a%3Bb%5Bc%5D%22d");
    CHECK(file_uri("/x\xff\n").value() == "file:///x%FF%0A");
    CHECK(file_uri("").error().code == ErrorCode::InvalidArgument);
    CHECK(file_uri("rel/a").error().code == ErrorCode::InvalidArgument);
}

TEST_CASE("transfer path_from_file_uri", "[transfer]") {
    CHECK(path_from_file_uri("file:///home/o%C4%9Fuz/a%20b.txt").value() ==
          Path("/home/oğuz/a b.txt"));
    CHECK(path_from_file_uri("file:///o%c4%9fuz").value() == Path("/oğuz"));
    CHECK(path_from_file_uri("FILE:///a").value() == Path("/a"));
    CHECK(path_from_file_uri("file://localhost/a").value() == Path("/a"));
    CHECK(path_from_file_uri("file://LocalHost/a").value() == Path("/a"));
    CHECK(path_from_file_uri("file:/a/b").value() == Path("/a/b")); // RFC 8089 minimal form
    CHECK(path_from_file_uri("file:///a/./b/../c/").value() == Path("/a/c"));
    CHECK(path_from_file_uri("file:///").value() == Path("/"));
    CHECK(path_from_file_uri("file:///raw ğ").error().code == ErrorCode::InvalidArgument);

    CHECK(path_from_file_uri("file://server/share/a").error().code == ErrorCode::Unsupported);
    for (const char* bad :
         {"", "/home/u/a", "http://x/a", "smb://server/share", "file:", "file://",
          "file://localhost", "file:a", "file:///a%4", "file:///a%G0", "file:///a%00b",
          "file:///a?x=1", "file:///a#frag", "1file:///a", "file\n:///a"}) {
        INFO(bad);
        CHECK(path_from_file_uri(bad).error().code == ErrorCode::InvalidArgument);
    }

    SECTION("round trip of every byte") {
        std::string all = "/";
        for (int c = 1; c < 256; ++c) {
            if (c != '/') {
                all.push_back(static_cast<char>(c));
            }
        }
        CHECK(path_from_file_uri(file_uri(Path(all)).value()).value() == Path(all));
    }
}

TEST_CASE("transfer text/uri-list", "[transfer]") {
    CHECK(encode_uri_list(std::vector<std::string>{"file:///a", "file:///b"}) ==
          "file:///a\r\nfile:///b\r\n");
    CHECK(encode_uri_list(std::vector<std::string>{}).empty());
    CHECK(encode_uri_list(std::vector<Path>{"/a b", "/ğ"}).value() ==
          "file:///a%20b\r\nfile:///%C4%9F\r\n");
    CHECK(encode_uri_list(std::vector<Path>{"/a", "rel"}).error().code ==
          ErrorCode::InvalidArgument);

    const UriList d = decode_uri_list("# comment\r\n"
                                      "file:///a%20b\r\n"
                                      "\r\n"
                                      "  file:///c\t\n"
                                      "file:///a%20b\n" // duplicate
                                      "https://example.org/x\n"
                                      "file://server/share\n"
                                      "not a uri\n"
                                      "file:///bad%zz\n"
                                      "file:///last");
    CHECK(d.paths == std::vector<Path>{"/a b", "/c", "/last"});
    CHECK(d.remote == std::vector<std::string>{"https://example.org/x", "file://server/share"});
    CHECK(d.malformed == 2);
    CHECK(decode_uri_list("").paths.empty());
    // Round trip.
    const std::vector<Path> paths{"/home/oğuz/a b.txt", "/tmp/x%y", "/\xff"};
    CHECK(decode_uri_list(encode_uri_list(paths).value()).paths == paths);
}

TEST_CASE("transfer GNOME / Nautilus copied-files formats", "[transfer]") {
    const FileClipboard copy{ClipboardAction::Copy, {"/home/oğuz/a b.txt", "/home/oğuz/c"}};
    CHECK(encode_gnome_copied_files(copy).value() ==
          "copy\nfile:///home/o%C4%9Fuz/a%20b.txt\nfile:///home/o%C4%9Fuz/c");
    const FileClipboard cut{ClipboardAction::Cut, {"/x"}};
    CHECK(encode_gnome_copied_files(cut).value() == "cut\nfile:///x");
    CHECK(encode_gnome_copied_files(FileClipboard{}).error().code == ErrorCode::InvalidArgument);
    CHECK(decode_gnome_copied_files(encode_gnome_copied_files(copy).value()) == copy);
    CHECK(decode_gnome_copied_files(encode_gnome_copied_files(cut).value()) == cut);

    SECTION("samples from GNOME Files, Nemo and Caja") {
        // Nautilus 43+: no trailing newline.
        CHECK(decode_gnome_copied_files("copy\nfile:///home/u/Belgeler/rapor%20son.pdf") ==
              FileClipboard{ClipboardAction::Copy, {"/home/u/Belgeler/rapor son.pdf"}});
        // Nemo / Caja: trailing newline; CRLF tolerated.
        CHECK(decode_gnome_copied_files("cut\r\nfile:///a\r\nfile:///b\r\n") ==
              FileClipboard{ClipboardAction::Cut, {"/a", "/b"}});
        CHECK(decode_gnome_copied_files("cut  \nfile:///a") ==
              FileClipboard{ClipboardAction::Cut, {"/a"}});
        CHECK_FALSE(decode_gnome_copied_files("move\nfile:///a").has_value());
        CHECK_FALSE(decode_gnome_copied_files("copy").has_value());
        CHECK_FALSE(decode_gnome_copied_files("copy\nhttps://x/y").has_value());
        CHECK_FALSE(decode_gnome_copied_files("").has_value());
    }
    SECTION("Nautilus text form (GNOME Files 3.28-42)") {
        CHECK(decode_nautilus_text("x-special/nautilus-clipboard\ncut\nfile:///home/u/a%20b\n") ==
              FileClipboard{ClipboardAction::Cut, {"/home/u/a b"}});
        CHECK(decode_nautilus_text("x-special/nautilus-clipboard\r\ncopy\r\nfile:///a\r\n") ==
              FileClipboard{ClipboardAction::Copy, {"/a"}});
        CHECK_FALSE(decode_nautilus_text("copy\nfile:///a").has_value());
        CHECK_FALSE(
            decode_nautilus_text("x-special/nautilus-clipboard\nlink\nfile:///a").has_value());
        CHECK_FALSE(decode_nautilus_text("x-special/nautilus-clipboard\ncopy\n").has_value());
        CHECK_FALSE(decode_nautilus_text("/home/u/a").has_value());
    }
}

TEST_CASE("transfer clipboard offer order and contents", "[transfer]") {
    const FileClipboard copy{ClipboardAction::Copy, {"/home/oğuz/a b", "/c"}};
    auto offer = make_clipboard_offer(copy);
    REQUIRE(offer.has_value());
    const std::vector<std::pair<std::string, std::string>> expected{
        {"x-special/gnome-copied-files", "copy\nfile:///home/o%C4%9Fuz/a%20b\nfile:///c"},
        {"text/uri-list", "file:///home/o%C4%9Fuz/a%20b\r\nfile:///c\r\n"},
        {"text/plain;charset=utf-8", "/home/oğuz/a b\n/c"},
        {"text/plain", "/home/oğuz/a b\n/c"},
        {"UTF8_STRING", "/home/oğuz/a b\n/c"},
    };
    CHECK(*offer == expected);

    auto cut = make_clipboard_offer(FileClipboard{ClipboardAction::Cut, {"/x"}});
    REQUIRE(cut.has_value());
    REQUIRE(cut->size() == 6);
    CHECK((*cut)[0] ==
          std::pair<std::string, std::string>{"x-special/gnome-copied-files", "cut\nfile:///x"});
    CHECK((*cut)[1].first == "text/uri-list");
    CHECK((*cut)[2] == std::pair<std::string, std::string>{"application/x-kde-cutselection", "1"});
    CHECK((*cut)[3].first == "text/plain;charset=utf-8");

    SECTION("a non-UTF-8 path turns the text form into URIs") {
        auto o = make_clipboard_offer(FileClipboard{ClipboardAction::Copy, {"/a", "/b\xff"}});
        REQUIRE(o.has_value());
        CHECK(o->back().second == "file:///a\nfile:///b%FF");
    }
    CHECK(make_clipboard_offer(FileClipboard{}).error().code == ErrorCode::InvalidArgument);
    CHECK(make_clipboard_offer(FileClipboard{ClipboardAction::Copy, {"rel"}}).error().code ==
          ErrorCode::InvalidArgument);

    SECTION("our own offer reads back") {
        std::map<std::string, std::string> data(offer->begin(), offer->end());
        CHECK(read_clipboard(clipboard(data)) == copy);
        std::map<std::string, std::string> cut_data(cut->begin(), cut->end());
        CHECK(read_clipboard(clipboard(cut_data)) == FileClipboard{ClipboardAction::Cut, {"/x"}});
    }
}

TEST_CASE("transfer read_clipboard acceptance order", "[transfer]") {
    // gnome-copied-files wins over uri-list.
    CHECK(read_clipboard(clipboard({{"x-special/gnome-copied-files", "cut\nfile:///g"},
                                    {"text/uri-list", "file:///u\r\n"}})) ==
          FileClipboard{ClipboardAction::Cut, {"/g"}});
    // A broken gnome payload falls through to uri-list.
    CHECK(read_clipboard(clipboard(
              {{"x-special/gnome-copied-files", "garbage"}, {"text/uri-list", "file:///u\r\n"}})) ==
          FileClipboard{ClipboardAction::Copy, {"/u"}});
    // Dolphin: uri-list + KDE cut marker.
    CHECK(read_clipboard(clipboard({{"text/uri-list", "file:///home/u/a\r\n"},
                                    {"application/x-kde-cutselection", "1"}})) ==
          FileClipboard{ClipboardAction::Cut, {"/home/u/a"}});
    CHECK(read_clipboard(clipboard({{"text/uri-list", "file:///home/u/a\r\n"},
                                    {"application/x-kde-cutselection", "0"}})) ==
          FileClipboard{ClipboardAction::Copy, {"/home/u/a"}});
    // GNOME Files 3.28-42 text form, under any text target.
    CHECK(read_clipboard(
              clipboard({{"text/plain", "x-special/nautilus-clipboard\ncopy\nfile:///n\n"}})) ==
          FileClipboard{ClipboardAction::Copy, {"/n"}});
    CHECK(read_clipboard(
              clipboard({{"UTF8_STRING", "x-special/nautilus-clipboard\ncut\nfile:///n\n"}})) ==
          FileClipboard{ClipboardAction::Cut, {"/n"}});
    // Plain-text paths are text, never files.
    CHECK_FALSE(read_clipboard(clipboard({{"text/plain;charset=utf-8", "/home/u/a"},
                                          {"text/plain", "/home/u/a"}}))
                    .has_value());
    // Only remote URIs: nothing to paste.
    CHECK_FALSE(
        read_clipboard(clipboard({{"text/uri-list", "https://example.org/\r\n"}})).has_value());
    CHECK_FALSE(read_clipboard(clipboard({})).has_value());
    CHECK_FALSE(read_clipboard(ClipboardGetter{}).has_value());
    CHECK_FALSE(read_clipboard([](std::string_view) -> std::optional<std::string> {
                    throw std::runtime_error("clipboard owner vanished");
                }).has_value());
}

TEST_CASE("transfer strict UTF-8 check", "[transfer]") {
    using navigator::transfer_internal::is_valid_utf8;
    CHECK(is_valid_utf8("ascii"));
    CHECK(is_valid_utf8("oğuz İŞÇ €𝄞"));
    CHECK_FALSE(is_valid_utf8("\xff"));
    CHECK_FALSE(is_valid_utf8("\xc0\xaf"));         // overlong '/'
    CHECK_FALSE(is_valid_utf8("\xe0\x80\xaf"));     // overlong
    CHECK_FALSE(is_valid_utf8("\xed\xa0\x80"));     // surrogate
    CHECK_FALSE(is_valid_utf8("\xf4\x90\x80\x80")); // > U+10FFFF
    CHECK_FALSE(is_valid_utf8("\xc4"));             // truncated
}

TEST_CASE("transfer drop action table", "[transfer]") {
    static_assert(choose_drop_action(DropContext{.internal = true, .same_filesystem = true}) ==
                  DropAction::Move);
    static_assert(choose_drop_action(DropContext{}) == DropAction::Copy);
    DropContext c;
    c.internal = true;
    c.same_filesystem = true;
    CHECK(choose_drop_action(c) == DropAction::Move);
    c.same_filesystem = false;
    CHECK(choose_drop_action(c) == DropAction::Copy);
    c.shift = true;
    CHECK(choose_drop_action(c) == DropAction::Move);
    c.ctrl = true;
    CHECK(choose_drop_action(c) == DropAction::Link);
    c.shift = false;
    CHECK(choose_drop_action(c) == DropAction::Copy);
    c.alt = true;
    CHECK(choose_drop_action(c) == DropAction::Ask);
    c = DropContext{};
    c.into_source_folder = true;
    CHECK(choose_drop_action(c) == DropAction::None);
    c.shift = true;
    CHECK(choose_drop_action(c) == DropAction::Move);
    c.into_itself = true;
    CHECK(choose_drop_action(c) == DropAction::None);
    c = DropContext{};
    c.target_writable = false;
    c.alt = true;
    CHECK(choose_drop_action(c) == DropAction::None);
}
