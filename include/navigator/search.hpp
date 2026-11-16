// navigator/search.hpp
//
// Purpose: search query and match value types shared by the filter, flatten and name-index paths.
// Owner:   contracts (header); implementation in src/search (role: search). STUB until the
//          search phase.
// Thread-safety: value types only.
#pragma once

#include <cstdint>
#include <string>

#include <navigator/types.hpp>

namespace navigator {

enum class MatchMode : std::uint8_t {
    Substring,
    Fuzzy,
    Glob,
};

struct SearchQuery {
    std::string text; // UTF-8
    MatchMode mode = MatchMode::Substring;
    bool case_sensitive = false;
    bool include_hidden = false;
    Path root; // empty: current directory only (filter); otherwise search below root
};

struct SearchHit {
    Path path;
    std::int32_t score = 0; // higher is better; only comparable within one query
};

} // namespace navigator
