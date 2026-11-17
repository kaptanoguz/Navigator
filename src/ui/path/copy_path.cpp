// src/ui/path/copy_path.cpp — see copy_path.hpp.
#include "path/copy_path.hpp"

#include <navigator/fs.hpp>
#include <navigator/path_input.hpp>

namespace navigator::ui {

std::vector<Path> copy_path_targets(const BrowserModel& model, bool current_folder) {
    std::vector<Path> out;
    if (!current_folder && model.selected_count() > 0) {
        const std::vector<fs::DirEntry>& entries = model.entries();
        out.reserve(model.selected_count());
        for (std::size_t i = 0; i < entries.size(); ++i) {
            if (model.is_selected(i)) {
                out.push_back(model.current_dir() / entries[i].name);
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
