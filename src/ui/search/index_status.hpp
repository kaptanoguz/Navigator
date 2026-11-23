// src/ui/search/index_status.hpp
//
// Phase 5: how the name index's state reads in the status bar of a whole-volume search
// (search.flatten_volume): a short translated label, an optional determinate fraction for the
// progress bar (scanning), and a tone. Pure (i18n tables only).
// Thread-safety: as tr().
#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include <navigator/index.hpp>

namespace navigator::ui {

enum class IndexTone : std::uint8_t { Quiet, Busy, Ready, Problem };

struct IndexStatusView {
    std::string text;
    std::optional<float> fraction; // 0..1 while scanning, when it can be estimated
    IndexTone tone = IndexTone::Quiet;
};

[[nodiscard]] IndexStatusView present_index_status(const search::IndexStatus& status);

// Scanning progress estimate: directories read / directories known (never shows 100% while
// Scanning); nullopt until the index reports pending directories.
[[nodiscard]] std::optional<float> index_scan_fraction(const search::IndexStatus& status) noexcept;

} // namespace navigator::ui
