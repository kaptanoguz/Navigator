// src/search/match_core.hpp (internal to navigator_search)
//
// Purpose: the matching engine behind search::Matcher (navigator/search.hpp "Matching model"),
//          exposed to the name index so it can reject names on their pre-folded bytes before
//          paying for a fold with offsets.
// Thread-safety: pure functions over caller-owned scratch buffers (one Scratch per thread).
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <navigator/search.hpp>

namespace navigator::search::detail {

// Strict UTF-8 code point length at s[i] (1..4); 1 for every byte that is not the start of a
// valid shortest-form sequence (the same decoding fs::fold_name uses, so invalid bytes are
// single code points).
[[nodiscard]] std::size_t code_point_length(std::string_view s, std::size_t i) noexcept;

// Caller-owned buffers (the Matcher's members, or the index's per-query scratch).
struct Scratch {
    std::string& folded;
    std::vector<std::uint32_t>& offsets;
    std::vector<std::uint32_t>& cps;
    std::vector<std::int32_t>& table;
};

// Necessary condition on the folded name bytes: every Substring term occurs as a byte substring,
// every Fuzzy term is a byte subsequence. False means "cannot match"; true means "maybe".
[[nodiscard]] bool may_match(const Query& query, std::string_view folded) noexcept;

// Full evaluation of `raw` against `query`. When `prefolded` is given it must be
// fs::fold_name(raw) (the index stores it) and is used for the cheap rejection. Appends
// highlight ranges to *highlights (sorted and merged) when not null.
[[nodiscard]] std::optional<std::int32_t>
evaluate(const Query& query, std::string_view raw, Scratch& scratch,
         std::vector<HighlightRange>* highlights,
         std::optional<std::string_view> prefolded = std::nullopt);

// Type of a name with a known "directory-like" bit (search.hpp "Type visibility filter"), with
// no allocation when the filter hides no extension. `fold_buffer` is scratch.
[[nodiscard]] bool allows_name(const TypeFilter& filter, bool directory_like, std::string_view name,
                               std::string& fold_buffer);

} // namespace navigator::search::detail
