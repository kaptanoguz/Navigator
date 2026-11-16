// Simple case folding table lookup (see case_fold.hpp).
#include "case_fold.hpp"

#include <algorithm>
#include <cstdint>
#include <iterator>

namespace navigator::core {
namespace {

// Code points first, first + stride, ..., first + (count - 1) * stride fold to cp + delta.
struct FoldRun {
    char32_t first;
    std::uint16_t count;
    std::uint8_t stride;
    std::int32_t delta;
};

#include "case_fold_data.inc"

constexpr char32_t kCapitalIWithDot = 0x0130;
constexpr char32_t kDotlessI = 0x0131;

} // namespace

std::string_view case_folding_unicode_version() noexcept { return kCaseFoldingUnicodeVersion; }

char32_t fold_non_ascii(char32_t cp) noexcept {
    if (cp == kCapitalIWithDot || cp == kDotlessI) {
        return U'i';
    }
    // Last run whose first <= cp.
    const auto* it = std::upper_bound(std::begin(kFoldRuns), std::end(kFoldRuns), cp,
                                      [](char32_t v, const FoldRun& r) { return v < r.first; });
    if (it == std::begin(kFoldRuns)) {
        return cp;
    }
    const FoldRun& run = *std::prev(it);
    const char32_t offset = cp - run.first;
    if (offset % run.stride != 0 || offset / run.stride >= run.count) {
        return cp;
    }
    return static_cast<char32_t>(static_cast<std::int64_t>(cp) + run.delta);
}

} // namespace navigator::core
