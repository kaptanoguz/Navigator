// Pure trash helpers (trash.hpp): DeletionDate, GIO-compatible Path escaping, .trashinfo text,
// topdir names and Path= values, UTF-8 safe name cutting. No I/O.
#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include <navigator/trash.hpp>

#include "trash_internal.hpp"

namespace {

using namespace std::string_literals;
using namespace std::string_view_literals;
using navigator::ErrorCode;
using navigator::Path;
using navigator::trash::escape_path;
using navigator::trash::format_deletion_date;
using navigator::trash::format_trashinfo;
using navigator::trash::info_path_for;
using navigator::trash::LocalDateTime;
using navigator::trash::original_path_from;
using navigator::trash::parse_deletion_date;
using navigator::trash::parse_trashinfo;
using navigator::trash::TrashInfo;
using navigator::trash::unescape_path;
namespace ti = navigator::trash_internal;

} // namespace

TEST_CASE("trash DeletionDate format and parse", "[trash][format]") {
    const LocalDateTime t{2026, 3, 7, 9, 5, 1};
    CHECK(format_deletion_date(t) == "2026-03-07T09:05:01");
    CHECK(format_deletion_date(LocalDateTime{}) == "1970-01-01T00:00:00");
    CHECK(parse_deletion_date("2026-03-07T09:05:01") == t);

    SECTION("lenient: fraction and zone ignored (GIO, KDE)") {
        CHECK(parse_deletion_date("2026-03-07T09:05:01.123") == t);
        CHECK(parse_deletion_date("2026-03-07T09:05:01Z") == t);
        CHECK(parse_deletion_date("2026-03-07T09:05:01+03:00") == t);
        CHECK(parse_deletion_date("2026-03-07T09:05:01-0130") == t);
        CHECK(parse_deletion_date("2026-03-07T09:05:01.5+03") == t);
        CHECK(parse_deletion_date("2026-12-31T23:59:60") ==
              LocalDateTime{2026, 12, 31, 23, 59, 60});
    }
    SECTION("rejected") {
        for (const char* bad :
             {"", "2026-03-07", "2026-03-07 09:05:01", "2026-13-07T09:05:01", "2026-00-07T09:05:01",
              "2026-03-32T09:05:01", "2026-03-00T09:05:01", "2026-03-07T24:05:01",
              "2026-03-07T09:60:01", "2026-03-07T09:05:61", "26-03-07T09:05:01",
              "2026-03-07T09:05:01x", "2026-03-07T09:05:01.", "2026-03-07T09:05:01+3",
              "2026-3-07T09:05:01", "2026-03-07T09:05:01+03:00:00"}) {
            INFO(bad);
            CHECK_FALSE(parse_deletion_date(bad).has_value());
        }
    }
}

TEST_CASE("trash Path escaping matches g_uri_escape_string(path, \"/\", FALSE)",
          "[trash][format]") {
    CHECK(escape_path("/home/oğuz/a b.txt") == "/home/o%C4%9Fuz/a%20b.txt");
    CHECK(escape_path("/ŞİÖÇÜĞışöçüğ") ==
          "/%C5%9E%C4%B0%C3%96%C3%87%C3%9C%C4%9E%C4%B1%C5%9F%C3%B6%C3%A7%C3%BC%C4%9F");
    CHECK(escape_path("/100%/x") == "/100%25/x");
    CHECK(escape_path("/a-b.c_d~e/F9") == "/a-b.c_d~e/F9");
    // Every reserved and sub-delim character is escaped (GIO keeps only unreserved and '/').
    CHECK(escape_path("/!$&'()*+,;=:@?#[]") ==
          "/%21%24%26%27%28%29%2A%2B%2C%3B%3D%3A%40%3F%23%5B%5D");
    // Invalid UTF-8 and control bytes are encoded byte by byte, uppercase hex.
    CHECK(escape_path("/x\xff\xfe\x01\x7f") == "/x%FF%FE%01%7F");
    CHECK(escape_path("/new\nline\ttab") == "/new%0Aline%09tab");

    SECTION("unescape") {
        CHECK(unescape_path("/home/o%C4%9Fuz/a%20b.txt") == "/home/oğuz/a b.txt"s);
        CHECK(unescape_path("/o%c4%9fuz") == "/oğuz"s);          // lowercase hex
        CHECK(unescape_path("/raw space/ğ") == "/raw space/ğ"s); // KDE leaves some raw
        CHECK(unescape_path("/x%FF") == "/x\xff"s);
        CHECK_FALSE(unescape_path("/a%4").has_value());
        CHECK_FALSE(unescape_path("/a%").has_value());
        CHECK_FALSE(unescape_path("/a%G1").has_value());
        CHECK_FALSE(unescape_path("/a%1G").has_value());
        CHECK_FALSE(unescape_path("/a%00b").has_value());
        CHECK_FALSE(unescape_path("/a\0b"sv).has_value());
    }
    SECTION("round trip of every byte") {
        std::string all = "/";
        for (int c = 1; c < 256; ++c) {
            all.push_back(static_cast<char>(c));
        }
        CHECK(unescape_path(escape_path(all)) == all);
    }
}

TEST_CASE("trashinfo exact text and parsing", "[trash][format]") {
    const TrashInfo info{"/home/oğuz/Belgeler/rapor son%.pdf",
                         LocalDateTime{2026, 11, 23, 14, 5, 9}};
    CHECK(format_trashinfo(info) == "[Trash Info]\n"
                                    "Path=/home/o%C4%9Fuz/Belgeler/rapor%20son%25.pdf\n"
                                    "DeletionDate=2026-11-23T14:05:09\n");
    CHECK(format_trashinfo(TrashInfo{"rel/a b", std::nullopt}) == "[Trash Info]\nPath=rel/a%20b\n");
    auto back = parse_trashinfo(format_trashinfo(info));
    REQUIRE(back.has_value());
    CHECK(*back == info);

    SECTION("what GIO / Nautilus write") {
        auto p = parse_trashinfo("[Trash Info]\n"
                                 "Path=/home/o%C4%9Fuz/%C4%B0ndirilenler/foto%C4%9Fraf%201.jpg\n"
                                 "DeletionDate=2026-11-20T08:00:59\n");
        REQUIRE(p.has_value());
        CHECK(p->path == "/home/oğuz/İndirilenler/fotoğraf 1.jpg");
        CHECK(p->deletion_date == LocalDateTime{2026, 11, 20, 8, 0, 59});
    }
    SECTION("KDE style: raw characters, CRLF, no trailing newline") {
        auto p = parse_trashinfo("[Trash Info]\r\nPath=/home/u/a b+c\r\nDeletionDate="
                                 "2026-11-20T08:00:59");
        REQUIRE(p.has_value());
        CHECK(p->path == "/home/u/a b+c");
        CHECK(p->deletion_date == LocalDateTime{2026, 11, 20, 8, 0, 59});
    }
    SECTION("relative Path of a per-volume trash") {
        auto p = parse_trashinfo("[Trash Info]\nPath=Fotolar/a%20b.png\nDeletionDate=x\n");
        REQUIRE(p.has_value());
        CHECK(p->path == "Fotolar/a b.png");
        CHECK_FALSE(p->deletion_date.has_value()); // unparsable date: still an item
    }
    SECTION("lines before the group, other groups, comments, first key wins") {
        auto p = parse_trashinfo("# comment\nPath=/ignored\n\n[Other]\nPath=/also-ignored\n"
                                 "[Trash Info]\n# c\nFoo=bar\nPath = /first\nPath=/second\n"
                                 "DeletionDate=2026-01-02T03:04:05\n"
                                 "DeletionDate=2027-01-02T03:04:05\n[Next]\nPath=/no\n");
        REQUIRE(p.has_value());
        CHECK(p->path == "/first");
        CHECK(p->deletion_date == LocalDateTime{2026, 1, 2, 3, 4, 5});
    }
    SECTION("errors") {
        CHECK(parse_trashinfo("").error().code == ErrorCode::InvalidArgument);
        CHECK(parse_trashinfo("Path=/a\n").error().code == ErrorCode::InvalidArgument);
        CHECK(parse_trashinfo("[Trash Info]\nDeletionDate=2026-01-02T03:04:05\n").error().code ==
              ErrorCode::InvalidArgument);
        CHECK(parse_trashinfo("[Trash Info]\nPath=\n").error().code == ErrorCode::InvalidArgument);
        CHECK(parse_trashinfo("[Trash Info]\nPath=/a%ZZ\n").error().code ==
              ErrorCode::InvalidArgument);
        CHECK(parse_trashinfo("[Trash Info]\nPath=/a%00\n").error().code ==
              ErrorCode::InvalidArgument);
        CHECK(parse_trashinfo("[Other]\nPath=/a\n[Trash Info]\n").error().code ==
              ErrorCode::InvalidArgument);
    }
}

TEST_CASE("trash topdir names and Path= values", "[trash][format]") {
    using navigator::trash::shared_topdir_trash;
    using navigator::trash::user_topdir_trash;
    CHECK(shared_topdir_trash("/media/u/STICK", 1000) == Path("/media/u/STICK/.Trash/1000"));
    CHECK(user_topdir_trash("/media/u/STICK", 1000) == Path("/media/u/STICK/.Trash-1000"));

    CHECK(info_path_for("/home/u/a b", "").value() == "/home/u/a b");
    CHECK(info_path_for("/media/u/STICK/Fotolar/a.png", "/media/u/STICK").value() ==
          "Fotolar/a.png");
    CHECK(info_path_for("/media/u/STICK/a", "/media/u/STICK/").value() == "a");
    CHECK(info_path_for("/data/x", "/").value() == "data/x");
    CHECK(info_path_for("/media/u/STICKY/a", "/media/u/STICK").error().code ==
          ErrorCode::InvalidArgument);
    CHECK(info_path_for("/media/u/STICK", "/media/u/STICK").error().code ==
          ErrorCode::InvalidArgument);
    CHECK(info_path_for("rel", "").error().code == ErrorCode::InvalidArgument);

    CHECK(original_path_from("/home/u/a", "").value() == Path("/home/u/a"));
    CHECK(original_path_from("/elsewhere/a", "/media/s").value() == Path("/elsewhere/a"));
    CHECK(original_path_from("Fotolar/a.png", "/media/s").value() ==
          Path("/media/s/Fotolar/a.png"));
    CHECK(original_path_from("a/./b/../c", "/media/s").value() == Path("/media/s/a/c"));
    CHECK(original_path_from("data/x", "/").value() == Path("/data/x"));
    CHECK(original_path_from("../etc/passwd", "/media/s").error().code ==
          ErrorCode::InvalidArgument);
    CHECK(original_path_from("a/../../x", "/media/s").error().code == ErrorCode::InvalidArgument);
    CHECK(original_path_from(".", "/media/s").error().code == ErrorCode::InvalidArgument);
    CHECK(original_path_from("rel", "").error().code == ErrorCode::InvalidArgument);
    CHECK(original_path_from("", "/media/s").error().code == ErrorCode::InvalidArgument);
}

TEST_CASE("trash candidate names fit NAME_MAX at a code point boundary", "[trash][format]") {
    CHECK(ti::candidate_name("a.txt", 1) == "a.txt");
    CHECK(ti::candidate_name("a.txt", 2) == "a.txt.2");
    CHECK(ti::candidate_name("a.txt", 13) == "a.txt.13");

    std::string turkish; // 2-byte "ğ" repeated: 254 bytes
    for (int i = 0; i < 127; ++i) {
        turkish += "ğ";
    }
    const std::string c1 = ti::candidate_name(turkish, 1);
    CHECK(c1.size() + 10 <= 255);
    CHECK(c1.size() == 244); // 245 would split a "ğ"
    CHECK(c1 == turkish.substr(0, 244));
    const std::string c7 = ti::candidate_name(turkish, 7);
    CHECK(c7.size() + 10 <= 255);
    CHECK(c7.ends_with(".7"));

    const std::string ascii(255, 'x');
    CHECK(ti::candidate_name(ascii, 1).size() == 245);
    CHECK(ti::candidate_name(ascii, 100).size() == 245);

    CHECK(ti::cut_utf8("aé", 2) == "a");                            // never half of "é"
    CHECK(ti::cut_utf8("a\xf0\x9f\x98\x80", 4) == "a");             // never 3/4 of an emoji
    CHECK(ti::cut_utf8("a\x80\x80\x80\x80\x80", 3) == "a\x80\x80"); // invalid: byte-wise
    CHECK(ti::cut_utf8("abc", 10) == "abc");
}
