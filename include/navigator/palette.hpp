// navigator/palette.hpp
//
// Purpose: Phase 8 command palette, the pure part: the entry model the ui builds from the
//          registry, keymap and aliases; the ranking of a typed query over those entries
//          (fuzzy title/name matching with the Phase 5 scoring model, category, description,
//          alias, history boost, context filtering, the "destructive is never first" rule);
//          and the bounded usage history stored in palette_history.json. The overlay itself
//          (text field, list, pickers, key handling) is src/ui.
// Owner:   contracts (header); implementation in src/search (role: search), tests in
//          tests/search. DEPENDENCY DIRECTION: src/commands must not depend on src/search, so the
//          ranking, which reuses search::Matcher, lives in src/search and takes plain structs;
//          it includes commands.hpp only for CommandId / Command / CommandContext / Danger /
//          ArgKind (value types, inline helpers), so src/search needs commands.hpp but does not
//          link navigator_commands. The history file I/O is in src/search too (same atomic-write
//          rules as layout::save_session; it may link navigator_core).
// Thread-safety: Entry, Hit, SearchResult, History (a value) and every free function except the
//                file functions are pure and reentrant: any thread, including the UI thread
//                (bounded: kMaxEntries entries, a query of at most search::kMaxQueryBytes bytes;
//                ranking about 500 entries is well under a millisecond). A search::Matcher is
//                created inside rank(); nothing is shared. load_history / save_history block on
//                file I/O: load at startup (the UI-thread exception, as for the session), save
//                from a worker with a copy of the History, debounced about 1 s after the last
//                recorded command and once on quit.
// Errors:  navigator::Result (types.hpp) for file I/O only. rank() never fails (a query that
//          matches nothing yields an empty result).
//
// =============================================================================================
// What the palette is (normative)
// =============================================================================================
//
//   P1  The palette is global: palette.open (Ctrl+Shift+P) works from every focus except inside
//       the palette (where it closes it) and except while a confirmation or conflict sheet is
//       open. It captures a CommandContext BEFORE taking focus.
//   P2  It lists every command with CommandInfo::palette_visible that is_applicable() in the
//       captured context, plus the builtin_command_variants() that apply, plus one `alias` per
//       user alias that is usable (effective_aliases). Commands that do not apply are absent,
//       not greyed.
//   P3  Running an entry enqueues Entry::command (id + arg) on the CommandQueue, exactly what
//       a key binding or the context menu enqueues. The palette never calls a handler, never
//       calls ops, and has no way to pass "confirmed": a destructive entry opens the same
//       confirmation sheet (initial focus on Cancel) that Shift+Delete opens. Enter on an entry
//       therefore never deletes anything by itself. The palette closes before the sheet opens.
//   P4  Safe default: the first result and the default highlight (the one Enter runs) is never a
//       destructive entry while a non-destructive one is in the result (rank() enforces it,
//       default_index below); when every result is destructive nothing is highlighted and Enter
//       does nothing until the user moves with Down. Destructive commands are never recorded in
//       the history (they never gain a boost) and their alias match is still a normal match.
//   P5  Commands with ArgKind::OpId / BookmarkRow get a second step (a ui picker) as described in
//       commands.hpp ArgKind; ranking is unaffected.
//   P6  Entries show their shortcut (Entry::shortcut: the primary binding, "Ctrl+B" or
//       "Ctrl+K Ctrl+S", empty when unbound) and, when the match came through an alias, the
//       alias. The ui translates modifier names for display.
//   P7  Live labels: edit.undo / edit.redo entries carry the operation label ("Undo: move 3
//       items to Photos") in Entry::title (from ops::make_label and the cmd.edit.undo_labeled
//       key); their match text also includes the generic title (Entry::match_title), so
//       typing "undo" finds them. They apply only with ContextFlags::CanUndo / CanRedo.
//   P8  Shown instead of the 8 s toast: with an empty query the palette lists recent commands
//       first (see browse order), so "Undo" is one Ctrl+Shift+P, Enter away.
//
// =============================================================================================
// Ranking (normative; tests/search pins it)
// =============================================================================================
//
// Input: a query string, the entries, a History, a CommandContext, SearchOptions.
//
//   R0  Filter: entries with !is_applicable (requires_all / hidden_if_any copied into the Entry)
//       are dropped before anything else.
//   R1  query = ASCII-trimmed text. Empty -> browse order (R7). Otherwise it is compiled with
//       search::compile_query(query, MatchMode::Fuzzy): ASCII-space-separated terms, ALL must
//       match (AND), each as a fuzzy subsequence; Substring mode is used for descriptions.
//   R2  Candidates per entry; the score of the entry is the MAXIMUM of the candidates that
//       match, and the entry is excluded when none does:
//         title        fuzzy score of Entry::title (and of Entry::match_title when set; the
//                      higher one)                                                    + 0
//         name         fuzzy score of Entry::name ("view.toggle_sidebar"; '.' and '_' are word
//                      starts)                                      - kNameDiscount
//         category     fuzzy score of category + " " + title ("file new")  - kCategoryDiscount
//         description  SUBSTRING score of Entry::description          - kDescriptionDiscount
//         alias        for each alias: query equals it (alias_names_equal) -> kAliasExactScore;
//                      query is a proper prefix of it -> kAliasPrefixScore - 4 * (alias length -
//                      query length) bytes, not below kAliasPrefixScore / 2
//       (Scores are search::Matcher::score values; the constants live in this header.)
//   R3  History boost, only for entries that are recordable (is_recordable) and only when R2
//       matched: + history_boost(history, key). Capped by kHistoryBoostMax so relevance, not
//       habit, decides between very different matches.
//   R4  Order: total score descending; ties: shorter title (code points), category order
//       (CommandCategory declaration order), title in Unicode simple-folded order, then the
//       entry's index in the input (stable and deterministic).
//   R5  Safe default (P4): after R4, if the first hit is destructive (Entry::danger) and a
//       non-destructive hit exists, the best non-destructive hit moves to the front (the others
//       keep their order). SearchResult::default_index is 0 when hits[0] is non-destructive,
//       otherwise nullopt.
//   R6  At most SearchOptions::max_results hits (default 60); highlights are computed for the
//       title only (HighlightRange, search.hpp) and only for hits that matched through the
//       title; `via` says which candidate won.
//   R7  Browse order (empty query): first the recent entries, newest first, at most
//       kBrowseRecents, taken from the history (only recordable entries, applicable ones);
//       then all remaining applicable entries by category order, then title in folded order.
//       default_index is 0 (the first entry is a recent or a first-category entry, never an
//       entry that is destructive by R5 applied the same way).
//
// Destructive and context: an entry's `danger` is the effective danger (commands.hpp
// effective_danger) at the time the ui built the entry.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <navigator/commands.hpp>
#include <navigator/search.hpp>
#include <navigator/types.hpp>

namespace navigator::palette {

// ---------------------------------------------------------------------------------------------
// Constants (tests pin the ranking with these)
// ---------------------------------------------------------------------------------------------

inline constexpr std::size_t kMaxEntries = 1024; // rank() considers at most this many
inline constexpr std::size_t kDefaultMaxResults = 60;
inline constexpr std::size_t kBrowseRecents = 8;

// Scoring offsets relative to search::kScoreChar (16 per matched character, +32 prefix, ...).
inline constexpr std::int32_t kNameDiscount = 24;
inline constexpr std::int32_t kCategoryDiscount = 12;
inline constexpr std::int32_t kDescriptionDiscount = 60;
inline constexpr std::int32_t kAliasExactScore = 10000; // beats every title score
inline constexpr std::int32_t kAliasPrefixScore = 300;

// History boost: rank 0 (most recent) adds kHistoryRecentBoost, each further rank 3 less (not
// below 0); each recorded use adds kHistoryCountBoost, at most kHistoryCountCap uses counted; the
// sum is capped at kHistoryBoostMax.
inline constexpr std::int32_t kHistoryRecentBoost = 30;
inline constexpr std::int32_t kHistoryRecentStep = 3;
inline constexpr std::int32_t kHistoryCountBoost = 2;
inline constexpr std::int32_t kHistoryCountCap = 10;
inline constexpr std::int32_t kHistoryBoostMax = 50;

// ---------------------------------------------------------------------------------------------
// Entries
// ---------------------------------------------------------------------------------------------

// What the ui lists. Built (cheaply, about 150 of them) every time the palette opens and when the
// undo / redo labels or the keymap change while it is open. Plain strings: the ui fills them from
// tr(), the keymap and the registry. No view points into ui storage.
struct Entry {
    Command command;         // what Enter enqueues (id + arg; a variant has a non-zero arg)
    std::string name;        // CommandInfo::name, e.g. "view.toggle_sidebar" (matched)
    std::string title;       // translated title, possibly dynamic (undo label); matched
    std::string match_title; // optional generic translated title also matched ("Undo")
    std::string category;    // translated category title; shown; "<category> <title>" matched
    std::string description; // translated, may be empty; matched (substring) at low weight
    std::string shortcut;    // display text of the primary binding; empty when unbound
    std::vector<std::string> aliases; // usable user aliases for exactly (command.id, command.arg)
    CommandCategory category_id = CommandCategory::App; // ordering key
    Danger danger = Danger::Safe;                       // effective danger
    ArgKind arg_kind = ArgKind::None;
    ContextFlags requires_all = ContextFlags::None; // copied from CommandInfo / CommandVariant
    ContextFlags hidden_if_any = ContextFlags::None;
};

// History key of an entry: Entry::name, or "<name>#<arg>" for a variant (non-zero arg of a
// ArgKind::Flags command). Entries with a runtime-id arg (picker results) are never recorded.
[[nodiscard]] std::string history_key(const Entry& entry);

// Whether running the entry is recorded in the history and gains a boost: not destructive, and
// not a picker-supplied runtime id. Pure.
[[nodiscard]] bool is_recordable(const Entry& entry) noexcept;

// ---------------------------------------------------------------------------------------------
// History (recents and frequency)
// ---------------------------------------------------------------------------------------------

inline constexpr std::size_t kMaxHistoryItems = 100;
inline constexpr std::uint32_t kMaxHistoryCount = 1000000; // counts saturate here
inline constexpr std::string_view kHistoryFileName = "palette_history.json";

[[nodiscard]] inline Path history_file(const Path& config_root) {
    return config_root / kHistoryFileName;
}

struct HistoryItem {
    std::string key;         // history_key()
    std::uint32_t count = 0; // times run, 1..kMaxHistoryCount

    friend bool operator==(const HistoryItem&, const HistoryItem&) = default;
};

// Most recently used first, at most kMaxHistoryItems (the least recent is dropped). A plain value
// owned by the UI thread; the background writer gets a copy.
class History {
public:
    History() = default;

    // Moves `key` to the front (inserting it with count 1, or incrementing, saturating at
    // kMaxHistoryCount). Empty keys are ignored.
    void record(std::string_view key);
    void clear() noexcept;
    // Replaces the items (loader). Entries beyond the limit, empty keys, zero counts and
    // duplicates (first wins) are dropped; counts clamp to kMaxHistoryCount.
    void assign(std::vector<HistoryItem> items);

    [[nodiscard]] Span<const HistoryItem> items() const noexcept { return items_; }
    // 0-based recency rank of `key`, nullopt when absent.
    [[nodiscard]] std::optional<std::size_t> rank_of(std::string_view key) const noexcept;
    [[nodiscard]] std::uint32_t count_of(std::string_view key) const noexcept;

    friend bool operator==(const History&, const History&) = default;

private:
    std::vector<HistoryItem> items_;
};

// R3 boost for `key` (0 when absent). Pure.
[[nodiscard]] std::int32_t history_boost(const History& history, std::string_view key) noexcept;

// JSON v1 (UTF-8, 2-space indent, final newline; deterministic):
//   { "version": 1, "items": [ { "command": "view.toggle_sidebar", "count": 3 }, ... ] }
// "items" is most recent first; "command" holds a history_key. Lenient load: unknown keys
// ignored; a missing or non-array "items" is an empty history; non-object elements, empty or
// non-string "command" and counts that are not positive integers are dropped (a missing count
// counts as 1); duplicates keep the first; beyond kMaxHistoryItems the rest is dropped; counts
// clamp. A history key of a command that no longer exists is harmless (never matches an entry).
// Errors: InvalidArgument (not a JSON object, or longer than 1 MiB), OutOfMemory.
[[nodiscard]] Result<History> history_from_json(std::string_view text);
[[nodiscard]] std::string to_json(const History& history);

// Errors of load_history: NotFound (first start: the caller uses an empty History),
// InvalidArgument (empty path; larger than 1 MiB; not a JSON object -- renamed to "<file>.bad"
// like the session), PermissionDenied, IoError, OutOfMemory.
[[nodiscard]] Result<History> load_history(const Path& file);
// Atomic write with the rules of layout::save_session (0700 dirs, 0600 file, temp + fsync +
// rename). Errors: InvalidArgument (empty path), PermissionDenied, IoError, OutOfMemory.
Result<void> save_history(const Path& file, const History& history);

// ---------------------------------------------------------------------------------------------
// Ranking
// ---------------------------------------------------------------------------------------------

// Which candidate produced the score of a hit.
enum class MatchedVia : std::uint8_t { Browse, Title, Name, Category, Description, Alias };

struct Hit {
    std::size_t entry = 0; // index into the entries passed to rank()
    std::int32_t score = 0;
    MatchedVia via = MatchedVia::Browse;
    // Title highlights (byte ranges of Entry::title) when via == Title; otherwise empty.
    std::vector<search::HighlightRange> highlights;
    // Via == Alias: the alias that matched (index into Entry::aliases).
    std::size_t alias = 0;
};

struct SearchOptions {
    std::size_t max_results = kDefaultMaxResults;
};

struct SearchResult {
    std::vector<Hit> hits;
    // The hit Enter runs and the list highlights initially (R5/R7): 0, or nullopt when the first
    // hit is destructive and no safe hit exists, or when there are no hits.
    std::optional<std::size_t> default_index;
};

// Ranks `entries` for `query` per R0-R7. Entries beyond kMaxEntries are ignored. Pure.
[[nodiscard]] SearchResult rank(std::string_view query, Span<const Entry> entries,
                                const History& history, const CommandContext& context,
                                const SearchOptions& options = {});

} // namespace navigator::palette
