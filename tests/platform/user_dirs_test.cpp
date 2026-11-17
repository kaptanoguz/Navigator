// parse_user_dirs (pure parser matrix), read_user_dirs on temporary roots, user_home against the
// passwd database (current user, unknown user, invalid names).
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

#include <navigator/platform.hpp>

#include "test_support.hpp"

namespace {

using navigator::ErrorCode;
using navigator::Path;
using navigator::platform::UserDirs;
using navigator::platform_test::TempDir;
using navigator::platform_test::write_file;
namespace stdfs = std::filesystem;
namespace platform = navigator::platform;

const Path kHome("/home/user");

UserDirs parse(std::string_view text, const Path& home = kHome) {
    auto result = platform::parse_user_dirs(text, home);
    REQUIRE(result.has_value());
    return *result;
}

UserDirs only_desktop_default(const Path& home = kHome) {
    UserDirs dirs;
    dirs.desktop = home / "Desktop";
    return dirs;
}

// The file xdg-user-dirs-update writes for an English (C/en_US) session.
constexpr std::string_view kGnomeEnglish =
    "# This file is written by xdg-user-dirs-update\n"
    "# If you want to change or add directories, just edit the line you're\n"
    "# interested in. All local changes will be retained on the next run.\n"
    "# Format is XDG_xxx_DIR=\"$HOME/yyy\", where yyy is a shell-escaped\n"
    "# homedir-relative path, or XDG_xxx_DIR=\"/yyy\", where /yyy is an\n"
    "# absolute path. No other format is supported.\n"
    "# \n"
    "XDG_DESKTOP_DIR=\"$HOME/Desktop\"\n"
    "XDG_DOWNLOAD_DIR=\"$HOME/Downloads\"\n"
    "XDG_TEMPLATES_DIR=\"$HOME/Templates\"\n"
    "XDG_PUBLICSHARE_DIR=\"$HOME/Public\"\n"
    "XDG_DOCUMENTS_DIR=\"$HOME/Documents\"\n"
    "XDG_MUSIC_DIR=\"$HOME/Music\"\n"
    "XDG_PICTURES_DIR=\"$HOME/Pictures\"\n"
    "XDG_VIDEOS_DIR=\"$HOME/Videos\"\n";

// The same file for a Turkish (tr_TR.UTF-8) session (Linux Mint / GNOME translations).
constexpr std::string_view kGnomeTurkish = "# This file is written by xdg-user-dirs-update\n"
                                           "# \n"
                                           "XDG_DESKTOP_DIR=\"$HOME/Masaüstü\"\n"
                                           "XDG_DOWNLOAD_DIR=\"$HOME/İndirilenler\"\n"
                                           "XDG_TEMPLATES_DIR=\"$HOME/Şablonlar\"\n"
                                           "XDG_PUBLICSHARE_DIR=\"$HOME/Genel\"\n"
                                           "XDG_DOCUMENTS_DIR=\"$HOME/Belgeler\"\n"
                                           "XDG_MUSIC_DIR=\"$HOME/Müzik\"\n"
                                           "XDG_PICTURES_DIR=\"$HOME/Resimler\"\n"
                                           "XDG_VIDEOS_DIR=\"$HOME/Videolar\"\n";

} // namespace

TEST_CASE("parse_user_dirs: GNOME default file, English", "[user_dirs]") {
    const UserDirs dirs = parse(kGnomeEnglish);
    CHECK(dirs.desktop == kHome / "Desktop");
    CHECK(dirs.documents == kHome / "Documents");
    CHECK(dirs.downloads == kHome / "Downloads");
    CHECK(dirs.music == kHome / "Music");
    CHECK(dirs.pictures == kHome / "Pictures");
    CHECK(dirs.videos == kHome / "Videos");
    CHECK(dirs.templates == kHome / "Templates");
    CHECK(dirs.public_share == kHome / "Public");
}

TEST_CASE("parse_user_dirs: GNOME default file, Turkish names are read, not guessed",
          "[user_dirs]") {
    const UserDirs dirs = parse(kGnomeTurkish);
    CHECK(dirs.desktop == kHome / "Masaüstü");
    CHECK(dirs.documents == kHome / "Belgeler");
    CHECK(dirs.downloads == kHome / "İndirilenler");
    CHECK(dirs.music == kHome / "Müzik");
    CHECK(dirs.pictures == kHome / "Resimler");
    CHECK(dirs.videos == kHome / "Videolar");
    CHECK(dirs.templates == kHome / "Şablonlar");
    CHECK(dirs.public_share == kHome / "Genel");
    // Byte-exact UTF-8 (no case folding or normalization).
    CHECK(dirs.downloads.filename().native() == "\xC4\xB0ndirilenler");
}

TEST_CASE("parse_user_dirs: defaults", "[user_dirs]") {
    CHECK(parse("") == only_desktop_default());
    CHECK(parse("\n\n# only comments\n   \t\n") == only_desktop_default());
    // Desktop is the only default; it is not applied when the file sets it.
    UserDirs music_only = only_desktop_default();
    music_only.music = kHome / "Music";
    CHECK(parse("XDG_MUSIC_DIR=\"$HOME/Music\"\n") == music_only);
    CHECK(parse("XDG_DESKTOP_DIR=\"/data/desk\"\n").desktop == Path("/data/desk"));
    // An invalid desktop line does not count as set.
    CHECK(parse("XDG_DESKTOP_DIR=\"desk\"\n").desktop == kHome / "Desktop");
}

TEST_CASE("parse_user_dirs: values", "[user_dirs]") {
    const auto music = [](std::string_view value) {
        return parse("XDG_MUSIC_DIR=" + std::string(value) + "\n").music;
    };
    CHECK(music(R"("$HOME")") == kHome); // disabled: kept as home
    CHECK(music(R"("$HOME/")") == kHome);
    CHECK(music(R"("$HOME//Music//")") == kHome / "Music");
    CHECK(music(R"("$HOME/a/./b/../c")") == kHome / "a" / "c");
    CHECK(music(R"("/")") == Path("/"));
    CHECK(music(R"("/mnt/music/")") == Path("/mnt/music"));
    CHECK(music(R"("/mnt//x/../music")") == Path("/mnt/music"));
    CHECK(music(R"("$HOME/My Music")") == kHome / "My Music");
    CHECK(music(R"("$HOME/Music" # trailing comment)") == kHome / "Music");
    CHECK(music("\"$HOME/Music\"\r") == kHome / "Music"); // CRLF files: CR is after the quote

    // Rejected values leave the field empty.
    CHECK(music(R"("Music")").empty());         // relative
    CHECK(music(R"("./Music")").empty());       // relative
    CHECK(music(R"("~/Music")").empty());       // no tilde expansion
    CHECK(music(R"("$HOMEMusic")").empty());    // not $HOME followed by '/'
    CHECK(music(R"("${HOME}/Music")").empty()); // only the plain $HOME form
    CHECK(music(R"("$USER/Music")").empty());   // other variables
    CHECK(music(R"("\$HOME/Music")").empty());  // escaped '$' is not the variable
    CHECK(music(R"("")").empty());
    CHECK(music(R"($HOME/Music)").empty());            // missing quotes
    CHECK(music(R"("$HOME/Music)").empty());           // missing closing quote
    CHECK(music(R"('$HOME/Music')").empty());          // single quotes
    CHECK(music(R"( "$HOME/Music")").empty());         // space after '='
    CHECK(music("\"/a\\").empty());                    // escape at end of line, no closing quote
    CHECK(music(std::string("\"/a\0b\"", 6)).empty()); // embedded NUL
}

TEST_CASE("parse_user_dirs: backslash escapes", "[user_dirs]") {
    const auto music = [](std::string_view value) {
        return parse("XDG_MUSIC_DIR=" + std::string(value) + "\n").music;
    };
    CHECK(music(R"("$HOME/a\"b")") == kHome / "a\"b");
    CHECK(music(R"("$HOME/a\\b")") == kHome / "a\\b");
    CHECK(music(R"("$HOME/cost\$5")") == kHome / "cost$5");
    CHECK(music(R"("$HOME/\`x\`")") == kHome / "`x`");
    CHECK(music(R"("$HOME/\a\ b")") == kHome / "a b"); // any character: itself
    CHECK(music(R"("/abs\"q" ignored")") == Path("/abs\"q"));
}

TEST_CASE("parse_user_dirs: lines, comments, keys", "[user_dirs]") {
    SECTION("leading blanks skipped, comments ignored") {
        const UserDirs dirs = parse("  \tXDG_MUSIC_DIR=\"$HOME/M\"\n"
                                    "#XDG_VIDEOS_DIR=\"$HOME/V\"\n"
                                    "   # XDG_PICTURES_DIR=\"$HOME/P\"\n");
        CHECK(dirs.music == kHome / "M");
        CHECK(dirs.videos.empty());
        CHECK(dirs.pictures.empty());
    }
    SECTION("malformed lines are skipped, others still parsed") {
        const UserDirs dirs = parse("garbage\n"
                                    "=\n"
                                    "XDG_MUSIC_DIR\n"
                                    "XDG_MUSIC_DIR =\"$HOME/x\"\n"
                                    "xdg_music_dir=\"$HOME/lower\"\n"
                                    "export XDG_MUSIC_DIR=\"$HOME/export\"\n"
                                    "XDG_DOCUMENTS_DIR=\"$HOME/Docs\"\n"
                                    "XDG_VIDEO_DIR=\"$HOME/typo\"\n"
                                    "XDG_UNKNOWN_DIR=\"$HOME/Unknown\"\n"
                                    "XDG_DOWNLOADS_DIR=\"$HOME/plural\"\n");
        UserDirs expected = only_desktop_default();
        expected.documents = kHome / "Docs";
        CHECK(dirs == expected);
    }
    SECTION("duplicate keys: the last valid line wins") {
        CHECK(parse("XDG_MUSIC_DIR=\"$HOME/one\"\nXDG_MUSIC_DIR=\"/two\"\n").music == Path("/two"));
        CHECK(parse("XDG_MUSIC_DIR=\"$HOME/one\"\nXDG_MUSIC_DIR=\"relative\"\n").music ==
              kHome / "one");
        CHECK(parse("XDG_DESKTOP_DIR=\"/d\"\nXDG_DESKTOP_DIR=\"bad\"\n").desktop == Path("/d"));
    }
    SECTION("no trailing newline, last line still read") {
        CHECK(parse("XDG_MUSIC_DIR=\"$HOME/M\"").music == kHome / "M");
    }
}

TEST_CASE("parse_user_dirs: home argument", "[user_dirs]") {
    CHECK(platform::parse_user_dirs("", Path()).error().code == ErrorCode::InvalidArgument);
    CHECK(platform::parse_user_dirs("", Path("home/user")).error().code ==
          ErrorCode::InvalidArgument);
    // home is normalized like the values, so "$HOME/" compares equal to it.
    const UserDirs dirs = parse("XDG_MUSIC_DIR=\"$HOME/\"\n", Path("/home/user/"));
    CHECK(dirs.music == kHome);
    CHECK(dirs.desktop == kHome / "Desktop");
    CHECK(parse("", Path("/")).desktop == Path("/Desktop"));
}

TEST_CASE("parse_user_dirs: a large text is parsed in full", "[user_dirs]") {
    std::string text;
    for (int i = 0; i < 20000; ++i) {
        text += "# filler line " + std::to_string(i) + "\n";
    }
    text += "XDG_MUSIC_DIR=\"$HOME/End\"\n";
    CHECK(parse(text).music == kHome / "End"); // the size cap belongs to read_user_dirs only
}

TEST_CASE("read_user_dirs: temporary roots", "[user_dirs]") {
    TempDir dir;
    const Path home = dir / "home";
    const Path config = dir / "config";
    const Path file = config / "user-dirs.dirs";

    SECTION("file present") {
        write_file(file, kGnomeTurkish);
        auto dirs = platform::read_user_dirs(home, config);
        REQUIRE(dirs.has_value());
        CHECK(*dirs == parse(kGnomeTurkish, home));
    }
    SECTION("missing file or missing config dir: defaults") {
        CHECK(platform::read_user_dirs(home, config).value() == only_desktop_default(home));
        write_file(dir / "plainfile", "x");
        CHECK(platform::read_user_dirs(home, dir / "plainfile").value() ==
              only_desktop_default(home)); // ENOTDIR
    }
    SECTION("empty file: defaults") {
        write_file(file, "");
        CHECK(platform::read_user_dirs(home, config).value() == only_desktop_default(home));
    }
    SECTION("64 KiB cap") {
        std::string text = "XDG_MUSIC_DIR=\"$HOME/M\"\n";
        text.resize(std::size_t{64} * 1024, '#'); // exactly the cap: still parsed
        write_file(file, text);
        CHECK(platform::read_user_dirs(home, config).value().music == home / "M");
        text.push_back('\n'); // one byte over: treated as empty
        write_file(file, text);
        CHECK(platform::read_user_dirs(home, config).value() == only_desktop_default(home));
        std::string huge(std::size_t{4} * 1024 * 1024, '#');
        huge.insert(0, "XDG_MUSIC_DIR=\"$HOME/M\"\n");
        write_file(file, huge);
        CHECK(platform::read_user_dirs(home, config).value() == only_desktop_default(home));
    }
    SECTION("argument errors") {
        CHECK(platform::read_user_dirs(Path(), config).error().code == ErrorCode::InvalidArgument);
        CHECK(platform::read_user_dirs("rel", config).error().code == ErrorCode::InvalidArgument);
        CHECK(platform::read_user_dirs(home, Path()).error().code == ErrorCode::InvalidArgument);
        CHECK(platform::read_user_dirs(home, "rel").error().code == ErrorCode::InvalidArgument);
    }
    SECTION("unreadable file or a directory in its place: errors") {
        if (::geteuid() != 0) {
            write_file(file, "XDG_MUSIC_DIR=\"$HOME/M\"\n");
            stdfs::permissions(file, stdfs::perms::none);
            CHECK(platform::read_user_dirs(home, config).error().code ==
                  ErrorCode::PermissionDenied);
            stdfs::permissions(file, stdfs::perms::owner_read | stdfs::perms::owner_write);
        }
        stdfs::remove(file);
        stdfs::create_directories(file);
        CHECK(platform::read_user_dirs(home, config).error().code == ErrorCode::IoError);
    }
    SECTION("a FIFO does not block") {
        stdfs::create_directories(config);
        REQUIRE(::mkfifo(file.c_str(), 0600) == 0);
        // O_NONBLOCK: no writer, read returns 0 at once -> defaults.
        CHECK(platform::read_user_dirs(home, config).value() == only_desktop_default(home));
    }
}

TEST_CASE("user_home: passwd lookups", "[user_home]") {
    SECTION("current user") {
        const passwd* self = ::getpwuid(::getuid());
        if (self == nullptr || self->pw_name == nullptr || self->pw_dir == nullptr ||
            self->pw_dir[0] != '/') {
            SKIP("current uid has no usable passwd entry");
        }
        const std::string name = self->pw_name;
        const std::string home = self->pw_dir;
        auto result = platform::user_home(name);
        REQUIRE(result.has_value());
        CHECK(result->native() == home);
    }
    SECTION("root exists on every Linux system") {
        auto result = platform::user_home("root");
        REQUIRE(result.has_value());
        CHECK(result->is_absolute());
    }
    SECTION("unknown user: NotFound") {
        CHECK(platform::user_home("navigator-no-such-user-7f3a").error().code ==
              ErrorCode::NotFound);
    }
    SECTION("invalid names: InvalidArgument") {
        CHECK(platform::user_home("").error().code == ErrorCode::InvalidArgument);
        CHECK(platform::user_home("a/b").error().code == ErrorCode::InvalidArgument);
        CHECK(platform::user_home("/").error().code == ErrorCode::InvalidArgument);
        CHECK(platform::user_home(std::string_view("ro\0ot", 5)).error().code ==
              ErrorCode::InvalidArgument);
    }
}
