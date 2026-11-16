// src/ui/i18n/catalog.hpp
//
// ui-internal helpers on top of navigator/i18n.hpp: catalog introspection (tests), plural
// selection and "{name}" placeholder substitution.
// Plural rules: English has "one" (exactly 1) and "other"; Turkish uses "other" for every count
// (no plural suffix after a number: "1 öğe", "5 öğe"). Keys are "<base>.one" / "<base>.other";
// a language may leave ".one" empty, which falls back to English only when the language is
// English (Turkish never reads ".one").
// Thread-safety: as tr(): any thread, never concurrently with set_language().
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <navigator/i18n.hpp>

namespace navigator::ui {

// True when the catalog has `key` with a non-empty text for `language` (no English fallback).
[[nodiscard]] bool has_translation(std::string_view key, Language language) noexcept;

// Every key in the catalog, in table order.
[[nodiscard]] std::vector<std::string_view> catalog_keys();

// "<base>.one" or "<base>.other" for `count` in the current language.
[[nodiscard]] std::string plural_key(std::string_view base, std::uint64_t count);

// Replaces every "{name}" in `pattern` with `value`.
[[nodiscard]] std::string substitute(std::string_view pattern, std::string_view name,
                                     std::string_view value);

// tr(plural_key(base, count)) with "{n}" replaced by `count` formatted with format_count().
[[nodiscard]] std::string tr_count(std::string_view base, std::uint64_t count);

// Thousands-grouped integer: "12,345" (English), "12.345" (Turkish).
[[nodiscard]] std::string format_count(std::uint64_t value);

} // namespace navigator::ui
