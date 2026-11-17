// src/ui/workspace/tab_search.hpp
//
// Data of the Search Tabs popup (tab.search) and the tab labels: open tabs of all panes (pane
// order, then tab order) followed by recently closed tabs (newest first), filtered as the user
// types. Headless and unit-tested.
//
// Filtering: the query is split at spaces; every word must occur in the tab's title or in its
// folder path (display form), compared after fs::fold_name() so case and Turkish dotted/dotless
// i do not matter. The list keeps its order; items whose title starts with the first word move
// ahead of the others within the same group (open / closed).
// Thread-safety: pure functions; tr() rules (never concurrently with set_language()).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <navigator/layout.hpp>
#include <navigator/types.hpp>

namespace navigator::ui {

// Tab label: the folder's display name; "/" -> tr("path.root").
[[nodiscard]] std::string tab_title(const Path& dir);
// Labels for the tabs of one strip: tab_title(), except that tabs whose titles collide (but
// whose folders differ) get the nearest parent folder name that tells them apart, as
// "parent › name" ("Projects › src" vs "Website › src"). Identical folders keep the plain title.
[[nodiscard]] std::vector<std::string> disambiguated_titles(const std::vector<Path>& dirs);

// Folder path for display, with the home folder shown as "~" (raw bytes made displayable).
[[nodiscard]] std::string tab_detail(const Path& dir, const Path* home);

struct TabSearchItem {
    enum class Source : std::uint8_t { Open, Closed };
    Source source = Source::Open;
    layout::TabId tab;            // Open
    layout::PaneId pane;          // Open
    std::size_t pane_number = 0;  // Open: 1-based position in pane_order()
    std::size_t closed_index = 0; // Closed: index into ClosedTabs::entries()
    bool active = false;          // Open: the active tab of the active pane
    std::string title;
    std::string detail;
};

[[nodiscard]] std::vector<TabSearchItem> collect_tab_search_items(const layout::Layout& layout,
                                                                  const layout::ClosedTabs& closed,
                                                                  const Path* home);

// Indices into `items` that match `query` (see above). An empty query keeps every item.
[[nodiscard]] std::vector<std::size_t> filter_tab_search(const std::vector<TabSearchItem>& items,
                                                         std::string_view query);

} // namespace navigator::ui
