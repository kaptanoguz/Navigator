// src/ui/path/copy_path.cpp — see copy_path.hpp.
#include "path/copy_path.hpp"

#include <navigator/fs.hpp>
#include <navigator/path_input.hpp>

namespace navigator::ui {

std::vector<Path> copy_path_targets(const BrowserModel& model, bool current_folder) {
    std::vector<Path> out;
    if (!current_folder && model.selected_count() > 0) {
        // Rows, not entries (Phase 5): a filtered or flattened view copies what it shows.
        out.reserve(model.selected_count());
        const std::size_t n = model.row_count();
        for (std::size_t i = 0; i < n; ++i) {
            if (model.is_selected(i)) {
                out.push_back(model.row_path(i));
            }
        }
    }
    if (out.empty() && !model.current_dir().empty()) {
        out.push_back(model.current_dir());
    }
    return out;
}

std::string copy_path_text(const std::vector<Path>& paths) {
    return fs::to_display_name(path_input::format_paths_for_copy(paths));
}

} // namespace navigator::ui
