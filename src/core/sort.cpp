// sort_entries() (navigator/fs.hpp, "Sorting").
#include <algorithm>
#include <compare>
#include <string_view>

#include <navigator/fs.hpp>

namespace navigator::fs {
namespace {

template <class T> [[nodiscard]] constexpr std::weak_ordering three_way(const T& a, const T& b) {
    if (a < b) {
        return std::weak_ordering::less;
    }
    if (b < a) {
        return std::weak_ordering::greater;
    }
    return std::weak_ordering::equivalent;
}

// Steps 2-4 of the comparison chain, ascending.
[[nodiscard]] std::weak_ordering compare_by_key(const DirEntry& a, const DirEntry& b,
                                                const SortOptions& options) noexcept {
    std::weak_ordering r = std::weak_ordering::equivalent;
    bool names_done = false;
    switch (options.key) {
    case SortKey::Size: r = three_way(a.size, b.size); break;
    case SortKey::Modified: r = three_way(a.mtime, b.mtime); break;
    case SortKey::Extension:
        r = compare_names(extension(a.name), extension(b.name), options);
        break;
    case SortKey::Name:
    case SortKey::Count:
    default:
        r = compare_names(a.name, b.name, options);
        names_done = true;
        break;
    }
    if (r != 0) {
        return r;
    }
    if (!names_done) {
        r = compare_names(a.name, b.name, options);
        if (r != 0) {
            return r;
        }
    }
    return three_way(std::string_view{a.name}, std::string_view{b.name});
}

class EntryLess {
public:
    explicit EntryLess(const SortOptions& options) noexcept : options_(options) {}

    bool operator()(const DirEntry& a, const DirEntry& b) const noexcept {
        if (options_.folders_first) {
            const bool da = is_directory_like(a);
            const bool db = is_directory_like(b);
            if (da != db) {
                return da;
            }
        }
        const std::weak_ordering r = compare_by_key(a, b, options_);
        return options_.descending ? r > 0 : r < 0;
    }

private:
    SortOptions options_;
};

} // namespace

void sort_entries(Span<DirEntry> entries, const SortOptions& options) {
    // stable_sort: byte-identical names keep their input order (the chain is total otherwise).
    std::stable_sort(entries.begin(), entries.end(), EntryLess{options});
}

} // namespace navigator::fs
