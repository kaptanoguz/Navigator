// src/ui/ops/app_trash.cpp — see app_trash.hpp.
#include "ops/app_trash.hpp"

namespace navigator::ui {

AppTrash create_app_trash(std::string_view trash_dir, bool headless) {
    AppTrash out;
    out.source = trash_source_for(trash_dir, headless);
    switch (out.source) {
    case TrashSource::None:
        out.note =
            "--frames without --trash-dir: no trash (trash, restore and empty are unsupported)";
        return out;
    case TrashSource::Injected: {
        trash::TrashOptions o;
        o.home_trash = Path(std::string(trash_dir));
        o.use_topdir_trashes = false; // fixtures: only DIR, never another volume's trash
        Result<std::shared_ptr<trash::ITrash>> t = trash::create_trash(std::move(o));
        if (!t) {
            out.note = "trash dir unusable: " + t.error().message;
            return out;
        }
        out.trash = std::move(*t);
        return out;
    }
    case TrashSource::System: {
        Result<std::shared_ptr<trash::ITrash>> t = trash::create_trash();
        if (!t) {
            out.note = "no trash: " + t.error().message;
            return out;
        }
        out.trash = std::move(*t);
        return out;
    }
    }
    return out;
}

} // namespace navigator::ui
