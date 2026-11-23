// src/search/match.cpp: compile_query, narrows, Matcher and ranks_before
// (navigator/search.hpp "Matching model" and "Ranking").
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <navigator/fs.hpp>
#include <navigator/search.hpp>

#include "match_core.hpp"

namespace navigator::search {

namespace detail {

namespace {

[[nodiscard]] constexpr bool in_range(unsigned char b, unsigned char lo, unsigned char hi) {
    return b >= lo && b <= hi;
}

} // namespace

std::size_t code_point_length(std::string_view s, std::size_t i) noexcept {
    const auto at = [&](std::size_t k) { return static_cast<unsigned char>(s[k]); };
    const unsigned char b0 = at(i);
    if (b0 < 0x80) {
        return 1;
    }
    const std::size_t left = s.size() - i;
    if (in_range(b0, 0xC2, 0xDF)) {
        return left >= 2 && in_range(at(i + 1), 0x80, 0xBF) ? 2 : 1;
    }
    if (in_range(b0, 0xE0, 0xEF)) {
        if (left < 3) {
            return 1;
        }
        const unsigned char lo = b0 == 0xE0 ? 0xA0 : 0x80;
        const unsigned char hi = b0 == 0xED ? 0x9F : 0xBF;
        return in_range(at(i + 1), lo, hi) && in_range(at(i + 2), 0x80, 0xBF) ? 3 : 1;
    }
    if (in_range(b0, 0xF0, 0xF4)) {
        if (left < 4) {
            return 1;
        }
        const unsigned char lo = b0 == 0xF0 ? 0x90 : 0x80;
        const unsigned char hi = b0 == 0xF4 ? 0x8F : 0xBF;
        return in_range(at(i + 1), lo, hi) && in_range(at(i + 2), 0x80, 0xBF) &&
                       in_range(at(i + 3), 0x80, 0xBF)
                   ? 4
                   : 1;
    }
    return 1;
}

namespace {

constexpr std::int32_t kNeg = std::numeric_limits<std::int32_t>::min() / 4;
constexpr std::int32_t kValid = kNeg / 2; // anything above is a real score

[[nodiscard]] bool is_ascii(std::string_view s) noexcept {
    return std::all_of(s.begin(), s.end(),
                       [](char c) { return (static_cast<unsigned char>(c) & 0x80U) == 0; });
}

[[nodiscard]] constexpr bool ascii_upper(unsigned char c) noexcept { return c >= 'A' && c <= 'Z'; }
[[nodiscard]] constexpr bool ascii_lower(unsigned char c) noexcept { return c >= 'a' && c <= 'z'; }
[[nodiscard]] constexpr bool ascii_digit(unsigned char c) noexcept { return c >= '0' && c <= '9'; }
[[nodiscard]] constexpr bool ascii_alpha(unsigned char c) noexcept {
    return ascii_upper(c) || ascii_lower(c);
}

// Key of the code point occupying bytes [b, e) (1..4 bytes): the bytes packed big-endian.
// Distinct byte sequences of valid-or-escaped code points get distinct keys.
[[nodiscard]] std::uint32_t pack(std::string_view s, std::size_t b, std::size_t e) noexcept {
    std::uint32_t key = 0;
    for (std::size_t i = b; i < e; ++i) {
        key = (key << 8U) | static_cast<unsigned char>(s[i]);
    }
    return key;
}

// Bonus for a term character aligned at code point j (search.hpp "Term alignment"); `first`
// returns the first RAW byte of a code point.
template <class First>
[[nodiscard]] std::int32_t position_bonus(std::size_t j, const First& first) noexcept {
    if (j == 0) {
        return kBonusPrefix;
    }
    const unsigned char prev = first(j - 1);
    if (prev >= 0x80) {
        return 0;
    }
    const unsigned char cur = first(j);
    if (!ascii_alpha(prev) && !ascii_digit(prev)) {
        return kBonusWordStart;
    }
    if (ascii_lower(prev) && ascii_upper(cur)) {
        return kBonusWordStart;
    }
    if (ascii_alpha(prev) && ascii_digit(cur)) {
        return kBonusWordStart;
    }
    return 0;
}

[[nodiscard]] std::int32_t leading_penalty(std::size_t p0) noexcept {
    return static_cast<std::int32_t>(
        std::min<std::size_t>(static_cast<std::size_t>(kPenaltyLeadingMax),
                              static_cast<std::size_t>(kPenaltyLeading) * p0));
}

// Fuzzy DP over one term. keys/bonus: name arrays of length L; tk: term keys, length m.
// Fills `positions` (when not null) with the lexicographically smallest optimal alignment.
[[nodiscard]] std::int32_t fuzzy_term(const std::uint32_t* keys, const std::uint32_t* bonus,
                                      std::size_t L, const std::uint32_t* tk, std::size_t m,
                                      std::vector<std::int32_t>& table,
                                      std::vector<std::uint32_t>* positions) {
    if (table.size() < m * L) {
        table.resize(m * L);
    }
    std::int32_t* const s = table.data();
    // Row m-1.
    {
        std::int32_t* row = s + ((m - 1) * L);
        for (std::size_t j = 0; j < L; ++j) {
            row[j] = keys[j] == tk[m - 1] ? kScoreChar + static_cast<std::int32_t>(bonus[j]) : kNeg;
        }
    }
    for (std::size_t kk = m - 1; kk-- > 0;) {
        std::int32_t* row = s + (kk * L);
        const std::int32_t* next = s + ((kk + 1) * L);
        std::int32_t best_far = kNeg; // max over j' >= j + 2 of next[j'] - gap * j'
        for (std::size_t j = L; j-- > 0;) {
            if (j + 2 < L && next[j + 2] > kValid) {
                best_far = std::max(best_far,
                                    next[j + 2] - (kPenaltyGap * static_cast<std::int32_t>(j + 2)));
            }
            if (keys[j] != tk[kk]) {
                row[j] = kNeg;
                continue;
            }
            std::int32_t best = kNeg;
            if (j + 1 < L && next[j + 1] > kValid) {
                best = next[j + 1] + kBonusConsecutive;
            }
            if (best_far > kValid) {
                best =
                    std::max(best, best_far + (kPenaltyGap * (static_cast<std::int32_t>(j) + 1)));
            }
            row[j] = best > kValid ? kScoreChar + static_cast<std::int32_t>(bonus[j]) + best : kNeg;
        }
    }
    std::int32_t total = kNeg;
    std::size_t p0 = 0;
    for (std::size_t j = 0; j < L; ++j) {
        if (s[j] > kValid) {
            const std::int32_t v = s[j] - leading_penalty(j);
            if (v > total) {
                total = v;
                p0 = j;
            }
        }
    }
    if (total <= kValid || positions == nullptr) {
        return total;
    }
    positions->push_back(static_cast<std::uint32_t>(p0));
    std::size_t prev = p0;
    for (std::size_t kk = 1; kk < m; ++kk) {
        const std::int32_t* row = s + (kk * L);
        const std::int32_t want =
            s[((kk - 1) * L) + prev] - kScoreChar - static_cast<std::int32_t>(bonus[prev]);
        std::size_t pick = L;
        for (std::size_t j = prev + 1; j < L; ++j) {
            if (row[j] <= kValid) {
                continue;
            }
            const std::int32_t step = j == prev + 1
                                          ? kBonusConsecutive
                                          : -kPenaltyGap * static_cast<std::int32_t>(j - prev - 1);
            if (row[j] + step == want) {
                pick = j;
                break;
            }
        }
        if (pick == L) { // cannot happen: the DP guarantees a continuation
            return kNeg;
        }
        positions->push_back(static_cast<std::uint32_t>(pick));
        prev = pick;
    }
    return total;
}

void merge_ranges(std::vector<HighlightRange>& ranges) {
    std::sort(ranges.begin(), ranges.end(),
              [](const HighlightRange& a, const HighlightRange& b) { return a.begin < b.begin; });
    std::size_t w = 0;
    for (std::size_t r = 0; r < ranges.size(); ++r) {
        if (w > 0 && ranges[r].begin <= ranges[w - 1].end) {
            ranges[w - 1].end = std::max(ranges[w - 1].end, ranges[r].end);
        } else {
            ranges[w++] = ranges[r];
        }
    }
    ranges.resize(w);
}

} // namespace

bool may_match(const Query& query, std::string_view folded) noexcept {
    for (const std::string& term : query.terms) {
        if (query.mode == MatchMode::Substring) {
            if (folded.find(term) == std::string_view::npos) {
                return false;
            }
        } else {
            std::size_t i = 0;
            for (const char c : term) {
                const std::size_t at = folded.find(c, i);
                if (at == std::string_view::npos) {
                    return false;
                }
                i = at + 1;
            }
        }
    }
    return true;
}

std::optional<std::int32_t> evaluate(const Query& query, std::string_view raw, Scratch& scratch,
                                     std::vector<HighlightRange>* highlights,
                                     std::optional<std::string_view> prefolded) {
    if (query.empty()) {
        return 0;
    }
    if (prefolded && !may_match(query, *prefolded)) {
        return std::nullopt;
    }
    std::string& folded_buf = scratch.folded;
    std::vector<std::uint32_t>& offsets = scratch.offsets;
    std::string_view folded;
    bool ascii = false;
    if (prefolded) {
        ascii = is_ascii(raw);
        if (ascii) {
            folded = *prefolded; // ASCII: the stored fold is the lowercase name
        }
    } else {
        // One branch-free pass: ASCII lowercase and "any byte >= 0x80" (vectorizes).
        folded_buf.resize(raw.size());
        char* out = folded_buf.data();
        unsigned high = 0;
        for (std::size_t i = 0; i < raw.size(); ++i) {
            const auto u = static_cast<unsigned char>(raw[i]);
            high |= u;
            const auto upper = static_cast<unsigned>(static_cast<unsigned char>(u - 'A') < 26U);
            out[i] = static_cast<char>(u + (upper << 5U));
        }
        ascii = (high & 0x80U) == 0;
        if (ascii) {
            folded = folded_buf;
            if (!may_match(query, folded)) {
                return std::nullopt;
            }
        }
    }
    if (!ascii) {
        fs::fold_name_into(raw, folded_buf, &offsets);
        folded = folded_buf;
        if (!prefolded && !may_match(query, folded)) {
            return std::nullopt;
        }
    }

    // Code point layout in scratch.cps: [0, L] folded byte starts, [L+1, 2L+1) keys,
    // [2L+1, 3L+1) position bonuses, then the current term's keys.
    std::vector<std::uint32_t>& cps = scratch.cps;
    const std::size_t fsize = folded.size();
    std::size_t L = 0;
    const std::size_t need = (4 * fsize) + 2;
    if (cps.size() < need) {
        cps.resize(need); // grow only: no per-name zero fill
    }
    if (ascii) {
        L = fsize;
        for (std::size_t j = 0; j <= L; ++j) {
            cps[j] = static_cast<std::uint32_t>(j);
        }
    } else {
        for (std::size_t b = 0; b < fsize; ++b) {
            if (b == 0 || offsets[b] != offsets[b - 1]) {
                cps[L++] = static_cast<std::uint32_t>(b);
            }
        }
        cps[L] = static_cast<std::uint32_t>(fsize);
    }
    std::uint32_t* const starts = cps.data();
    std::uint32_t* const keys = starts + L + 1;
    std::uint32_t* const bonus = keys + L;
    std::uint32_t* const tkeys = bonus + L;
    for (std::size_t j = 0; j < L; ++j) {
        keys[j] =
            ascii ? static_cast<unsigned char>(folded[j]) : pack(folded, starts[j], starts[j + 1]);
    }
    const auto raw_begin = [&](std::size_t j) -> std::uint32_t {
        return ascii ? static_cast<std::uint32_t>(j) : offsets[starts[j]];
    };
    const auto first = [&](std::size_t j) -> unsigned char {
        return static_cast<unsigned char>(raw[raw_begin(j)]);
    };
    for (std::size_t j = 0; j < L; ++j) {
        bonus[j] = static_cast<std::uint32_t>(position_bonus(j, first));
    }

    // Folded stem length in bytes (for kBonusExactStem), or npos.
    std::size_t stem_bytes = std::string_view::npos;
    const std::string_view ext = fs::extension(raw);
    if (!ext.empty()) {
        const std::size_t raw_stem = raw.size() - ext.size() - 1;
        if (ascii) {
            stem_bytes = raw_stem;
        } else {
            const auto it = std::lower_bound(offsets.begin(), offsets.end(),
                                             static_cast<std::uint32_t>(raw_stem));
            stem_bytes = static_cast<std::size_t>(it - offsets.begin());
        }
    }

    std::vector<std::uint32_t> positions;
    std::int32_t total = 0;
    for (const std::string& term : query.terms) {
        std::size_t m = 0;
        for (std::size_t b = 0; b < term.size();) {
            if (m == L) {
                return std::nullopt; // longer than the name (also bounds tkeys)
            }
            const std::size_t len = code_point_length(term, b);
            tkeys[m++] = pack(term, b, b + len);
            b += len;
        }
        if (m == 0 || m > L) {
            return std::nullopt;
        }
        std::int32_t best = kNeg;
        positions.clear();
        if (query.mode == MatchMode::Substring) {
            std::size_t best_start = 0;
            for (std::size_t st = 0; st + m <= L; ++st) {
                if (keys[st] != tkeys[0] || !std::equal(tkeys, tkeys + m, keys + st)) {
                    continue;
                }
                std::int32_t sc =
                    (kBonusConsecutive * static_cast<std::int32_t>(m - 1)) - leading_penalty(st);
                for (std::size_t k = 0; k < m; ++k) {
                    sc += kScoreChar + static_cast<std::int32_t>(bonus[st + k]);
                }
                if (sc > best) {
                    best = sc;
                    best_start = st;
                }
            }
            if (best <= kValid) {
                return std::nullopt;
            }
            if (highlights != nullptr) {
                for (std::size_t k = 0; k < m; ++k) {
                    positions.push_back(static_cast<std::uint32_t>(best_start + k));
                }
            }
        } else {
            best = fuzzy_term(keys, bonus, L, tkeys, m, scratch.table,
                              highlights != nullptr ? &positions : nullptr);
            if (best <= kValid) {
                return std::nullopt;
            }
        }
        if (m == L) {
            best += kBonusExactName;
        } else if (stem_bytes != std::string_view::npos && term.size() == stem_bytes &&
                   std::string_view(folded).substr(0, stem_bytes) == term) {
            best += kBonusExactStem;
        }
        total += best;
        if (highlights != nullptr) {
            for (const std::uint32_t p : positions) {
                const std::uint32_t b = raw_begin(p);
                const std::uint32_t e =
                    p + 1 < L ? raw_begin(p + 1) : static_cast<std::uint32_t>(raw.size());
                highlights->push_back(HighlightRange{b, e});
            }
        }
    }
    if (highlights != nullptr) {
        merge_ranges(*highlights);
    }
    return total;
}

bool allows_name(const TypeFilter& filter, bool directory_like, std::string_view name,
                 std::string& fold_buffer) {
    if (!filter.active()) {
        return true;
    }
    if (directory_like) {
        return !filter.hide_folders;
    }
    const std::string_view ext = fs::extension(name);
    if (ext.empty()) {
        return !filter.hide_no_extension;
    }
    if (filter.hidden_extensions.empty()) {
        return true;
    }
    fs::fold_name_into(ext, fold_buffer);
    return !std::binary_search(filter.hidden_extensions.begin(), filter.hidden_extensions.end(),
                               fold_buffer);
}

} // namespace detail

// ---------------------------------------------------------------------------------------------
// Query
// ---------------------------------------------------------------------------------------------

namespace {

// Length of the longest prefix of `text` that ends on a code point boundary and fits in
// kMaxQueryBytes.
[[nodiscard]] std::size_t cut_length(std::string_view text) noexcept {
    if (text.size() <= kMaxQueryBytes) {
        return text.size();
    }
    std::size_t i = 0;
    while (i < text.size()) {
        const std::size_t len = detail::code_point_length(text, i);
        if (i + len > kMaxQueryBytes) {
            break;
        }
        i += len;
    }
    return i;
}

// True when `prefix` (a prefix of `text`) ends on a code point boundary of `text`'s decoding.
[[nodiscard]] bool boundary_at(std::string_view text, std::size_t prefix) noexcept {
    std::size_t i = 0;
    while (i < prefix) {
        i += detail::code_point_length(text, i);
    }
    return i == prefix;
}

} // namespace

Query compile_query(std::string_view text, MatchMode mode) {
    Query q;
    q.mode = mode;
    q.text.assign(text.substr(0, cut_length(text)));
    std::vector<std::string_view> words;
    std::string_view rest = q.text;
    while (true) {
        const std::size_t b = rest.find_first_not_of(' ');
        if (b == std::string_view::npos) {
            break;
        }
        rest.remove_prefix(b);
        const std::size_t e = std::min(rest.find(' '), rest.size());
        words.push_back(rest.substr(0, e));
        rest.remove_prefix(e);
    }
    const std::size_t n = std::min(words.size(), kMaxQueryTerms);
    q.terms.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        if (i + 1 == kMaxQueryTerms && words.size() > kMaxQueryTerms) {
            std::string joined(words[i]);
            for (std::size_t k = i + 1; k < words.size(); ++k) {
                joined += ' ';
                joined.append(words[k]);
            }
            q.terms.push_back(fs::fold_name(joined));
        } else {
            q.terms.push_back(fs::fold_name(words[i]));
        }
    }
    return q;
}

bool narrows(const Query& prev, const Query& next) noexcept {
    if (prev.mode != next.mode) {
        return false;
    }
    const std::string_view p = prev.text;
    const std::string_view n = next.text;
    if (n.size() < p.size() || n.substr(0, p.size()) != p) {
        return false;
    }
    // prev may have been cut at kMaxQueryBytes when it is within one code point of the limit.
    if (p.size() + 4 > kMaxQueryBytes) {
        return p == n;
    }
    // Appending must not merge prev's trailing bytes into a longer code point.
    return boundary_at(n, p.size());
}

// ---------------------------------------------------------------------------------------------
// Matcher
// ---------------------------------------------------------------------------------------------

Matcher::Matcher(Query query) : query_(std::move(query)) {}

std::optional<Match> Matcher::match(std::string_view name) {
    detail::Scratch s{folded_, offsets_, starts_, table_};
    Match m;
    const std::optional<std::int32_t> score = detail::evaluate(query_, name, s, &m.highlights);
    if (!score) {
        return std::nullopt;
    }
    m.score = *score;
    return m;
}

std::optional<std::int32_t> Matcher::score(std::string_view name) {
    detail::Scratch s{folded_, offsets_, starts_, table_};
    return detail::evaluate(query_, name, s, nullptr);
}

bool Matcher::matches(std::string_view name) { return score(name).has_value(); }

std::optional<Match> match(std::string_view name, const Query& query) {
    Matcher m(query);
    return m.match(name);
}

// ---------------------------------------------------------------------------------------------
// Ranking
// ---------------------------------------------------------------------------------------------

bool ranks_before(const RankInfo& a, const RankInfo& b) noexcept {
    if (a.score != b.score) {
        return a.score > b.score;
    }
    if (a.depth != b.depth) {
        return a.depth < b.depth;
    }
    if (a.name.size() != b.name.size()) {
        return a.name.size() < b.name.size();
    }
    const fs::SortOptions opts{};
    const std::weak_ordering byname = fs::compare_names(a.name, b.name, opts);
    if (byname != 0) {
        return byname < 0;
    }
    if (a.name != b.name) {
        return a.name < b.name;
    }
    const std::weak_ordering byparent = fs::compare_names(a.parent, b.parent, opts);
    if (byparent != 0) {
        return byparent < 0;
    }
    return a.parent < b.parent;
}

} // namespace navigator::search
