// src/ui/ops/app_trash.hpp
//
// Which ITrash the app uses (Phase 7 safety, AppConfig::trash_dir): an injected home trash
// (fixtures), the real XDG trash (interactive runs), or none (headless runs without
// --trash-dir: trash / restore / empty fail with Unsupported and nothing can reach the owner's
// real trash). Pure decision plus the factory call; no SDL.
// Thread-safety: any thread.
#pragma once

#include <memory>
#include <string>
#include <string_view>

#include <navigator/trash.hpp>

namespace navigator::ui {

enum class TrashSource : std::uint8_t {
    Injected, // --trash-dir DIR: DIR is the home trash, per-volume trashes off
    System,   // the real XDG trash
    None,     // headless without --trash-dir
};

// `headless`: AppConfig::max_frames > 0.
[[nodiscard]] constexpr TrashSource trash_source_for(std::string_view trash_dir,
                                                     bool headless) noexcept {
    if (!trash_dir.empty()) {
        return TrashSource::Injected;
    }
    return headless ? TrashSource::None : TrashSource::System;
}

struct AppTrash {
    std::shared_ptr<trash::ITrash> trash; // null for TrashSource::None or on failure
    TrashSource source = TrashSource::None;
    std::string note; // why there is no trash (log line); empty otherwise
};

[[nodiscard]] AppTrash create_app_trash(std::string_view trash_dir, bool headless);

} // namespace navigator::ui
