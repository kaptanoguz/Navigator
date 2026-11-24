// src/ui/ops/op_text.hpp
//
// Wording of file operations (Phase 7): progress titles ("Copying 3 items to Documents"),
// finished summaries ("Moved 3 items to the trash", "2 of 3 items moved"), plain-language
// error sentences with the failing item, undo labels and rate / ETA text. Headless and pure
// apart from tr(): English and Turkish come from the catalog, plurals through tr_count().
// Error::message is never shown; ErrorCode picks the text (docs/architecture.md "Phase 7").
// Thread-safety: as tr().
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <navigator/ops.hpp>
#include <navigator/types.hpp>

namespace navigator::ui {

// "Copying “a.txt” to Documents" / "Copying 3 items to Documents" (running form). `name` is the
// display name of the first item, `folder` the display name of the destination folder (may be
// empty for kinds without one). Undo / Redo use `label` (make_label text) instead.
[[nodiscard]] std::string op_title(ops::OpKind kind, std::size_t count, std::string_view name,
                                   std::string_view folder, std::string_view label = {});

// "Copied 3 items to Documents" (finished form, all items done).
[[nodiscard]] std::string op_done_text(ops::OpKind kind, std::size_t count, std::string_view name,
                                       std::string_view folder, std::string_view label = {});

// "2 of 3 items moved" (some items failed, were skipped or the operation was cancelled).
[[nodiscard]] std::string op_partial_text(ops::OpKind kind, std::size_t done, std::size_t total,
                                          bool cancelled);

// One sentence naming the failing item: "Not enough space on the destination for “a.txt”."
[[nodiscard]] std::string error_sentence(ErrorCode code, std::string_view item_name);
// ItemStatus::Changed (undo / redo guard mismatch).
[[nodiscard]] std::string changed_sentence(std::string_view item_name);

// "Undo: move 3 items to Photos" text of make_label() (without the "Undo:" prefix).
[[nodiscard]] std::string undo_label_text(const ops::UndoLabel& label);

// "12.4 MB/s" and "1 min 20 s left" / "45 s left" / "2 h 5 min left".
[[nodiscard]] std::string format_rate(double bytes_per_second);
[[nodiscard]] std::string format_eta(std::chrono::seconds eta);

// Display name of the last component of `path` ("Documents"; "/" for the root).
[[nodiscard]] std::string folder_display(const Path& path);

// Fraction 0..1 of an operation: bytes when totals are final, else items, else nullopt
// (indeterminate: scanning or waiting).
[[nodiscard]] std::optional<double> op_fraction(const ops::OpProgress& p);

// Key suffix of an OpKind ("copy", "move", ...): ops::to_string.
[[nodiscard]] std::string kind_key(ops::OpKind kind);

} // namespace navigator::ui
