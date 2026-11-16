// src/ui/browser/format.cpp — see format.hpp.
#include "browser/format.hpp"

#include <array>
#include <chrono>
#include <cstdio>
#include <ctime>

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

std::string format_size(std::uint64_t bytes) {
    if (bytes < 1000) {
        return tr_count("size.bytes", bytes);
    }
    constexpr std::array<std::string_view, 5> keys{"size.kb", "size.mb", "size.gb", "size.tb",
                                                   "size.pb"};
    double value = static_cast<double>(bytes) / 1000.0;
    std::size_t unit = 0;
    while (unit + 1 < keys.size() && value >= 999.95) {
        value /= 1000.0;
        ++unit;
    }
    // Three significant figures: 1.23 is shown as 1.2 (one decimal below 100), 123.4 as 123.
    const int decimals = value < 99.95 ? 1 : 0;
    return substitute(tr(keys[unit]), "n", with_decimal(value, decimals));
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
        date = substitute(tr("date.format"), "d", std::to_string(tw.tm_mday));
        date = substitute(date, "mon", tr(month_key));
        date = substitute(date, "y", std::to_string(tw.tm_year + 1900));
    }
    std::string out = substitute(tr("date.with_time"), "date", date);
    return substitute(out, "time", time);
}

std::string type_label(const fs::DirEntry& entry) {
    if (entry.kind == fs::EntryKind::Symlink && entry.dangling) {
        return std::string(tr("type.link_broken"));
    }
    const std::string_view key = mime::type_label_key(mime::guess_for_entry(entry));
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
