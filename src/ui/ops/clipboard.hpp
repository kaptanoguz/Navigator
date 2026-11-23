// src/ui/ops/clipboard.hpp
//
// The file clipboard seam (Phase 7): FileOps offers transfer::make_clipboard_offer() formats and
// reads them back through this interface. The SDL implementation (SDL_SetClipboardData /
// SDL_GetClipboardData) lives in sdl_clipboard.cpp; MemoryClipboard is the headless double used
// by tests and by headless runs (never touches the desktop clipboard).
// Thread-safety: UI thread only.
#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace navigator::ui {

class IClipboard {
public:
    IClipboard() = default;
    virtual ~IClipboard() = default;
    IClipboard(const IClipboard&) = delete;
    IClipboard& operator=(const IClipboard&) = delete;
    IClipboard(IClipboard&&) = delete;
    IClipboard& operator=(IClipboard&&) = delete;

    // Replaces the clipboard with these (mime type, bytes) pairs, in offer order.
    virtual void offer(std::vector<std::pair<std::string, std::string>> formats) = 0;
    // Bytes of `mime` on the clipboard (ours or another program's); nullopt when not offered.
    [[nodiscard]] virtual std::optional<std::string> read(std::string_view mime) = 0;
    // Empties the clipboard when it still holds Navigator's last offer; otherwise leaves it.
    virtual void clear_own() = 0;
    // The clipboard still holds Navigator's last offer (no other program took it since).
    [[nodiscard]] virtual bool owns_offer() const = 0;
};

// In-memory clipboard. `set_foreign` simulates another program taking the clipboard (Nautilus,
// Nemo, Dolphin formats in tests).
class MemoryClipboard final : public IClipboard {
public:
    void offer(std::vector<std::pair<std::string, std::string>> formats) override {
        data_.clear();
        for (auto& [mime, bytes] : formats) {
            data_[std::move(mime)] = std::move(bytes);
        }
        own_ = true;
    }
    [[nodiscard]] std::optional<std::string> read(std::string_view mime) override {
        const auto it = data_.find(std::string(mime));
        if (it == data_.end()) {
            return std::nullopt;
        }
        return it->second;
    }
    void clear_own() override {
        if (own_) {
            data_.clear();
            own_ = false;
        }
    }
    [[nodiscard]] bool owns_offer() const override { return own_; }

    void set_foreign(std::map<std::string, std::string> data) {
        data_ = std::move(data);
        own_ = false;
    }
    [[nodiscard]] const std::map<std::string, std::string>& data() const { return data_; }

private:
    std::map<std::string, std::string> data_;
    bool own_ = false;
};

} // namespace navigator::ui
