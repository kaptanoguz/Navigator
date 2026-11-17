// src/ui/browser/format.hpp
//
// Text for the Details view and status bar: human sizes, locale-aware short dates, Type-column
// labels and the file-type category used to tint glyphs. Pure (no SDL, no I/O except the C
// library's time-zone database for local time), so tests cover them headless.
// Thread-safety: as tr() (any thread, never concurrently with set_language()); format_date uses
// localtime_r.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include <navigator/fs.hpp>
#include <navigator/types.hpp>

namespace navigator::ui {

// "0 bytes", "1 byte", "999 bytes", "1.2 kB", "12.3 MB", "123 GB" (decimal SI units, as GNOME
// Files). Turkish uses a decimal comma ("1,2 MB") and "bayt".
[[nodiscard]] std::string format_size(std::uint64_t bytes);

// The Size column's form: the number and a short unit as separate runs, so the column can
// right-align numbers in one slot and units in the next ("980" "B", "88.1" "kB"). Same
// rounding as format_size(); units "B", "kB", "MB", ... (catalog keys size.unit.*).
struct SizeParts {
    std::string number;
    std::string unit;
};
[[nodiscard]] SizeParts format_size_parts(std::uint64_t bytes);

// Local-time short date with 24-hour time: "Nov 16, 2025 14:05" / "16 Kas 2025 14:05"; the year
// is dropped within the year of `now` ("Nov 10 10:00" / "10 Kas 10:00"); "Today 14:05" /
// "Bugün 14:05" and "Yesterday 09:12" / "Dün 09:12" relative to `now`.
// Empty for Timestamp{} (unknown).
[[nodiscard]] std::string format_date(Timestamp when, Timestamp now);

// The Modified column when a pane is narrow: "14:05" for today, otherwise the date without the
// time ("Nov 10" / "10 Kas" this year, "Nov 10, 2024" / "10 Kas 2024" before).
[[nodiscard]] std::string format_date_short(Timestamp when, Timestamp now);

// Details columns for a list whose name text may use `room_dp` (from the name's left edge to
// the right content edge). Narrow panes degrade in this order: Type hides, Modified shortens to
// format_date_short(), Size hides, Modified hides; Name always stays.
inline constexpr float kColumnGapDp = 20.0F;
inline constexpr float kSizeWidthDp = 84.0F;
inline constexpr float kTypeWidthDp = 160.0F;
inline constexpr float kDateWidthDp = 150.0F;
inline constexpr float kShortDateWidthDp = 92.0F;
struct ColumnPlan {
    bool size = false;
    bool type = false;
    bool date = false;
    bool short_date = false;
    friend constexpr bool operator==(const ColumnPlan&, const ColumnPlan&) = default;
};
[[nodiscard]] ColumnPlan plan_columns(float room_dp) noexcept;

// Type-column text: tr(mime::type_label_key(mime::guess_for_entry(entry))), except a dangling
// link ("Broken link") and an unknown type with an extension ("XYZ file").
[[nodiscard]] std::string type_label(const fs::DirEntry& entry);

// Glyph tint category of a file, from its MIME label key.
enum class FileTint : std::uint8_t {
    None,
    Image,
    Audio,
    Video,
    Archive,
    Code,
    Document,
    Pdf,
    Sheet,
    Slides,
    Program,
};
[[nodiscard]] FileTint file_tint(const fs::DirEntry& entry) noexcept;

// File name for display (fs::to_display_name: invalid UTF-8 bytes become U+FFFD).
[[nodiscard]] std::string display_name(std::string_view raw);

} // namespace navigator::ui
