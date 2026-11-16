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

// Local-time short date with 24-hour time: "Nov 16, 2026 14:05" / "16 Kas 2026 14:05";
// "Today 14:05" / "Bugün 14:05" and "Yesterday 09:12" / "Dün 09:12" relative to `now`.
// Empty for Timestamp{} (unknown).
[[nodiscard]] std::string format_date(Timestamp when, Timestamp now);

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
