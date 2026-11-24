// src/ui/menu/openwith_popup.hpp
//
// Phase 8 Open With popup (open_with.show): lists applications for the selected files' MIME
// type(s) loaded asynchronously, filtered by typed text, with an "Always open with this" checkbox
// (set_default_handler) and Enter / click launch via openwith::launch_with.
// Thread-safety: UI thread only.
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include <navigator/commands.hpp>
#include <navigator/openwith.hpp>
#include <navigator/types.hpp>

#include "shell/painter.hpp"
#include "theme/theme.hpp"
#include "widgets/rect.hpp"
#include "widgets/text_edit.hpp"
#include "widgets/ui_context.hpp"

namespace navigator::ui {

struct OpenWithAction {
    openwith::Handler handler;
    std::vector<Path> paths;
    std::string primary_mime;
    bool set_as_default = false;
};

class OpenWithOverlay {
public:
    [[nodiscard]] bool is_open() const noexcept { return open_; }
    [[nodiscard]] bool wants_text_input() const noexcept { return open_; }
    [[nodiscard]] TextEdit* active_field() noexcept { return open_ ? &query_ : nullptr; }

    void open(std::vector<Path> paths, std::string primary_mime);
    void set_handlers(std::vector<openwith::Handler> handlers);
    void close() noexcept;

    void query_changed();
    std::optional<OpenWithAction> handle_key(KeyChord chord);
    std::optional<OpenWithAction> draw(const Painter& p, const Palette& pal, const RectF& viewport,
                                       const PointerInput& pointer);

private:
    void refilter();
    std::optional<OpenWithAction> activate_selected();

    bool open_ = false;
    bool loading_ = true;
    bool always_default_ = false;
    std::vector<Path> paths_;
    std::string primary_mime_;
    std::vector<openwith::Handler> handlers_;
    std::vector<std::size_t> filtered_;
    std::size_t selected_ = 0;
    std::size_t scroll_ = 0;

    TextEdit query_;
    float field_scroll_ = 0.0F;
};

} // namespace navigator::ui
