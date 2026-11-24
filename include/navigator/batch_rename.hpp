// navigator/batch_rename.hpp
//
// Purpose: Phase 9B interactive batch rename engine contract: rule-based renaming with template
//          tokens ({name}, {stem}, {ext}, {counter[:N]}, {date[:FMT]}, {uid[:N]}, {parent}),
//          find/replace, case transforms, whitespace trimming, multi-line and single-line paste
//          semantics (File Pilot v0.5.0 / v0.8.5), live collision detection (both intra-batch and
//          against existing siblings), and conversion to an atomic, undoable ops::OpRequest
//          (OpKind::BatchRename).
//          No SDL, no renderer, no filesystem I/O in the planner.
// Owner:   contracts (header); implementation in src/ops (role: ops), tests in tests/ops.
// Thread-safety: All types are plain values and all functions are pure (callable from any thread,
//                including the UI thread for instant preview of 200+ items).
// Errors:  Invalid names and collisions are values in BatchRenamePlan (never exceptions).
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <navigator/ops.hpp>
#include <navigator/types.hpp>

namespace navigator::ops {

// One item selected for batch renaming.
struct BatchRenameItem {
    Path path;                 // absolute normalized path
    std::string original_name; // raw on-disk filename (basename)
    bool is_directory = false;
    Timestamp mtime{};
    std::uint64_t size = 0;

    friend bool operator==(const BatchRenameItem&, const BatchRenameItem&) = default;
};

enum class CaseTransform : std::uint8_t {
    None = 0,
    Lower = 1, // ASCII + basic UTF-8 lowercase on stem
    Upper = 2, // ASCII + basic UTF-8 uppercase on stem
    Title = 3, // Title Case words on stem
};

// Rule describing how to compute proposed names for a batch of items.
//
// Template tokens (in `template_str`; empty template_str is treated as "{name}"):
//   {name}          original full name (stem + ext)
//   {stem}          original stem (ops::split_extension)
//   {ext}           original extension including leading '.' (empty for directories)
//   {counter}       counter_start + index * counter_step (decimal, no padding)
//   {counter:N}     zero-padded to width N (clamped to 1..12)
//   {date}          item mtime as YYYY-MM-DD (UTC if local unavailable; local time in POSIX)
//   {date:FMT}      custom date format supporting YYYY, YY, MM, DD, HH, mm, ss
//   {uid}           8-hex-char unique identifier derived from uid_seed + index
//   {uid:N}         N-hex-char unique identifier (clamped to 4..32)
//   {parent}        display name of the item's parent directory
//
// Pipeline per item:
//   1. If manual_overrides[i] is non-empty, start from manual_overrides[i]; else expand
//      `template_str`.
//   2. If find_text is non-empty (and no manual override), apply find/replace.
//   3. Apply case_transform to the stem (preserving extension, unless directory).
//   4. If trim_whitespace is true (default, File Pilot v0.8.5), strip leading/trailing ASCII
//      whitespace (' ', '\t', '\r', '\n').
struct BatchRenameRule {
    std::string template_str = "{name}";
    std::string find_text;
    std::string replace_text;
    bool find_case_sensitive = true;
    bool find_replace_all = true;
    CaseTransform case_transform = CaseTransform::None;
    bool trim_whitespace = true;
    int counter_start = 1;
    int counter_step = 1;
    std::uint64_t uid_seed = 0x4E41563942ULL; // deterministic default; UI randomizes on open

    friend bool operator==(const BatchRenameRule&, const BatchRenameRule&) = default;
};

enum class PreviewRowStatus : std::uint8_t {
    Unchanged = 0,         // proposed_name == original_name
    Ready = 1,             // valid new name, no collision
    InvalidName = 2,       // validate_new_name(proposed_name) != NameError::None
    CollisionInternal = 3, // collides with another item in the same batch (same parent dir)
    CollisionExisting = 4, // collides with an existing sibling not renamed away by this batch
};

struct BatchRenamePreviewRow {
    std::size_t index = 0; // index into items
    std::string proposed_name;
    PreviewRowStatus status = PreviewRowStatus::Unchanged;
    NameError name_error = NameError::None;
    std::optional<std::size_t> collides_with; // for CollisionInternal: other item's index

    friend bool operator==(const BatchRenamePreviewRow&, const BatchRenamePreviewRow&) = default;
};

struct BatchRenamePlan {
    std::vector<BatchRenamePreviewRow> rows;
    std::size_t changed_count = 0;
    std::size_t collision_count = 0;
    std::size_t invalid_count = 0;

    [[nodiscard]] bool has_errors() const noexcept {
        return collision_count > 0 || invalid_count > 0;
    }
    [[nodiscard]] bool can_apply() const noexcept { return changed_count > 0 && !has_errors(); }
};

// Result of interpreting clipboard text pasted into the batch rename overlay.
//   - Multi-line text: assigns line i to item i (manual_overrides).
//   - Single-line text (File Pilot v0.5.0 special case): when pasting one line for multiple items,
//     produces a template (trimmed line + "{ext}" when the line contains no '{' token and has no
//     extension, or trimmed line as-is when it already contains tokens), OR fills manual_overrides
//     for a single item.
struct BatchPasteResult {
    bool is_single_line = false;
    std::string single_line_template;        // set when is_single_line == true
    std::vector<std::string> line_overrides; // set when is_single_line == false
};

// Splits clipboard text on CRLF / LF, dropping a single trailing empty line if present. Pure.
[[nodiscard]] std::vector<std::string> split_paste_lines(std::string_view text);

// Interprets pasted clipboard text for `items` (handles single-line special case). Pure.
[[nodiscard]] BatchPasteResult interpret_batch_paste(std::string_view clipboard_text,
                                                     const std::vector<BatchRenameItem>& items,
                                                     bool keep_extension = true);

// Generates a deterministic N-char lowercase hex unique ID for (seed, index). N clamped to 4..32.
[[nodiscard]] std::string format_batch_uid(std::uint64_t seed, std::size_t index,
                                           std::size_t length = 8);

// Formats a Timestamp using FMT (YYYY, YY, MM, DD, HH, mm, ss). Empty FMT defaults to YYYY-MM-DD.
[[nodiscard]] std::string format_batch_date(Timestamp ts, std::string_view fmt = "YYYY-MM-DD");

// Computes the full preview plan for `items` under `rule` and optional per-row `manual_overrides`.
// `existing_sibling_names` lists names currently present in the target folder(s) (optional; when
// items span multiple folders, pass empty or full paths are checked per parent). Items that are
// renamed away in the same batch do NOT count as existing collisions (swaps like a->b, b->a are
// Ready, not CollisionExisting). Pure.
[[nodiscard]] BatchRenamePlan
build_batch_rename_plan(const std::vector<BatchRenameItem>& items, const BatchRenameRule& rule,
                        const std::vector<std::string>& manual_overrides = {},
                        const std::vector<std::string>& existing_sibling_names = {});

// Builds an OpRequest (OpKind::BatchRename) containing only the Ready (changed) items from `plan`.
// Returns an OpRequest with empty sources if !plan.can_apply(). Pure.
[[nodiscard]] OpRequest make_batch_rename_request(const std::vector<BatchRenameItem>& items,
                                                  const BatchRenamePlan& plan);

} // namespace navigator::ops
