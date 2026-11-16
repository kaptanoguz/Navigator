// src/ui/shell/ui_settings.cpp — see ui_settings.hpp.
#include "shell/ui_settings.hpp"

namespace navigator::ui {
namespace {

UiSettings& storage() noexcept {
    static UiSettings settings;
    return settings;
}

} // namespace

void set_ui_settings(const UiSettings& settings) noexcept { storage() = settings; }

UiSettings ui_settings() noexcept { return storage(); }

} // namespace navigator::ui
