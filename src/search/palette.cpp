// Command palette ranking and history (navigator/palette.hpp, rules R0-R7). Pure CPU work, no
// shared state. It includes navigator/commands.hpp only for value types and constexpr helpers;
// nothing here calls into navigator_commands.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <navigator/fs.hpp>
#include <navigator/palette.hpp>
#include <navigator/search.hpp>

namespace navigator::palette {
namespace {

constexpr std::size_t kAliasStepPenalty = 4;

[[nodiscard]] bool is_space(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

[[nodiscard]] std::string_view trim_ascii(std::string_view text) noexcept {
    while (!text.empty() && is_space(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && is_space(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

[[nodiscard]] char lower_ascii(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

// ASCII-case-insensitive; bytes >= 0x80 compare exactly (same rule as keymap_file.hpp
// alias_names_equal, reimplemented because src/search must not call src/commands).
[[nodiscard]] bool equal_ci(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (lower_ascii(a[i]) != lower_ascii(b[i])) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool is_applicable(const Entry& entry, const CommandContext& context) noexcept {
    return has(context.flags, entry.requires_all) && !has_any(context.flags, entry.hidden_if_any);
}

[[nodiscard]] std::uint32_t code_points(std::string_view text) noexcept {
    std::uint32_t n = 0;
    for (const char c : text) {
        if ((static_cast<unsigned char>(c) & 0xC0U) != 0x80U) {
            ++n;
        }
    }
    return n;
}

[[nodiscard]] bool is_runtime_id_kind(ArgKind kind) noexcept {
    return kind == ArgKind::OpId || kind == ArgKind::TabId || kind == ArgKind::PaneId ||
           kind == ArgKind::BookmarkId || kind == ArgKind::BookmarkRow;
}

// Per-hit data used by the ordering (R4).
struct Candidate {
    Hit hit;
    std::uint32_t title_cps = 0;
    std::string folded_title; // filled lazily, see ensure_folded
    bool folded = false;
};

class Ordering {
public:
    Ordering(Span<const Entry> entries, std::vector<Candidate>& candidates, bool by_score)
        : entries_(entries), candidates_(candidates), by_score_(by_score) {}

    // R4 (by_score) / R7 (browse: category, folded title, index).
    bool operator()(std::size_t a, std::size_t b) {
        Candidate& ca = candidates_[a];
        Candidate& cb = candidates_[b];
        const Entry& ea = entries_[ca.hit.entry];
        const Entry& eb = entries_[cb.hit.entry];
        if (by_score_) {
            if (ca.hit.score != cb.hit.score) {
                return ca.hit.score > cb.hit.score;
            }
            if (ca.title_cps != cb.title_cps) {
                return ca.title_cps < cb.title_cps;
            }
        }
        if (ea.category_id != eb.category_id) {
            return static_cast<std::uint8_t>(ea.category_id) <
                   static_cast<std::uint8_t>(eb.category_id);
        }
        const int cmp = ensure_folded(ca, ea).compare(ensure_folded(cb, eb));
        if (cmp != 0) {
            return cmp < 0;
        }
        return ca.hit.entry < cb.hit.entry;
    }

private:
    static const std::string& ensure_folded(Candidate& candidate, const Entry& entry) {
        if (!candidate.folded) {
            candidate.folded_title = fs::fold_name(entry.title);
            candidate.folded = true;
        }
        return candidate.folded_title;
    }

    Span<const Entry> entries_;
    std::vector<Candidate>& candidates_;
    bool by_score_;
};

// R5: when the first of `order` is destructive and a safe one exists, move the first safe one to
// the front, keeping the others in order. Returns the default index.
std::optional<std::size_t> apply_safe_default(std::vector<std::size_t>& order,
                                              const std::vector<Candidate>& candidates,
                                              Span<const Entry> entries) {
    if (order.empty()) {
        return std::nullopt;
    }
    const auto destructive = [&](std::size_t k) {
        return entries[candidates[k].hit.entry].danger == Danger::Destructive;
    };
    if (!destructive(order.front())) {
        return std::size_t{0};
    }
    const auto safe =
        std::find_if(order.begin(), order.end(), [&](std::size_t k) { return !destructive(k); });
    if (safe == order.end()) {
        return std::nullopt;
    }
    std::rotate(order.begin(), safe, safe + 1);
    return std::size_t{0};
}

SearchResult finish(std::vector<std::size_t>& order, std::vector<Candidate>& candidates,
                    Span<const Entry> entries, const SearchOptions& options) {
    SearchResult result;
    result.default_index = apply_safe_default(order, candidates, entries);
    const std::size_t count = std::min(order.size(), options.max_results);
    result.hits.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        result.hits.push_back(std::move(candidates[order[i]].hit));
    }
    if (count == 0) {
        result.default_index = std::nullopt;
    }
    return result;
}

SearchResult browse(Span<const Entry> entries, const History& history,
                    const CommandContext& context, const SearchOptions& options) {
    std::vector<Candidate> candidates;
    std::vector<std::size_t> applicable; // candidate ids
    std::unordered_map<std::string, std::size_t> by_key;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (!is_applicable(entries[i], context)) {
            continue;
        }
        Candidate c;
        c.hit.entry = i;
        c.hit.via = MatchedVia::Browse;
        candidates.push_back(std::move(c));
        applicable.push_back(candidates.size() - 1);
        if (is_recordable(entries[i])) {
            by_key.emplace(history_key(entries[i]), candidates.size() - 1); // first wins
        }
    }
    std::vector<std::size_t> order;
    std::vector<bool> taken(candidates.size(), false);
    for (const HistoryItem& item : history.items()) {
        if (order.size() >= kBrowseRecents) {
            break;
        }
        const auto found = by_key.find(item.key);
        if (found != by_key.end() && !taken[found->second]) {
            taken[found->second] = true;
            order.push_back(found->second);
        }
    }
    std::vector<std::size_t> rest;
    for (const std::size_t id : applicable) {
        if (!taken[id]) {
            rest.push_back(id);
        }
    }
    Ordering ordering(entries, candidates, false);
    std::sort(rest.begin(), rest.end(),
              [&](std::size_t a, std::size_t b) { return ordering(a, b); });
    order.insert(order.end(), rest.begin(), rest.end());
    return finish(order, candidates, entries, options);
}

struct Scored {
    std::int32_t score = 0;
    MatchedVia via = MatchedVia::Title;
    std::size_t alias = 0;
};

void consider(std::optional<Scored>& best, std::int32_t score, MatchedVia via,
              std::size_t alias = 0) {
    if (!best || score > best->score) {
        best = Scored{score, via, alias};
    }
}

} // namespace

std::string history_key(const Entry& entry) {
    if (entry.arg_kind == ArgKind::Flags && entry.command.arg != 0) {
        return entry.name + "#" + std::to_string(entry.command.arg);
    }
    return entry.name;
}

bool is_recordable(const Entry& entry) noexcept {
    if (entry.danger == Danger::Destructive) {
        return false;
    }
    return !is_runtime_id_kind(entry.arg_kind) || entry.command.arg == 0;
}

void History::record(std::string_view key) {
    if (key.empty()) {
        return;
    }
    std::uint32_t count = 1;
    const auto found = std::find_if(items_.begin(), items_.end(),
                                    [&](const HistoryItem& item) { return item.key == key; });
    if (found != items_.end()) {
        count = std::min(found->count + 1, kMaxHistoryCount);
        items_.erase(found);
    }
    items_.insert(items_.begin(), HistoryItem{std::string(key), count});
    if (items_.size() > kMaxHistoryItems) {
        items_.resize(kMaxHistoryItems);
    }
}

void History::clear() noexcept { items_.clear(); }

void History::assign(std::vector<HistoryItem> items) {
    std::vector<HistoryItem> kept;
    kept.reserve(std::min(items.size(), kMaxHistoryItems));
    for (HistoryItem& item : items) {
        if (kept.size() >= kMaxHistoryItems) {
            break;
        }
        if (item.key.empty() || item.count == 0) {
            continue;
        }
        const bool duplicate = std::any_of(kept.begin(), kept.end(), [&](const HistoryItem& other) {
            return other.key == item.key;
        });
        if (duplicate) {
            continue;
        }
        item.count = std::min(item.count, kMaxHistoryCount);
        kept.push_back(std::move(item));
    }
    items_ = std::move(kept);
}

std::optional<std::size_t> History::rank_of(std::string_view key) const noexcept {
    for (std::size_t i = 0; i < items_.size(); ++i) {
        if (items_[i].key == key) {
            return i;
        }
    }
    return std::nullopt;
}

std::uint32_t History::count_of(std::string_view key) const noexcept {
    for (const HistoryItem& item : items_) {
        if (item.key == key) {
            return item.count;
        }
    }
    return 0;
}

std::int32_t history_boost(const History& history, std::string_view key) noexcept {
    const Span<const HistoryItem> items = history.items();
    for (std::size_t rank = 0; rank < items.size(); ++rank) {
        if (items[rank].key != key) {
            continue;
        }
        const std::int64_t recent = std::max<std::int64_t>(
            0, std::int64_t{kHistoryRecentBoost} -
                   std::int64_t{kHistoryRecentStep} * static_cast<std::int64_t>(rank));
        const std::int64_t uses =
            std::min<std::int64_t>(items[rank].count, kHistoryCountCap) * kHistoryCountBoost;
        return static_cast<std::int32_t>(std::min<std::int64_t>(recent + uses, kHistoryBoostMax));
    }
    return 0;
}

SearchResult rank(std::string_view query, Span<const Entry> entries, const History& history,
                  const CommandContext& context, const SearchOptions& options) {
    try {
        if (entries.size() > kMaxEntries) {
            entries = entries.first(kMaxEntries);
        }
        const std::string_view text = trim_ascii(query);
        if (text.empty()) {
            return browse(entries, history, context, options);
        }

        search::Matcher fuzzy(search::compile_query(text, search::MatchMode::Fuzzy));
        search::Matcher substring(search::compile_query(text, search::MatchMode::Substring));
        std::string scratch;

        std::vector<Candidate> candidates;
        for (std::size_t i = 0; i < entries.size(); ++i) {
            const Entry& entry = entries[i];
            if (!is_applicable(entry, context)) {
                continue;
            }
            std::optional<Scored> best;
            if (const auto s = fuzzy.score(entry.title)) {
                consider(best, *s, MatchedVia::Title);
            }
            if (!entry.match_title.empty()) {
                if (const auto s = fuzzy.score(entry.match_title)) {
                    consider(best, *s, MatchedVia::Title);
                }
            }
            if (!entry.name.empty()) {
                if (const auto s = fuzzy.score(entry.name)) {
                    consider(best, *s - kNameDiscount, MatchedVia::Name);
                }
            }
            if (!entry.category.empty()) {
                scratch.assign(entry.category).append(" ").append(entry.title);
                if (const auto s = fuzzy.score(scratch)) {
                    consider(best, *s - kCategoryDiscount, MatchedVia::Category);
                }
            }
            if (!entry.description.empty()) {
                if (const auto s = substring.score(entry.description)) {
                    consider(best, *s - kDescriptionDiscount, MatchedVia::Description);
                }
            }
            for (std::size_t a = 0; a < entry.aliases.size(); ++a) {
                const std::string& alias = entry.aliases[a];
                if (equal_ci(text, alias)) {
                    consider(best, kAliasExactScore, MatchedVia::Alias, a);
                } else if (text.size() < alias.size() &&
                           equal_ci(text, std::string_view(alias).substr(0, text.size()))) {
                    const auto gap = static_cast<std::int64_t>(alias.size() - text.size());
                    const std::int64_t value = std::max<std::int64_t>(
                        kAliasPrefixScore - std::int64_t{kAliasStepPenalty} * gap,
                        kAliasPrefixScore / 2);
                    consider(best, static_cast<std::int32_t>(value), MatchedVia::Alias, a);
                }
            }
            if (best.has_value()) {
                const Scored chosen = best.value_or(Scored{});
                Candidate c;
                c.hit.entry = i;
                c.hit.score = chosen.score;
                c.hit.via = chosen.via;
                c.hit.alias = chosen.alias;
                if (is_recordable(entry)) {
                    c.hit.score += history_boost(history, history_key(entry));
                }
                c.title_cps = code_points(entry.title);
                if (chosen.via == MatchedVia::Title) {
                    // Highlights are for Entry::title; a generic-title win highlights only if the
                    // query also matches the displayed title.
                    if (auto m = fuzzy.match(entry.title)) {
                        c.hit.highlights = std::move(m->highlights);
                    }
                }
                candidates.push_back(std::move(c));
            }
        }

        std::vector<std::size_t> order(candidates.size());
        for (std::size_t k = 0; k < order.size(); ++k) {
            order[k] = k;
        }
        Ordering ordering(entries, candidates, true);
        std::sort(order.begin(), order.end(),
                  [&](std::size_t a, std::size_t b) { return ordering(a, b); });
        return finish(order, candidates, entries, options);
    } catch (const std::bad_alloc&) {
        return {};
    }
}

} // namespace navigator::palette
