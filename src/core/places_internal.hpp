// src/core/places_internal.hpp (internal to navigator_core)
//
// Purpose: pieces of navigator/places.hpp shared between places.cpp (stores) and
//          places_json.cpp (JSON loading).
// Thread-safety: as places.hpp (plain values, pure functions).
#pragma once

#include <string_view>

#include <navigator/places.hpp>

namespace navigator::places::internal {

// BookmarkStore::add's name rule: valid UTF-8 without control characters (< 0x20, 0x7F).
[[nodiscard]] bool is_valid_bookmark_name(std::string_view name) noexcept;

// Appends `entry` (normalized, unique, within capacity: the caller checks) at the end of the
// store's list, keeping its last_visit and visit_count. For recents_from_json() only.
void append_loaded(RecentsStore& store, RecentEntry entry);

} // namespace navigator::places::internal
