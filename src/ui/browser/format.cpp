// src/ui/browser/format.cpp — see format.hpp.
#include "browser/format.hpp"

#include <array>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <utility>

#include <navigator/i18n.hpp>
#include <navigator/mime.hpp>

#include "i18n/catalog.hpp"

namespace navigator::ui {
namespace {

std::string with_decimal(double value, int decimals) {
    std::array<char, 32> buf{};
    std::snprintf(buf.data(), buf.size(), "%.*f", decimals, value);
    std::string s(buf.data());
    if (language() == Language::Turkish) {
        for (char& c : s) {
            if (c == '.') {
                c = ',';
            }
        }
    }
    return s;
}

std::string two_digits(int v) {
    std::array<char, 8> buf{};
    std::snprintf(buf.data(), buf.size(), "%02d", v);
    return {buf.data()};
}

bool local_tm(std::time_t t, std::tm& out) noexcept { return localtime_r(&t, &out) != nullptr; }

bool same_day(const std::tm& a, const std::tm& b) noexcept {
    return a.tm_year == b.tm_year && a.tm_yday == b.tm_yday;
}

} // namespace

namespace {

struct Scaled {
    double value = 0.0;
    std::size_t unit = 0; // 0 = kB, 1 = MB, ...
    int decimals = 1;
};

// bytes >= 1000 in decimal SI units with three significant figures.
Scaled scale_bytes(std::uint64_t bytes) {
    Scaled s;
    s.value = static_cast<double>(bytes) / 1000.0;
    while (s.unit + 1 < 5 && s.value >= 999.95) {
        s.value /= 1000.0;
        ++s.unit;
    }
    s.decimals = s.value < 99.95 ? 1 : 0;
    return s;
}

} // namespace

std::string format_size(std::uint64_t bytes) {
    if (bytes < 1000) {
        return tr_count("size.bytes", bytes);
    }
    constexpr std::array<std::string_view, 5> keys{"size.kb", "size.mb", "size.gb", "size.tb",
                                                   "size.pb"};
    const Scaled s = scale_bytes(bytes);
    return substitute(tr(keys[s.unit]), "n", with_decimal(s.value, s.decimals));
}

SizeParts format_size_parts(std::uint64_t bytes) {
    if (bytes < 1000) {
        return SizeParts{format_count(bytes), std::string(tr("size.unit.b"))};
    }
    constexpr std::array<std::string_view, 5> keys{"size.unit.kb", "size.unit.mb", "size.unit.gb",
                                                   "size.unit.tb", "size.unit.pb"};
    const Scaled s = scale_bytes(bytes);
    return SizeParts{with_decimal(s.value, s.decimals), std::string(tr(keys[s.unit]))};
}

std::string format_date(Timestamp when, Timestamp now) {
    if (when == Timestamp{}) {
        return {};
    }
    const auto secs = [](Timestamp t) {
        return static_cast<std::time_t>(
            std::chrono::duration_cast<std::chrono::seconds>(t.time_since_epoch()).count());
    };
    std::tm tw{};
    std::tm tn{};
    std::tm ty{};
    if (!local_tm(secs(when), tw) || !local_tm(secs(now), tn) ||
        !local_tm(secs(now) - (std::time_t{24} * 60 * 60), ty)) {
        return {};
    }
    const std::string time = two_digits(tw.tm_hour) + ":" + two_digits(tw.tm_min);
    std::string date;
    if (same_day(tw, tn)) {
        date = std::string(tr("date.today"));
    } else if (same_day(tw, ty)) {
        date = std::string(tr("date.yesterday"));
    } else {
        const std::string month_key = "date.month." + std::to_string(tw.tm_mon + 1);
        const bool this_year = tw.tm_year == tn.tm_year;
        date = substitute(tr(this_year ? "date.format_this_year" : "date.format"), "d",
                          std::to_string(tw.tm_mday));
        date = substitute(date, "mon", tr(month_key));
        date = substitute(date, "y", std::to_string(tw.tm_year + 1900));
    }
    std::string out = substitute(tr("date.with_time"), "date", date);
    return substitute(out, "time", time);
}

std::string format_date_short(Timestamp when, Timestamp now) {
    if (when == Timestamp{}) {
        return {};
    }
    const auto secs = [](Timestamp t) {
        return static_cast<std::time_t>(
            std::chrono::duration_cast<std::chrono::seconds>(t.time_since_epoch()).count());
    };
    std::tm tw{};
    std::tm tn{};
    if (!local_tm(secs(when), tw) || !local_tm(secs(now), tn)) {
        return {};
    }
    if (same_day(tw, tn)) {
        return two_digits(tw.tm_hour) + ":" + two_digits(tw.tm_min);
    }
    const std::string month_key = "date.month." + std::to_string(tw.tm_mon + 1);
    const bool this_year = tw.tm_year == tn.tm_year;
    std::string date = substitute(tr(this_year ? "date.format_this_year" : "date.format"), "d",
                                  std::to_string(tw.tm_mday));
    date = substitute(date, "mon", tr(month_key));
    return substitute(date, "y", std::to_string(tw.tm_year + 1900));
}

ColumnPlan plan_columns(float room_dp) noexcept {
    // Widest first; each step trades a column for name room (Phase 2 review: Type goes first,
    // then Modified shortens, then Size goes; Modified is the last to go).
    constexpr float gap = kColumnGapDp;
    struct Step {
        bool size;
        bool type;
        bool date;
        bool short_date;
        float min_name;
    };
    constexpr std::array<Step, 5> steps{
        Step{true, true, true, false, 200.0F}, Step{true, false, true, false, 150.0F},
        Step{true, false, true, true, 130.0F}, Step{false, false, true, true, 110.0F},
        Step{false, false, false, false, 0.0F}};
    for (const Step& st : steps) {
        float used = 0.0F;
        if (st.size) {
            used += kSizeWidthDp + gap;
        }
        if (st.type) {
            used += kTypeWidthDp + gap;
        }
        if (st.date) {
            used += (st.short_date ? kShortDateWidthDp : kDateWidthDp) + gap;
        }
        if (room_dp - used >= st.min_name || st.min_name <= 0.0F) {
            return ColumnPlan{st.size, st.type, st.date, st.short_date};
        }
    }
    return ColumnPlan{};
}

std::string type_label(const fs::DirEntry& entry) {
    if (entry.kind == fs::EntryKind::Symlink && entry.dangling) {
        return std::string(tr("type.link_broken"));
    }
    const std::string_view key = mime::type_label_key(mime::guess_for_entry(entry));
    if (key == "type.video") {
        // Phase 6 design review: "MP4 video" / "MP4 videosu", like "JPEG image".
        std::string ext(fs::extension(entry.name));
        for (char& c : ext) {
            if (c >= 'A' && c <= 'Z') {
                c = static_cast<char>(c - 'A' + 'a');
            }
        }
        static constexpr std::pair<std::string_view, std::string_view> kNames[] = {
            {"mp4", "MP4"},       {"m4v", "MP4"},   {"mkv", "Matroska"}, {"webm", "WebM"},
            {"mov", "QuickTime"}, {"avi", "AVI"},   {"wmv", "WMV"},      {"flv", "Flash"},
            {"mpg", "MPEG"},      {"mpeg", "MPEG"}, {"ts", "MPEG-TS"},   {"mts", "AVCHD"},
            {"m2ts", "AVCHD"},    {"3gp", "3GPP"},  {"ogv", "Ogg"},
        };
        for (const auto& [e, name] : kNames) {
            if (ext == e) {
                return substitute(tr("type.video_named"), "name", name);
            }
        }
    }
    if (key == "type.file") {
        const std::string_view ext = fs::extension(entry.name);
        if (!ext.empty() && ext.size() <= 8) {
            std::string upper = display_name(ext);
            for (char& c : upper) {
                if (c >= 'a' && c <= 'z') {
                    c = static_cast<char>(c - ('a' - 'A'));
                }
            }
            return substitute(tr("type.file_ext"), "ext", upper);
        }
    }
    return std::string(tr(key));
}

FileTint file_tint(const fs::DirEntry& entry) noexcept {
    const std::string_view key = mime::type_label_key(mime::guess_for_entry(entry));
    const auto starts = [key](std::string_view prefix) { return key.starts_with(prefix); };
    if (starts("type.image")) {
        return FileTint::Image;
    }
    if (key == "type.audio") {
        return FileTint::Audio;
    }
    if (key == "type.video") {
        return FileTint::Video;
    }
    if (starts("type.archive") || starts("type.package") || key == "type.disk_image") {
        return FileTint::Archive;
    }
    if (key == "type.source" || key == "type.script" || key == "type.json" || key == "type.xml" ||
        key == "type.html") {
        return FileTint::Code;
    }
    if (key == "type.pdf") {
        return FileTint::Pdf;
    }
    if (key == "type.spreadsheet" || key == "type.csv") {
        return FileTint::Sheet;
    }
    if (key == "type.presentation") {
        return FileTint::Slides;
    }
    if (key == "type.document" || key == "type.text" || key == "type.markdown") {
        return FileTint::Document;
    }
    if (key == "type.executable" || key == "type.appimage" || key == "type.desktop_entry") {
        return FileTint::Program;
    }
    return FileTint::None;
}

std::string display_name(std::string_view raw) { return fs::to_display_name(raw); }

} // namespace navigator::ui
