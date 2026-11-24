// src/ops/batch_rename.cpp — Phase 9B pure batch rename planner & helpers.
#include <navigator/batch_rename.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <navigator/fs.hpp>

namespace navigator::ops {
namespace {

[[nodiscard]] bool is_ascii_space(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

[[nodiscard]] std::string trim_ascii(std::string_view s) {
    std::size_t first = 0;
    while (first < s.size() && is_ascii_space(s[first])) {
        ++first;
    }
    std::size_t last = s.size();
    while (last > first && is_ascii_space(s[last - 1])) {
        --last;
    }
    return std::string(s.substr(first, last - first));
}

[[nodiscard]] std::uint64_t splitmix64(std::uint64_t& state) noexcept {
    state += 0x9E3779B97F4A7C15ULL;
    std::uint64_t z = state;
    z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31U);
}

[[nodiscard]] std::string format_counter(int value, int width) {
    const bool negative = value < 0;
    const unsigned long long mag =
        negative ? (0ULL - static_cast<unsigned long long>(value))
                 : static_cast<unsigned long long>(value);
    std::string digits = std::to_string(mag);
    const std::size_t w = width > 0 ? static_cast<std::size_t>(std::clamp(width, 1, 12)) : 1U;
    if (digits.size() < w) {
        digits.insert(0, w - digits.size(), '0');
    }
    if (negative) {
        digits.insert(0, 1, '-');
    }
    return digits;
}

[[nodiscard]] int parse_int_token(std::string_view s, int fallback) noexcept {
    if (s.empty()) {
        return fallback;
    }
    int v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') {
            return fallback;
        }
        if (v > 1000) {
            return fallback;
        }
        v = (v * 10) + (c - '0');
    }
    return v;
}

[[nodiscard]] std::string expand_template(std::string_view tpl, const BatchRenameItem& item,
                                          std::size_t index, const BatchRenameRule& rule) {
    if (tpl.empty()) {
        tpl = "{name}";
    }
    const NameParts parts = split_extension(item.original_name, item.is_directory);
    std::string out;
    out.reserve(tpl.size() + item.original_name.size());

    std::size_t i = 0;
    while (i < tpl.size()) {
        if (tpl[i] != '{') {
            out.push_back(tpl[i]);
            ++i;
            continue;
        }
        const std::size_t close = tpl.find('}', i + 1);
        if (close == std::string_view::npos) {
            out.append(tpl.substr(i));
            break;
        }
        const std::string_view body = tpl.substr(i + 1, close - (i + 1));
        const std::size_t colon = body.find(':');
        const std::string_view key = colon == std::string_view::npos ? body : body.substr(0, colon);
        const std::string_view arg =
            colon == std::string_view::npos ? std::string_view{} : body.substr(colon + 1);

        if (key == "name") {
            out.append(item.original_name);
        } else if (key == "stem") {
            out.append(parts.stem);
        } else if (key == "ext") {
            out.append(parts.extension);
        } else if (key == "counter") {
            const long long val = static_cast<long long>(rule.counter_start) +
                                  (static_cast<long long>(index) *
                                   static_cast<long long>(rule.counter_step));
            const int width = arg.empty() ? 1 : parse_int_token(arg, 1);
            out.append(format_counter(static_cast<int>(val), width));
        } else if (key == "date") {
            out.append(format_batch_date(item.mtime, arg.empty() ? "YYYY-MM-DD" : arg));
        } else if (key == "uid") {
            const int len = arg.empty() ? 8 : parse_int_token(arg, 8);
            out.append(format_batch_uid(rule.uid_seed, index,
                                        static_cast<std::size_t>(std::clamp(len, 4, 32))));
        } else if (key == "parent") {
            const Path parent = item.path.parent_path();
            if (!parent.empty()) {
                out.append(fs::to_display_name(parent.filename().native()));
            }
        } else {
            // Unknown token: keep literal so the user sees it immediately.
            out.append(tpl.substr(i, close - i + 1));
        }
        i = close + 1;
    }
    return out;
}

[[nodiscard]] char ascii_lower(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

[[nodiscard]] char ascii_upper(char c) noexcept {
    return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
}

[[nodiscard]] std::string apply_find_replace(std::string_view input, std::string_view find,
                                             std::string_view replace, bool case_sensitive,
                                             bool replace_all) {
    if (find.empty()) {
        return std::string(input);
    }
    std::string haystack_cmp;
    std::string needle_cmp;
    std::string_view haystack_view = input;
    std::string_view needle_view = find;
    if (!case_sensitive) {
        haystack_cmp.reserve(input.size());
        for (char c : input) {
            haystack_cmp.push_back(ascii_lower(c));
        }
        needle_cmp.reserve(find.size());
        for (char c : find) {
            needle_cmp.push_back(ascii_lower(c));
        }
        haystack_view = haystack_cmp;
        needle_view = needle_cmp;
    }

    std::string out;
    out.reserve(input.size());
    std::size_t pos = 0;
    while (pos < input.size()) {
        const std::size_t found = haystack_view.find(needle_view, pos);
        if (found == std::string_view::npos) {
            out.append(input.substr(pos));
            break;
        }
        out.append(input.substr(pos, found - pos));
        out.append(replace);
        pos = found + find.size();
        if (!replace_all) {
            out.append(input.substr(pos));
            break;
        }
    }
    return out;
}

[[nodiscard]] std::string apply_case_transform(std::string_view name, bool is_directory,
                                               CaseTransform ct) {
    if (ct == CaseTransform::None) {
        return std::string(name);
    }
    const NameParts parts = split_extension(name, is_directory);
    std::string stem(parts.stem);
    if (ct == CaseTransform::Lower) {
        for (char& c : stem) {
            c = ascii_lower(c);
        }
    } else if (ct == CaseTransform::Upper) {
        for (char& c : stem) {
            c = ascii_upper(c);
        }
    } else if (ct == CaseTransform::Title) {
        bool start_word = true;
        for (char& c : stem) {
            const auto uc = static_cast<unsigned char>(c);
            if (uc >= 0x80U) {
                start_word = false;
            } else if (std::isalnum(uc) != 0) {
                c = start_word ? ascii_upper(c) : ascii_lower(c);
                start_word = false;
            } else {
                start_word = true;
            }
        }
    }
    stem.append(parts.extension);
    return stem;
}

} // namespace

std::vector<std::string> split_paste_lines(std::string_view text) {
    std::vector<std::string> lines;
    std::size_t pos = 0;
    while (pos <= text.size()) {
        const std::size_t nl = text.find('\n', pos);
        std::string_view line =
            (nl == std::string_view::npos) ? text.substr(pos) : text.substr(pos, nl - pos);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        lines.emplace_back(line);
        if (nl == std::string_view::npos) {
            break;
        }
        pos = nl + 1;
    }
    if (lines.size() > 1 && lines.back().empty()) {
        lines.pop_back();
    }
    return lines;
}

BatchPasteResult interpret_batch_paste(std::string_view clipboard_text,
                                       const std::vector<BatchRenameItem>& items,
                                       bool keep_extension) {
    BatchPasteResult res;
    std::vector<std::string> lines = split_paste_lines(clipboard_text);
    if (lines.empty()) {
        return res;
    }
    if (lines.size() == 1 && items.size() > 1) {
        res.is_single_line = true;
        std::string trimmed = trim_ascii(lines.front());
        if (trimmed.find('{') != std::string::npos || !keep_extension) {
            res.single_line_template = std::move(trimmed);
        } else {
            const NameParts p = split_extension(trimmed, false);
            if (p.extension.empty()) {
                res.single_line_template = trimmed + "{ext}";
            } else {
                res.single_line_template = std::move(trimmed);
            }
        }
        return res;
    }
    res.is_single_line = false;
    const std::size_t count = std::min(lines.size(), items.size());
    res.line_overrides.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        res.line_overrides.push_back(std::move(lines[i]));
    }
    return res;
}

std::string format_batch_uid(std::uint64_t seed, std::size_t index, std::size_t length) {
    constexpr char kHex[] = "0123456789abcdef";
    const std::size_t n = std::clamp(length, std::size_t{4}, std::size_t{32});
    std::uint64_t state = seed ^ (static_cast<std::uint64_t>(index) * 0x9E3779B97F4A7C15ULL);
    std::string out;
    out.reserve(n);
    while (out.size() < n) {
        std::uint64_t v = splitmix64(state);
        for (int shift = 60; shift >= 0 && out.size() < n; shift -= 4) {
            out.push_back(kHex[(v >> static_cast<unsigned>(shift)) & 0xFU]);
        }
    }
    return out;
}

std::string format_batch_date(Timestamp ts, std::string_view fmt) {
    if (fmt.empty()) {
        fmt = "YYYY-MM-DD";
    }
    const auto secs = std::chrono::floor<std::chrono::seconds>(ts).time_since_epoch().count();
    const auto tt = static_cast<std::time_t>(secs);
    struct tm tm_buf {};
    if (::localtime_r(&tt, &tm_buf) == nullptr) {
        ::gmtime_r(&tt, &tm_buf);
    }

    auto two = [](int v) -> std::string {
        std::array<char, 8> buf{};
        std::snprintf(buf.data(), buf.size(), "%02d", v);
        return std::string(buf.data());
    };
    auto four = [](int v) -> std::string {
        std::array<char, 12> buf{};
        std::snprintf(buf.data(), buf.size(), "%04d", v);
        return std::string(buf.data());
    };

    const int year = tm_buf.tm_year + 1900;
    const std::string yyyy = four(year);
    const std::string yy = two(year % 100);
    const std::string mm = two(tm_buf.tm_mon + 1);
    const std::string dd = two(tm_buf.tm_mday);
    const std::string hh = two(tm_buf.tm_hour);
    const std::string mi = two(tm_buf.tm_min);
    const std::string ss = two(tm_buf.tm_sec);

    std::string out;
    out.reserve(fmt.size() + 4);
    for (std::size_t i = 0; i < fmt.size();) {
        if (fmt.substr(i, 4) == "YYYY") {
            out.append(yyyy);
            i += 4;
        } else if (fmt.substr(i, 2) == "YY") {
            out.append(yy);
            i += 2;
        } else if (fmt.substr(i, 2) == "MM") {
            out.append(mm);
            i += 2;
        } else if (fmt.substr(i, 2) == "DD") {
            out.append(dd);
            i += 2;
        } else if (fmt.substr(i, 2) == "HH") {
            out.append(hh);
            i += 2;
        } else if (fmt.substr(i, 2) == "mm") {
            out.append(mi);
            i += 2;
        } else if (fmt.substr(i, 2) == "ss") {
            out.append(ss);
            i += 2;
        } else {
            out.push_back(fmt[i]);
            ++i;
        }
    }
    return out;
}

BatchRenamePlan build_batch_rename_plan(const std::vector<BatchRenameItem>& items,
                                        const BatchRenameRule& rule,
                                        const std::vector<std::string>& manual_overrides,
                                        const std::vector<std::string>& existing_sibling_names) {
    BatchRenamePlan plan;
    plan.rows.reserve(items.size());

    // Step 1: compute proposed names and validate syntax.
    for (std::size_t i = 0; i < items.size(); ++i) {
        const BatchRenameItem& item = items[i];
        BatchRenamePreviewRow row;
        row.index = i;

        std::string name;
        const bool has_manual = i < manual_overrides.size() && !manual_overrides[i].empty();
        if (has_manual) {
            name = manual_overrides[i];
        } else {
            name = expand_template(rule.template_str, item, i, rule);
            if (!rule.find_text.empty()) {
                name = apply_find_replace(name, rule.find_text, rule.replace_text,
                                          rule.find_case_sensitive, rule.find_replace_all);
            }
        }
        name = apply_case_transform(name, item.is_directory, rule.case_transform);
        if (rule.trim_whitespace) {
            name = trim_ascii(name);
        }
        row.proposed_name = std::move(name);
        row.name_error = validate_new_name(row.proposed_name);

        if (row.name_error != NameError::None) {
            row.status = PreviewRowStatus::InvalidName;
        } else if (row.proposed_name == item.original_name) {
            row.status = PreviewRowStatus::Unchanged;
        } else {
            row.status = PreviewRowStatus::Ready;
        }
        plan.rows.push_back(std::move(row));
    }

    // Step 2: collision detection keyed by (parent_dir + '\0' + name).
    auto make_key = [](const Path& item_path, std::string_view name) {
        std::string key = item_path.parent_path().native();
        key.push_back('\0');
        key.append(name);
        return key;
    };

    // Track vacated original names (items that are changing to a valid new name).
    std::unordered_set<std::string> vacated;
    vacated.reserve(items.size());
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (plan.rows[i].status == PreviewRowStatus::Ready) {
            vacated.insert(make_key(items[i].path, items[i].original_name));
        }
    }

    // Detect intra-batch collisions (including colliding with an Unchanged item in the batch).
    std::unordered_map<std::string, std::size_t> seen_targets;
    seen_targets.reserve(items.size());
    for (std::size_t i = 0; i < items.size(); ++i) {
        BatchRenamePreviewRow& row = plan.rows[i];
        if (row.status == PreviewRowStatus::InvalidName) {
            continue;
        }
        const std::string key = make_key(items[i].path, row.proposed_name);
        const auto [it, inserted] = seen_targets.emplace(key, i);
        if (!inserted) {
            const std::size_t other = it->second;
            row.status = PreviewRowStatus::CollisionInternal;
            row.collides_with = other;
            if (plan.rows[other].status == PreviewRowStatus::Ready ||
                plan.rows[other].status == PreviewRowStatus::Unchanged) {
                plan.rows[other].status = PreviewRowStatus::CollisionInternal;
                plan.rows[other].collides_with = i;
            }
        }
    }

    // Detect existing-sibling collisions (for Ready items whose target exists on disk and is not
    // being vacated by another item in the batch).
    if (!existing_sibling_names.empty()) {
        std::unordered_set<std::string_view> existing(existing_sibling_names.begin(),
                                                      existing_sibling_names.end());
        for (std::size_t i = 0; i < items.size(); ++i) {
            BatchRenamePreviewRow& row = plan.rows[i];
            if (row.status != PreviewRowStatus::Ready) {
                continue;
            }
            if (existing.contains(row.proposed_name)) {
                const std::string key = make_key(items[i].path, row.proposed_name);
                if (!vacated.contains(key)) {
                    row.status = PreviewRowStatus::CollisionExisting;
                }
            }
        }
    }

    // Step 3: tally counts.
    for (const BatchRenamePreviewRow& row : plan.rows) {
        switch (row.status) {
        case PreviewRowStatus::Unchanged: break;
        case PreviewRowStatus::Ready: ++plan.changed_count; break;
        case PreviewRowStatus::InvalidName: ++plan.invalid_count; break;
        case PreviewRowStatus::CollisionInternal:
        case PreviewRowStatus::CollisionExisting: ++plan.collision_count; break;
        }
    }
    return plan;
}

OpRequest make_batch_rename_request(const std::vector<BatchRenameItem>& items,
                                    const BatchRenamePlan& plan) {
    OpRequest req;
    req.kind = OpKind::BatchRename;
    if (!plan.can_apply() || items.size() != plan.rows.size()) {
        return req;
    }
    req.sources.reserve(plan.changed_count);
    req.new_names.reserve(plan.changed_count);
    for (std::size_t i = 0; i < plan.rows.size(); ++i) {
        if (plan.rows[i].status == PreviewRowStatus::Ready) {
            req.sources.push_back(items[i].path);
            req.new_names.push_back(plan.rows[i].proposed_name);
        }
    }
    return req;
}

} // namespace navigator::ops
