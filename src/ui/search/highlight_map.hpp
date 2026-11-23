// src/ui/search/highlight_map.hpp
//
// Phase 5 match highlights: search::Match gives byte ranges of the RAW name; the Name column
// draws display_name(raw) (invalid UTF-8 bytes become U+FFFD, 3 bytes each), possibly elided
// ("head…tail"). These pure helpers carry the ranges through both steps; a match hidden in the
// elided middle highlights the ellipsis, so the user still sees that something matched there.
// Thread-safety: pure functions.
#pragma once

#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

#include <navigator/search.hpp>

namespace navigator::ui {

using ByteRanges = std::vector<std::pair<std::uint32_t, std::uint32_t>>;

// Raw-name ranges onto `display` == fs::to_display_name(raw). Empty when the two do not line up
// (never expected; highlights are then simply not drawn).
[[nodiscard]] ByteRanges to_display_ranges(std::string_view raw, std::string_view display,
                                           const std::vector<search::HighlightRange>& ranges);

// Display ranges onto the fitted text. `shown` empty: the text was not elided.
[[nodiscard]] ByteRanges to_shown_ranges(std::string_view display, std::string_view shown,
                                         const ByteRanges& ranges);

} // namespace navigator::ui
