// src/ui/inspector/metadata.hpp
//
// Phase 6 Inspector key/value metadata (design review "EXIF and metadata"): grouped rows (File,
// Image, Camera, Location, Video, Audio, Text, Folder), translated labels, locale-aware numbers
// and units, empty groups skipped (never "—" rows). GPS is shown as coordinates only (no map, no
// network). Pure: tests check the formatting in English and Turkish.
// Thread-safety: as tr() (UI thread; never concurrently with set_language()).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <navigator/fs.hpp>
#include <navigator/preview.hpp>
#include <navigator/types.hpp>

namespace navigator::ui::inspector {

struct MetaRow {
    std::string label; // translated
    std::string value; // translated / formatted; copyable
};

struct MetaGroup {
    std::string title; // translated, e.g. "Camera"
    std::vector<MetaRow> rows;
};

// Everything the Inspector lists for `entry` (and its preview, when finished). `now` drives
// "Today" style dates.
[[nodiscard]] std::vector<MetaGroup>
build_metadata(const fs::DirEntry& entry, const preview::PreviewResult* result, Timestamp now);

// Formatting helpers (exposed for tests).
[[nodiscard]] std::string format_decimal(double value, int digits);   // locale separator
[[nodiscard]] std::string format_dimensions(preview::PixelSize size); // "4032 × 3024"
[[nodiscard]] std::string format_megapixels(preview::PixelSize size); // "12.2 MP"
[[nodiscard]] std::string format_exposure(preview::Rational seconds); // "1/250 s", "2.5 s"
[[nodiscard]] std::string format_aperture(double f_number);           // "f/2.8"
[[nodiscard]] std::string format_focal(double mm, std::optional<std::uint32_t> mm35);
[[nodiscard]] std::string format_duration(std::int64_t ms);                 // "0:42", "1:02:03"
[[nodiscard]] std::string format_frame_rate(preview::Rational rate);        // "29.97 fps"
[[nodiscard]] std::string format_bit_rate(std::uint64_t bits_per_second);   // "4.2 Mb/s"
[[nodiscard]] std::string format_coordinate(double degrees, bool latitude); // "41.00820° N"
[[nodiscard]] std::string format_exif_date(const preview::ExifDateTime& when);
[[nodiscard]] std::string format_sample_rate(std::uint32_t hz); // "48 kHz"

// B1 (design review Phase 6): the image came from a system thumbnailer or the shared thumbnail
// cache for a file that is not an image (PDF, office, e-book, ...): a "page preview", a Navigator
// extra. The Inspector labels it so and lists no Image group (the thumbnail's pixel size is not
// the document's).
[[nodiscard]] bool is_page_preview(const fs::DirEntry& entry,
                                   const preview::PreviewResult& result) noexcept;

// Header subtitle, the most useful one-liner per kind: image "4000 × 3000 · 719 kB", video
// "0:08 · 1280 × 720 · 3.2 MB", text "312 lines · 7.8 kB"; otherwise "JPEG image · 4.2 MB"
// (folders: item count when known).
[[nodiscard]] std::string header_subtitle(const fs::DirEntry& entry,
                                          const preview::PreviewResult* result);

} // namespace navigator::ui::inspector
