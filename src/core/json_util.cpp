// Shared JSON helpers (src/core/json_util.hpp).
#include "json_util.hpp"

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "utf8.hpp"

namespace navigator::core::json {
namespace {

constexpr char kHexDigits[] = "0123456789abcdef";

int hex_value(char c) noexcept {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

} // namespace

std::optional<std::string> as_bytes(const InJson* value) {
    if (value == nullptr) {
        return std::nullopt;
    }
    if (value->is_string()) {
        return value->get<std::string>();
    }
    const InJson* hex = member(*value, "hex");
    if (hex == nullptr || !hex->is_string()) {
        return std::nullopt;
    }
    const auto& digits = hex->get_ref<const std::string&>();
    if (digits.size() % 2 != 0) {
        return std::nullopt;
    }
    std::string raw;
    raw.reserve(digits.size() / 2);
    for (std::size_t i = 0; i < digits.size(); i += 2) {
        const int hi = hex_value(digits[i]);
        const int lo = hex_value(digits[i + 1]);
        if (hi < 0 || lo < 0) {
            return std::nullopt;
        }
        raw.push_back(static_cast<char>((hi << 4) | lo));
    }
    return raw;
}

OutJson bytes_to_json(std::string_view raw) {
    if (is_valid_utf8(raw)) {
        // Not braced: {x} would select nlohmann's initializer-list (array) constructor.
        return OutJson(std::string(raw)); // NOLINT(modernize-return-braced-init-list)
    }
    std::string hex;
    hex.reserve(raw.size() * 2);
    for (const char c : raw) {
        const auto b = static_cast<unsigned char>(c);
        hex.push_back(kHexDigits[b >> 4U]);
        hex.push_back(kHexDigits[b & 0x0FU]);
    }
    OutJson out = OutJson::object();
    out["hex"] = std::move(hex);
    return out;
}

std::string dump(const OutJson& doc) {
    std::string text = doc.dump(2, ' ', false, OutJson::error_handler_t::strict);
    text.push_back('\n');
    return text;
}

const InJson* member(const InJson& object, const char* key) {
    if (!object.is_object()) {
        return nullptr;
    }
    const auto it = object.find(key);
    return it == object.end() ? nullptr : &*it;
}

// Integer value, saturated to the int64 range. Floats (even 1.0) are not integers.
std::optional<std::int64_t> as_integer(const InJson* value) {
    if (value == nullptr || !value->is_number_integer()) {
        return std::nullopt;
    }
    if (value->is_number_unsigned()) {
        const auto u = value->get<std::uint64_t>();
        constexpr auto kMax = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
        return u > kMax ? std::numeric_limits<std::int64_t>::max() : static_cast<std::int64_t>(u);
    }
    return value->get<std::int64_t>();
}

bool as_bool(const InJson* value, bool fallback) {
    return value != nullptr && value->is_boolean() ? value->get<bool>() : fallback;
}

std::optional<std::string> as_string(const InJson* value) {
    if (value == nullptr || !value->is_string()) {
        return std::nullopt;
    }
    return value->get<std::string>();
}

Result<InJson> parse_object(std::string_view text) {
    InJson doc = InJson::parse(text.begin(), text.end(), nullptr, false);
    if (doc.is_discarded()) {
        return fail(ErrorCode::InvalidArgument, "not valid JSON");
    }
    if (!doc.is_object()) {
        return fail(ErrorCode::InvalidArgument, "top-level JSON value is not an object");
    }
    return doc;
}

} // namespace navigator::core::json
