// Shape cache: LRU eviction order and hit/miss accounting.
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "text/lru_cache.hpp"

using navigator::ui::text::LruCache;

TEST_CASE("LRU cache returns what was inserted and counts hits", "[text][cache]") {
    LruCache<int> cache(4);
    CHECK(cache.find("a") == nullptr);
    cache.insert("a", 1);
    const auto a = cache.find("a");
    REQUIRE(a != nullptr);
    CHECK(*a == 1);
    CHECK(cache.hits() == 1U);
    CHECK(cache.misses() == 1U);
    cache.insert("a", 2); // replace keeps one entry
    CHECK(cache.size() == 1U);
    CHECK(*cache.find("a") == 2);
    CHECK(*a == 1); // a held value survives replacement
}

TEST_CASE("LRU cache evicts the least recently used entry", "[text][cache]") {
    LruCache<std::string> cache(3);
    cache.insert("a", "A");
    const auto held = cache.insert("b", "B");
    cache.insert("c", "C");
    CHECK(cache.find("a") != nullptr); // a is now most recent; b is the oldest
    cache.insert("d", "D");
    CHECK(cache.size() == 3U);
    CHECK(cache.find("b") == nullptr);
    CHECK(cache.find("a") != nullptr);
    CHECK(cache.find("c") != nullptr);
    CHECK(cache.find("d") != nullptr);
    REQUIRE(held != nullptr);
    CHECK(*held == "B"); // evicted while held: still alive for the holder
    cache.clear();
    CHECK(cache.size() == 0U);
    CHECK(cache.find("a") == nullptr);
}

TEST_CASE("Keys with embedded NUL bytes are distinct", "[text][cache]") {
    LruCache<int> cache(8);
    cache.insert(std::string("x\0a", 3), 1);
    cache.insert(std::string("x\0b", 3), 2);
    CHECK(*cache.find(std::string("x\0a", 3)) == 1);
    CHECK(*cache.find(std::string("x\0b", 3)) == 2);
    CHECK(cache.find("x") == nullptr);
}
