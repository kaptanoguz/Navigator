// src/ui/ops/sdl_clipboard.hpp
//
// IClipboard over SDL3's clipboard data API (SDL_SetClipboardData / SDL_GetClipboardData): the
// formats of transfer::make_clipboard_offer are offered together (GNOME / Nemo / Caja / Thunar
// copied-files, text/uri-list, the KDE cut marker, text) and read back in the order of
// transfer::read_clipboard. SDL calls the data callback lazily when another program pastes and
// the cleanup callback when the offer is replaced, which is how owns_offer() learns that
// another program took the clipboard.
// Thread-safety: UI thread only (SDL calls the callbacks from the event pump).
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>

#include "ops/clipboard.hpp"

namespace navigator::ui {

class SdlClipboard final : public IClipboard {
public:
    SdlClipboard() = default;
    ~SdlClipboard() override;
    SdlClipboard(const SdlClipboard&) = delete;
    SdlClipboard& operator=(const SdlClipboard&) = delete;
    SdlClipboard(SdlClipboard&&) = delete;
    SdlClipboard& operator=(SdlClipboard&&) = delete;

    void offer(std::vector<std::pair<std::string, std::string>> formats) override;
    [[nodiscard]] std::optional<std::string> read(std::string_view mime) override;
    void clear_own() override;
    [[nodiscard]] bool owns_offer() const override { return current_ != nullptr; }

    struct Offer;                         // SDL callback userdata (defined in the .cpp)
    void released(Offer* offer) noexcept; // the cleanup callback

private:
    Offer* current_ = nullptr;
};

} // namespace navigator::ui
