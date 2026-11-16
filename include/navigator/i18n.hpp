// navigator/i18n.hpp
//
// Purpose: UI string translation (English, Turkish). Every user-visible string goes through tr().
// Owner:   contracts (header); implementation in src/ui (role: ui) for Phase 0.
// Thread-safety: set_language() is called on the UI thread, at startup or between frames, never
//                concurrently with tr(). tr() and language() may be called from any thread as
//                long as no set_language() runs at the same time.
//
// Keys are stable dotted ASCII ids, e.g. "app.title", "cmd.app.quit" (CommandInfo::title_key).
#pragma once

#include <string_view>

namespace navigator {

enum class Language {
    English,
    Turkish,
};

// Default before any call: English.
void set_language(Language language);
[[nodiscard]] Language language() noexcept;

// Translation of `key` in the current language. Falls back to English, then to `key` itself.
// A returned translation has static storage duration; when the key is returned, it refers to the
// caller's storage.
[[nodiscard]] std::string_view tr(std::string_view key) noexcept;

} // namespace navigator
