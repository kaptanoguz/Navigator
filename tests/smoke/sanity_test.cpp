// Phase 0 sanity: toolchain, language level, UTF-8 source/literal handling.
#include <cstddef>
#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

TEST_CASE("build uses C++20 or newer", "[sanity]") { STATIC_REQUIRE(__cplusplus >= 202002L); }

TEST_CASE("build configuration is reported", "[sanity]") {
#ifdef NDEBUG
    constexpr bool asserts_enabled = false;
#else
    constexpr bool asserts_enabled = true;
#endif
    INFO("asserts enabled: " << asserts_enabled);
    STATIC_REQUIRE(sizeof(void*) == 8); // 64-bit Linux targets only
    SUCCEED();
}

TEST_CASE("UTF-8 literals round-trip, including Turkish", "[sanity][utf8]") {
    constexpr std::u8string_view u8text = u8"ğüşıöçİ";
    const std::string bytes(reinterpret_cast<const char*>(u8text.data()), u8text.size());

    // 7 code points, each 2 bytes in UTF-8.
    REQUIRE(bytes.size() == 14);
    REQUIRE(std::string_view{"ğüşıöçİ"} == bytes); // execution charset is UTF-8

    // Spot-check encodings: ğ = U+011F (C4 9F), İ = U+0130 (C4 B0), ı = U+0131 (C4 B1).
    REQUIRE(static_cast<unsigned char>(bytes[0]) == 0xC4);
    REQUIRE(static_cast<unsigned char>(bytes[1]) == 0x9F);
    REQUIRE(static_cast<unsigned char>(bytes[12]) == 0xC4);
    REQUIRE(static_cast<unsigned char>(bytes[13]) == 0xB0);

    const std::u8string back(reinterpret_cast<const char8_t*>(bytes.data()), bytes.size());
    REQUIRE(back == u8text);

    std::size_t code_points = 0;
    for (const char c : bytes) {
        if ((static_cast<unsigned char>(c) & 0xC0U) != 0x80U) {
            ++code_points;
        }
    }
    REQUIRE(code_points == 7);
}
