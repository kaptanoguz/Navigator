// src/ui/shell/ui_settings.hpp
//
// ui-internal start-up settings that AppConfig (the public contract) does not carry yet. The
// executable sets them from its command line before App::create(); App::create() reads them once.
// Currently: list density (`navigator --density compact|comfortable`).
// Thread-safety: set and read on the main thread only, before App::create().
#pragma once

#include "theme/theme.hpp"

namespace navigator::ui {

struct UiSettings {
    Density density = Density::Compact;
};

void set_ui_settings(const UiSettings& settings) noexcept;
[[nodiscard]] UiSettings ui_settings() noexcept;

} // namespace navigator::ui
