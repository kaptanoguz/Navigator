// navigator/search.hpp
//
// Purpose: Phase 5 name matching shared by the folder filter, the flattened view (Ctrl+E) and
//          the name index: compiled queries, substring and fuzzy matching with a documented
//          scoring model and byte-accurate highlight ranges, deterministic ranking, the
//          extension/type visibility filter, and the filter pipeline over listed entries.
//          No wildcard or regex language (docs/features.md: no `*` syntax): every character of
//          the query is literal. No content search (Phase 10).
// Owner:   contracts (header); implementation in src/search (role: search), tests in
//          tests/search. src/search links navigator_core (fs::fold_name_into, compare_names).
// Thread-safety: Query, Match, TypeFilter, TypeCount and RankInfo are plain values (types.hpp
//                rule). Every free function is pure CPU work and reentrant, callable from any
//                thread including the UI thread (bounded by input size; see the limits). A
//                Matcher holds scratch buffers: one Matcher per thread (it is cheap to create
//                and copyable).
// Errors:  navigator::Result (types.hpp); only the stop_token functions fail (Cancelled) and
//          allocation (OutOfMemory). No exceptions escape.
//
// =============================================================================================
// Matching model
// =============================================================================================
//
// Folding. Names and query are compared after fs::fold_name (locale-independent simple case
// folding plus I/i/İ/ı -> i, invalid bytes as escaped code points), so "istanbul" finds
// "İstanbul.txt" and "ISTANBUL". Positions below count CODE POINTS of the folded strings, which
// correspond 1:1 to code points of the raw name (fs.hpp, fold_name_into). No normalization:
// NFD and NFC spellings of one name are different strings.
//
// Query. compile_query(text, mode) trims ASCII spaces at both ends and splits the rest at runs
// of ASCII spaces into TERMS (at most kMaxQueryTerms; extra terms are appended to the last
// term with one space). Every term must match (AND); terms are matched independently and may
// overlap. A query with no terms is EMPTY: it matches every name with score 0 and no
// highlights. Text longer than kMaxQueryBytes is cut at the last code point boundary before
// the limit (never an error: a filter field must not fail while typing).
//
// Term alignment. For a folded name f[0..L) and a folded term t[0..m), an alignment is a strictly
// increasing position list p[0..m) with f[p[k]] == t[k]. Substring mode allows only contiguous
// alignments (p[k] == p[0] + k); Fuzzy mode allows any alignment (subsequence). The term score
// is the MAXIMUM over its alignments of
//
//   sum over k of   kScoreChar
//                 + (p[k] == 0                  ? kBonusPrefix
//                    : is_word_start(p[k])      ? kBonusWordStart : 0)
//                 + (k > 0 && p[k] == p[k-1] + 1 ? kBonusConsecutive : 0)
//   - kPenaltyGap * sum over k > 0 of (p[k] - p[k-1] - 1)          (gaps inside the match)
//   - min(kPenaltyLeadingMax, kPenaltyLeading * p[0])                (late start)
//   + kBonusExactName  if m == L                       (the term is the whole name)
//   + kBonusExactStem  else if the term equals the folded stem: the name without "." and
//                      fs::extension(name), when that extension is not empty
//
// is_word_start(i), for i > 0, looks at the RAW (unfolded) code points n[i-1], n[i]:
//   - n[i-1] is ASCII and not an ASCII letter or digit (space, '.', '_', '-', '(', '[', ...), or
//   - n[i-1] is an ASCII lowercase letter and n[i] an ASCII uppercase letter (camelCase), or
//   - n[i-1] is an ASCII letter and n[i] an ASCII digit ("report2024").
// Non-ASCII letters never form camelCase boundaries (no Unicode property tables in Phase 5).
//
// Ties between alignments with the same score: the lexicographically smallest position list
// wins (earliest positions). The QUERY score is the sum of its term scores; a name matches only
// if every term has at least one alignment. Since contiguous alignments are fuzzy alignments
// too, every Substring match is a Fuzzy match with a score >= its Substring score.
//
// Highlights: the union of the chosen alignments of all terms, mapped to BYTE ranges of the raw
// name (fold_name_into offsets), merged into sorted, non-overlapping, non-adjacent half-open
// ranges. A range never splits a UTF-8 sequence or an escaped byte.
//
// Ranking (flattened view and index results with a non-empty query; the folder filter keeps
// the tab's sort order and does not rank): ranks_before(a, b) orders by
//   1. score, higher first;
//   2. depth, shallower first (0 for the folder filter, 1 for the root's children);
//   3. name length in bytes, shorter first;
//   4. fs::compare_names(name) with default fs::SortOptions;
//   5. raw name bytes;
//   6. parent path: fs::compare_names, then raw bytes.
// This is a strict weak ordering that is total for distinct (parent, name) pairs, so results
// are identical across runs, thread counts and readdir orders (gold tests rely on it).
//
// Cost. Substring: O(L * m) worst case per term. Fuzzy: a linear subsequence pre-check, then an
// O(L * m) dynamic program over names that pass it. Names are at most a few hundred bytes and
// queries at most kMaxQueryBytes, so one match is bounded; the filter functions below check
// their stop token at least every kFilterStopCheckInterval entries.
#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include <navigator/fs.hpp>
#include <navigator/types.hpp>
#include <navigator/walk.hpp>

namespace navigator::search {

enum class MatchMode : std::uint8_t {
    Substring = 0, // contiguous, case-insensitive "partial name" (folder filter default)
    Fuzzy = 1,     // ordered subsequence (flattened view default)
};

// Stable lowercase name ("substring", "fuzzy"), for logs, tests and JSON.
[[nodiscard]] constexpr std::string_view to_string(MatchMode mode) noexcept {
    return mode == MatchMode::Fuzzy ? std::string_view{"fuzzy"} : std::string_view{"substring"};
}

// Scoring constants (see "Term alignment"). Part of the contract: gold tests compute expected
// scores from them. Changing one is a contract change (docs/architecture.md changelog).
inline constexpr std::int32_t kScoreChar = 16;
inline constexpr std::int32_t kBonusPrefix = 32;
inline constexpr std::int32_t kBonusWordStart = 20;
inline constexpr std::int32_t kBonusConsecutive = 24;
inline constexpr std::int32_t kPenaltyGap = 3;
inline constexpr std::int32_t kPenaltyLeading = 1;
inline constexpr std::int32_t kPenaltyLeadingMax = 15;
inline constexpr std::int32_t kBonusExactName = 100;
inline constexpr std::int32_t kBonusExactStem = 60;

inline constexpr std::size_t kMaxQueryBytes = 256;
inline constexpr std::size_t kMaxQueryTerms = 8;
inline constexpr std::size_t kFilterStopCheckInterval = 4096;

// Folder filter: at or below this many entries the ui filters synchronously on the UI thread
// on every keystroke (target < 2 ms for 20k ASCII names in a Release build; the Phase 5
// benchmark checks it); above it the filter runs on a worker (docs/architecture.md "Phase 5").
inline constexpr std::size_t kUiThreadFilterLimit = 20'000;

// A compiled query. Build it with compile_query(); the fields are readable for tests and for
// the ui (e.g. "is the query empty"), but should not be edited by hand.
struct Query {
    std::string text; // the (possibly cut) text as typed, untrimmed
    MatchMode mode = MatchMode::Substring;
    std::vector<std::string> terms; // folded terms, each non-empty; empty: matches everything

    [[nodiscard]] bool empty() const noexcept { return terms.empty(); }
    friend bool operator==(const Query&, const Query&) = default;
};

// Pure; never fails (see "Query").
[[nodiscard]] Query compile_query(std::string_view text, MatchMode mode);

// True when every name matching `next` is guaranteed to match `prev`: same mode, next.text
// starts with prev.text, and prev.text was not cut at kMaxQueryBytes (appending characters
// lengthens the last term or adds terms, which can only remove matches). The ui may then
// refilter only prev's hits while the user keeps typing. Pure.
[[nodiscard]] bool narrows(const Query& prev, const Query& next) noexcept;

// Half-open byte range [begin, end) of the raw name.
struct HighlightRange {
    std::uint32_t begin = 0;
    std::uint32_t end = 0;
    friend constexpr bool operator==(const HighlightRange&, const HighlightRange&) = default;
};

struct Match {
    std::int32_t score = 0;
    std::vector<HighlightRange> highlights; // sorted, merged (see "Highlights")
    friend bool operator==(const Match&, const Match&) = default;
};

// Reusable matcher: holds the query and scratch buffers so that matching a million names does
// not allocate per name. Not thread-safe (one per thread); copying copies the query.
class Matcher {
public:
    Matcher() = default; // empty query: everything matches with score 0
    explicit Matcher(Query query);

    [[nodiscard]] const Query& query() const noexcept { return query_; }

    // Full match with highlights; nullopt when some term does not match.
    [[nodiscard]] std::optional<Match> match(std::string_view name);
    // Score only (no highlight computation); nullopt when some term does not match.
    [[nodiscard]] std::optional<std::int32_t> score(std::string_view name);
    // Cheapest test: does every term match at all.
    [[nodiscard]] bool matches(std::string_view name);

private:
    Query query_;
    std::string folded_;                 // scratch: folded name
    std::vector<std::uint32_t> offsets_; // scratch: fold_name_into offsets
    std::vector<std::uint32_t> starts_;  // scratch: code point starts / DP rows
    std::vector<std::int32_t> table_;    // scratch: fuzzy DP
};

// Convenience: Matcher(query).match(name). Allocates; prefer a Matcher in loops.
[[nodiscard]] std::optional<Match> match(std::string_view name, const Query& query);

// Everything ranks_before() looks at. Views must outlive the comparison.
struct RankInfo {
    std::int32_t score = 0;
    std::uint32_t depth = 0;
    std::string_view name;   // raw bytes
    std::string_view parent; // raw bytes, relative to the search root ("" for depth <= 1)
};

// "Ranking" order above. Pure; does not allocate.
[[nodiscard]] bool ranks_before(const RankInfo& a, const RankInfo& b) noexcept;

// =============================================================================================
// Type visibility filter (types popup)
// =============================================================================================
//
// Every entry has exactly one type:
//   Folder       fs::is_directory_like(entry) (a link to a folder is a folder, a folder named
//                "photos.2024" is a folder, not an extension)
//   NoExtension  not a folder and fs::extension(name) is empty (".bashrc", "Makefile", "x.")
//   Extension    otherwise; the key is fs::fold_name(fs::extension(name)): "JPG" and "jpg" are
//                one type; only the last suffix counts ("a.tar.gz" -> "gz").
// Other inode types (fifo, socket, device) follow the name rule like files.
//
// The filter stores the HIDDEN types, so types that appear later (new files, watch events,
// another folder) are visible by default. It is a per-tab view setting like show_hidden:
// persisted in TabState (session and named layouts, docs/features.md "Type visibility filters
// saved in session and layout"), kept across navigation, applied to the folder listing and the
// flattened view alike. It applies AFTER the hidden-file policy ("respects hidden/system"):
// with show_hidden off, hidden entries are not in the listing at all, are not counted in the
// popup, and cannot be revealed by a type toggle.

enum class TypeKind : std::uint8_t {
    Folder = 0,
    NoExtension = 1,
    Extension = 2,
};

struct TypeKey {
    TypeKind kind = TypeKind::Extension;
    std::string extension; // folded, no dot; empty unless kind == Extension

    friend bool operator==(const TypeKey&, const TypeKey&) = default;
    // Popup order is summarize_types()'s; this ordering (kind, then bytes) is for containers.
    friend auto operator<=>(const TypeKey&, const TypeKey&) = default;
};

struct TypeFilter {
    bool hide_folders = false;
    bool hide_no_extension = false;
    // Folded extensions without the dot, sorted by raw bytes, unique, each non-empty and free of
    // '/' and NUL (normalize() establishes this; the session loader calls it).
    std::vector<std::string> hidden_extensions;

    // True when anything is hidden (the title-bar chip shows "filtered").
    [[nodiscard]] bool active() const noexcept {
        return hide_folders || hide_no_extension || !hidden_extensions.empty();
    }
    friend bool operator==(const TypeFilter&, const TypeFilter&) = default;
};

// One row of the types popup: a type present in the listing and how many entries have it.
struct TypeCount {
    TypeKey key;
    std::size_t count = 0;
    bool visible = true; // !hidden by the filter the summary was built with
    friend bool operator==(const TypeCount&, const TypeCount&) = default;
};

// Pure helpers (src/search).
[[nodiscard]] TypeKey type_key_of(const fs::DirEntry& entry);
// False when the entry's type is hidden by `filter`.
[[nodiscard]] bool allows(const TypeFilter& filter, const fs::DirEntry& entry);
[[nodiscard]] bool is_visible(const TypeFilter& filter, const TypeKey& key) noexcept;
void set_visible(TypeFilter& filter, const TypeKey& key, bool visible);
// "Toggle all" in the popup, over the types PRESENT in `present` (a summarize_types() result):
// if every present type is visible, hide all of them; otherwise make all of them visible.
// Hidden types that are not present are left as they are.
void toggle_all(TypeFilter& filter, Span<const TypeCount> present);
// Sorts, de-duplicates, folds and drops invalid hidden_extensions (see TypeFilter). Idempotent.
void normalize(TypeFilter& filter);

// The types popup rows for `entries`, respecting the hidden-file policy: with show_hidden ==
// false, entries with DirEntry::hidden do not count (a type that only hidden files have is not
// listed). Order: Folder, then NoExtension (each only when present), then extensions by count
// (descending), then by extension bytes. `visible` reflects `filter`.
[[nodiscard]] std::vector<TypeCount> summarize_types(Span<const fs::DirEntry> entries,
                                                     bool show_hidden, const TypeFilter& filter);

// =============================================================================================
// Filter pipeline (folder filter and flattened view)
// =============================================================================================
//
// One rule for both views: an entry is shown when allows(types, entry) and the query matches
// its name. The folder filter matches names only (never the parent path); the flattened view
// matches names only too (Phase 5: "partial name + fuzzy"; matching the relative path is a
// possible later extension, not a Phase 5 behaviour).

struct FilterHit {
    std::uint32_t index = 0; // index into the input span
    std::int32_t score = 0;  // 0 for an empty query
};

// Appends the hits of entries[i] to `out` in input order (the caller keeps the listing's sort
// order for the folder filter, or ranks for the flattened view). `out` is cleared first.
// Errors: Cancelled (stop requested; `out` is cleared), OutOfMemory.
[[nodiscard]] Result<void> filter_entries(Span<const fs::DirEntry> entries, Matcher& matcher,
                                          const TypeFilter& types, std::vector<FilterHit>& out,
                                          std::stop_token stop = {});
// Same over walked entries (flattened view; hidden policy was applied by the walk).
[[nodiscard]] Result<void> filter_entries(Span<const fs::WalkEntry> entries, Matcher& matcher,
                                          const TypeFilter& types, std::vector<FilterHit>& out,
                                          std::stop_token stop = {});

} // namespace navigator::search
