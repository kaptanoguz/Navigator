// Details-view text: sizes, dates, Type labels, plurals and the i18n catalog coverage.
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include <navigator/commands.hpp>
#include <navigator/i18n.hpp>
#include <navigator/mime.hpp>

#include "browser/format.hpp"
#include "i18n/catalog.hpp"

using namespace navigator;
using namespace navigator::ui;

namespace {

struct LanguageGuard {
    explicit LanguageGuard(Language l) { set_language(l); }
    LanguageGuard(const LanguageGuard&) = delete;
    LanguageGuard& operator=(const LanguageGuard&) = delete;
    LanguageGuard(LanguageGuard&&) = delete;
    LanguageGuard& operator=(LanguageGuard&&) = delete;
    ~LanguageGuard() { set_language(Language::English); }
};

// Dates in UTC regardless of the machine's zone.
struct UtcGuard {
    UtcGuard() {
        // NOLINTBEGIN(concurrency-mt-unsafe): single-threaded test setup.
        if (const char* tz = std::getenv("TZ")) {
            saved = tz;
            had = true;
        }
        setenv("TZ", "UTC", 1);
        tzset();
        // NOLINTEND(concurrency-mt-unsafe)
    }
    UtcGuard(const UtcGuard&) = delete;
    UtcGuard& operator=(const UtcGuard&) = delete;
    UtcGuard(UtcGuard&&) = delete;
    UtcGuard& operator=(UtcGuard&&) = delete;
    ~UtcGuard() {
        // NOLINTBEGIN(concurrency-mt-unsafe)
        if (had) {
            setenv("TZ", saved.c_str(), 1);
        } else {
            unsetenv("TZ");
        }
        tzset();
        // NOLINTEND(concurrency-mt-unsafe)
    }
    std::string saved;
    bool had = false;
};

Timestamp at(int y, unsigned mo, unsigned d, int h, int mi) {
    namespace c = std::chrono;
    const c::sys_days date = c::year{y} / c::month{mo} / c::day{d};
    return Timestamp(date.time_since_epoch() + c::hours{h} + c::minutes{mi});
}

fs::DirEntry file(std::string name) {
    fs::DirEntry e;
    e.name = std::move(name);
    e.kind = fs::EntryKind::File;
    e.mode = 0100644;
    return e;
}

} // namespace

TEST_CASE("Sizes use decimal units with three significant figures", "[format]") {
    const LanguageGuard en(Language::English);
    CHECK(format_size(0) == "0 bytes");
    CHECK(format_size(1) == "1 byte");
    CHECK(format_size(999) == "999 bytes");
    CHECK(format_size(1000) == "1.0 kB");
    CHECK(format_size(1234) == "1.2 kB");
    CHECK(format_size(12'345) == "12.3 kB");
    CHECK(format_size(123'456) == "123 kB");
    CHECK(format_size(999'960) == "1.0 MB"); // rounds up into the next unit
    CHECK(format_size(4'500'000) == "4.5 MB");
    CHECK(format_size(3'200'000'000ULL) == "3.2 GB");
    CHECK(format_size(5'000'000'000'000ULL) == "5.0 TB");
}

TEST_CASE("Size column parts split number and unit", "[format]") {
    {
        const LanguageGuard en(Language::English);
        const SizeParts b = format_size_parts(980);
        CHECK(b.number == "980");
        CHECK(b.unit == "B");
        CHECK(format_size_parts(0).number == "0");
        const SizeParts k = format_size_parts(88'123);
        CHECK(k.number == "88.1");
        CHECK(k.unit == "kB");
        CHECK(format_size_parts(734'003'200).number == "734");
        CHECK(format_size_parts(734'003'200).unit == "MB");
        CHECK(format_size_parts(999'960).unit == "MB"); // same rounding as format_size
    }
    const LanguageGuard tr_(Language::Turkish);
    CHECK(format_size_parts(4'500'000).number == "4,5");
    CHECK(format_size_parts(12).unit == "B");
}

TEST_CASE("Turkish sizes use a decimal comma and bayt", "[format]") {
    const LanguageGuard tr_(Language::Turkish);
    CHECK(format_size(1) == "1 bayt");
    CHECK(format_size(500) == "500 bayt");
    CHECK(format_size(1234) == "1,2 kB");
    CHECK(format_size(4'500'000) == "4,5 MB");
}

TEST_CASE("Dates are short, local and relative for today/yesterday", "[format]") {
    const UtcGuard utc;
    const Timestamp now = at(2026, 11, 16, 15, 0);
    {
        const LanguageGuard en(Language::English);
        CHECK(format_date(at(2026, 11, 16, 9, 5), now) == "Today 09:05");
        CHECK(format_date(at(2026, 11, 15, 23, 59), now) == "Yesterday 23:59");
        CHECK(format_date(at(2024, 2, 3, 14, 30), now) == "Feb 3, 2024 14:30");
        CHECK(format_date(at(2026, 3, 9, 8, 15), now) == "Mar 9 08:15"); // this year: no year
        CHECK(format_date(at(2025, 12, 31, 23, 0), now) == "Dec 31, 2025 23:00");
        CHECK(format_date(Timestamp{}, now).empty());
    }
    {
        const LanguageGuard tr_(Language::Turkish);
        CHECK(format_date(at(2026, 11, 16, 9, 5), now) == "Bugün 09:05");
        CHECK(format_date(at(2026, 11, 15, 8, 0), now) == "Dün 08:00");
        CHECK(format_date(at(2024, 8, 30, 14, 30), now) == "30 Ağu 2024 14:30");
        CHECK(format_date(at(2026, 8, 30, 14, 30), now) == "30 Ağu 14:30");
    }
}

TEST_CASE("Type labels come from the MIME table, links and extensions", "[format]") {
    const LanguageGuard en(Language::English);
    CHECK(type_label(file("photo.JPG")) == "JPEG image");
    CHECK(type_label(file("notes.md")) == "Markdown document");
    CHECK(type_label(file("backup.tar.gz")) == "Archive");
    CHECK(type_label(file("model.xyz")) == "XYZ file");
    CHECK(type_label(file("Makefile")) == "File");
    fs::DirEntry dir;
    dir.name = "src";
    dir.kind = fs::EntryKind::Directory;
    CHECK(type_label(dir) == "Folder");
    fs::DirEntry broken;
    broken.name = "gone";
    broken.kind = fs::EntryKind::Symlink;
    broken.dangling = true;
    CHECK(type_label(broken) == "Broken link");
    fs::DirEntry exe = file("run");
    exe.mode = 0100755;
    CHECK(type_label(exe) == "Program");

    const LanguageGuard tr_(Language::Turkish);
    CHECK(type_label(dir) == "Klasör");
    CHECK(type_label(file("model.xyz")) == "XYZ dosyası");
    CHECK(type_label(file("a.pdf")) == "PDF belgesi");
}

TEST_CASE("File tints group types", "[format]") {
    CHECK(file_tint(file("a.png")) == FileTint::Image);
    CHECK(file_tint(file("a.mp3")) == FileTint::Audio);
    CHECK(file_tint(file("a.zip")) == FileTint::Archive);
    CHECK(file_tint(file("a.cpp")) == FileTint::Code);
    CHECK(file_tint(file("a.pdf")) == FileTint::Pdf);
    CHECK(file_tint(file("a.unknownext")) == FileTint::None);
}

TEST_CASE("Plurals: English one/other, Turkish has no plural after numbers", "[format][i18n]") {
    {
        const LanguageGuard en(Language::English);
        CHECK(tr_count("status.items", 0) == "0 items");
        CHECK(tr_count("status.items", 1) == "1 item");
        CHECK(tr_count("status.items", 12'345) == "12,345 items");
        CHECK(tr_count("status.selected", 1) == "1 selected");
        CHECK(format_count(1'000'000) == "1,000,000");
        CHECK(format_count(999) == "999");
    }
    {
        const LanguageGuard tr_(Language::Turkish);
        CHECK(tr_count("status.items", 1) == "1 öğe");
        CHECK(tr_count("status.items", 5) == "5 öğe");
        CHECK(tr_count("status.items", 12'345) == "12.345 öğe");
        CHECK(tr_count("status.hidden", 3) == "3 gizli");
    }
    CHECK(substitute("{a}-{b}-{a}", "a", "x") == "x-{b}-x");
}

TEST_CASE("Every command, type, error and key name is translated in both languages", "[i18n]") {
    CommandRegistry registry;
    REQUIRE(register_builtin_commands(registry));
    for (const CommandInfo& info : registry.all()) {
        INFO(info.title_key);
        CHECK(has_translation(info.title_key, Language::English));
        CHECK(has_translation(info.title_key, Language::Turkish));
    }
    for (std::string_view key : {"type.folder",
                                 "type.link",
                                 "type.link_broken",
                                 "type.fifo",
                                 "type.socket",
                                 "type.block_device",
                                 "type.char_device",
                                 "type.executable",
                                 "type.file",
                                 "type.file_ext",
                                 "type.image",
                                 "type.image_png",
                                 "type.image_jpeg",
                                 "type.image_gif",
                                 "type.image_webp",
                                 "type.image_svg",
                                 "type.text",
                                 "type.markdown",
                                 "type.csv",
                                 "type.html",
                                 "type.source",
                                 "type.script",
                                 "type.json",
                                 "type.xml",
                                 "type.pdf",
                                 "type.archive",
                                 "type.archive_zip",
                                 "type.package",
                                 "type.package_deb",
                                 "type.appimage",
                                 "type.disk_image",
                                 "type.desktop_entry",
                                 "type.document",
                                 "type.spreadsheet",
                                 "type.presentation",
                                 "type.audio",
                                 "type.video",
                                 "type.font",
                                 "key.ctrl",
                                 "key.shift",
                                 "key.alt",
                                 "key.super",
                                 "column.name",
                                 "column.size",
                                 "column.type",
                                 "column.modified",
                                 "empty.folder",
                                 "empty.unreadable",
                                 "status.items.other",
                                 "status.hidden.other",
                                 "status.selected.other",
                                 "empty.hidden_only.other",
                                 "notice.open_folder_failed",
                                 "notice.open_file_failed",
                                 "notice.refresh_failed",
                                 "notice.folder_gone",
                                 "path.home",
                                 "path.root"}) {
        INFO(key);
        CHECK(has_translation(key, Language::English));
        CHECK(has_translation(key, Language::Turkish));
    }
    // Every mime label key the platform can return is in the catalog.
    for (std::string_view name :
         {"a.png", "a.jpg",  "a.gif", "a.webp",     "a.svg",    "a.bmp",     "a.txt", "a.md",
          "a.csv", "a.html", "a.c",   "a.sh",       "a.json",   "a.xml",     "a.pdf", "a.zip",
          "a.tar", "a.deb",  "a.rpm", "a.appimage", "a.iso",    "a.desktop", "a.odt", "a.ods",
          "a.odp", "a.mp3",  "a.mp4", "a.ttf",      "a.unknown"}) {
        const std::string_view key = mime::type_label_key(mime::guess_from_name(name));
        INFO(name << " -> " << key);
        CHECK(has_translation(key, Language::Turkish));
    }
    for (const ErrorCode code :
         {ErrorCode::Unknown, ErrorCode::InvalidArgument, ErrorCode::NotFound,
          ErrorCode::PermissionDenied, ErrorCode::AlreadyExists, ErrorCode::IoError,
          ErrorCode::Cancelled, ErrorCode::Unsupported, ErrorCode::OutOfMemory,
          ErrorCode::PlatformError}) {
        const std::string key = "error." + std::string(to_string(code));
        INFO(key);
        CHECK(has_translation(key, Language::English));
        CHECK(has_translation(key, Language::Turkish));
    }
    // Every English plural key with a ".one" form also has ".other".
    for (const std::string_view key : catalog_keys()) {
        if (key.ends_with(".one")) {
            std::string other(key.substr(0, key.size() - 4));
            other += ".other";
            INFO(key);
            CHECK(has_translation(other, Language::English));
            CHECK(has_translation(other, Language::Turkish));
        }
    }
}

TEST_CASE("Narrow-pane dates drop the time or the day (Phase 3)", "[format]") {
    const UtcGuard utc;
    const Timestamp now = at(2026, 11, 16, 15, 0);
    {
        const LanguageGuard en(Language::English);
        CHECK(format_date_short(at(2026, 11, 16, 9, 5), now) == "09:05");
        CHECK(format_date_short(at(2026, 11, 15, 9, 5), now) == "Nov 15");
        CHECK(format_date_short(at(2024, 2, 3, 14, 30), now) == "Feb 3, 2024");
        CHECK(format_date_short(Timestamp{}, now).empty());
    }
    {
        const LanguageGuard tr_(Language::Turkish);
        CHECK(format_date_short(at(2026, 8, 30, 14, 30), now) == "30 Ağu");
        CHECK(format_date_short(at(2024, 8, 30, 14, 30), now) == "30 Ağu 2024");
    }
}

TEST_CASE("Columns degrade in review order as panes narrow (Phase 3)", "[format]") {
    // Wide: everything. Type goes first, then Modified shortens, then Size goes, then Modified.
    CHECK(plan_columns(1200.0F) == ColumnPlan{true, true, true, false});
    CHECK(plan_columns(654.0F) == ColumnPlan{true, true, true, false});
    CHECK(plan_columns(600.0F) == ColumnPlan{true, false, true, false});
    CHECK(plan_columns(450.0F) == ColumnPlan{true, false, true, false});
    CHECK(plan_columns(380.0F) == ColumnPlan{true, false, true, true});
    // A ~380px pane leaves about 300dp of name room: Name + short Modified.
    CHECK(plan_columns(300.0F) == ColumnPlan{false, false, true, true});
    CHECK(plan_columns(150.0F) == ColumnPlan{});
    // Monotonic: narrowing never brings a column back.
    ColumnPlan prev = plan_columns(2000.0F);
    for (float w = 2000.0F; w >= 0.0F; w -= 5.0F) {
        const ColumnPlan c = plan_columns(w);
        CHECK((!c.type || prev.type));
        CHECK((!c.size || prev.size));
        CHECK((!c.date || prev.date));
        prev = c;
    }
}
