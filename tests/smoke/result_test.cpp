// Phase 0 contract smoke: navigator::Result / fail() / ok() from <navigator/types.hpp>.
#include <string>
#include <type_traits>

#include <catch2/catch_test_macros.hpp>

#include <navigator/types.hpp>

namespace {

using navigator::ErrorCode;
using navigator::Result;

Result<int> parse_positive(int v) {
    if (v <= 0) {
        return navigator::fail(ErrorCode::InvalidArgument, "not positive");
    }
    return v;
}

Result<void> find_thing(bool present) {
    if (!present) {
        return navigator::fail(ErrorCode::NotFound, "x");
    }
    return navigator::ok();
}

} // namespace

TEST_CASE("Result<int> holds a success value", "[result]") {
    const Result<int> r = parse_positive(42);
    REQUIRE(r.has_value());
    REQUIRE(static_cast<bool>(r));
    REQUIRE(*r == 42);
    REQUIRE(r.value() == 42);
}

TEST_CASE("fail() converts to Result<int>", "[result]") {
    const Result<int> r = navigator::fail(ErrorCode::NotFound, "x");
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == ErrorCode::NotFound);
    REQUIRE(r.error().message == "x");

    const Result<int> r2 = parse_positive(-1);
    REQUIRE_FALSE(r2);
    REQUIRE(r2.error().code == ErrorCode::InvalidArgument);
}

TEST_CASE("fail() converts to Result<void>; ok() is success", "[result]") {
    const Result<void> bad = navigator::fail(ErrorCode::NotFound, "x");
    REQUIRE_FALSE(bad.has_value());
    REQUIRE(bad.error().code == ErrorCode::NotFound);
    REQUIRE(bad.error().message == "x");

    REQUIRE(find_thing(true).has_value());
    REQUIRE(navigator::ok().has_value());
    REQUIRE(find_thing(false).error().code == ErrorCode::NotFound);
}

TEST_CASE("ErrorCode has stable string ids", "[result]") {
    STATIC_REQUIRE(navigator::to_string(ErrorCode::NotFound) == "not_found");
    STATIC_REQUIRE(navigator::to_string(ErrorCode::Unknown) == "unknown");
}

TEST_CASE("FileId compares by value", "[types]") {
    constexpr navigator::FileId a{1, 2};
    constexpr navigator::FileId b{1, 2};
    constexpr navigator::FileId c{1, 3};
    STATIC_REQUIRE(a == b);
    STATIC_REQUIRE(!(a == c));
    STATIC_REQUIRE(std::is_same_v<decltype(navigator::fail(ErrorCode::Unknown)),
                                  navigator::Unexpected<navigator::Error>>);
}
