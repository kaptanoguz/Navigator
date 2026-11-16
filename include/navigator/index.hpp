// navigator/index.hpp
//
// Purpose: file-name index interface used by search and flatten views.
// Owner:   contracts (header); implementation in src/search (role: search). STUB until the
//          search phase.
// Thread-safety: implementations must allow query() from any thread concurrently with the
//                index's own background updates. Long operations must honour cancellation.
#pragma once

#include <cstddef>
#include <vector>

#include <navigator/search.hpp>
#include <navigator/types.hpp>

namespace navigator {

class IIndex {
public:
    virtual ~IIndex() = default;

    // Search phase. Adds `root` to the indexed set; indexing runs in the background.
    virtual Result<void> add_root(const Path& root) = 0;
    // Search phase. At most `limit` hits, best first. Errors: Cancelled, InvalidArgument.
    virtual Result<std::vector<SearchHit>> query(const SearchQuery& query, std::size_t limit) = 0;
    // Search phase. Number of indexed entries (approximate while indexing).
    [[nodiscard]] virtual std::size_t size() const noexcept = 0;
};

} // namespace navigator
