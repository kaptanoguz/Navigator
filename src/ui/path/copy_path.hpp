// src/ui/path/copy_path.hpp
//
// edit.copy_path ("Copy as Path", docs/features.md): which paths a tab copies and the clipboard
// text. The selection's absolute paths in list order, or the current folder when nothing is
// selected or CopyPathArg::CurrentFolder is set; formatted by
// path_input::format_paths_for_copy(PathQuoting::Auto) and made valid UTF-8 with
// fs::to_display_name (lossy only for names that are not UTF-8). Pure.
// Thread-safety: UI thread (reads a BrowserModel).
#pragma once

#include <string>
#include <vector>

#include <navigator/types.hpp>

#include "browser/browser_model.hpp"

namespace navigator::ui {

[[nodiscard]] std::vector<Path> copy_path_targets(const BrowserModel& model, bool current_folder);
[[nodiscard]] std::string copy_path_text(const std::vector<Path>& paths);

} // namespace navigator::ui
