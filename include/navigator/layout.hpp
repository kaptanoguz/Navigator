// navigator/layout.hpp
//
// Purpose: the headless tab / split-pane model (Phase 3): strong ids, per-tab persisted state,
//          the layout tree with its invariants and edit operations, the recently-closed-tabs
//          ring, the Session (window geometry + layout + closed tabs + Phase 4 sidebar state),
//          JSON (de)serialization and the on-disk session / named-layout files. No SDL, no
//          renderer, no JSON library types: the ui draws and drives this model; src/core
//          implements it.
// Owner:   contracts (header); implementation in src/core (role: core-fs), tests in tests/core.
// Thread-safety: TabState, LayoutNode, Layout, ClosedTabs, WindowGeometry, SidebarState and
//                Session are plain values (see types.hpp): distinct objects may be used from
//                different threads, one object must not be mutated concurrently. In the app the
//                live Session is owned by the UI thread. Layout and ClosedTabs member functions
//                and the JSON functions are pure CPU work (no I/O) and may run on the UI thread.
//                The file functions (load_session, save_session, *_named_layout) perform
//                blocking file I/O; see docs/architecture.md "Phase 3" for where the ui may call
//                them. All functions are reentrant: concurrent calls on distinct objects / files
//                are safe, and two concurrent saves to the same file never produce a torn file
//                (last rename wins).
// Errors:  navigator::Result (types.hpp). No exceptions escape these functions; allocation
//          failure is reported as ErrorCode::OutOfMemory where a Result is returned.
//
// Vocabulary
//   Pane:  a rectangle of the window that shows one tab at a time and owns an ordered tab list.
//   Tab:   one browsing location (folder + view state) inside a pane.
//   Split: an inner node that divides its rectangle among >= 2 children along one axis.
//   The tree is n-ary: three side-by-side panes are ONE horizontal split with three children,
//   not nested binary splits (see the invariants on Layout).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <navigator/fs.hpp>
#include <navigator/types.hpp>

namespace navigator::layout {

// ---------------------------------------------------------------------------------------------
// Strong ids
// ---------------------------------------------------------------------------------------------
//
// Runtime identities, unique within ONE Layout object for its whole lifetime (allocated from a
// per-Layout counter, never reused, never 0). They are NOT persisted: Layout::from_tree() and
// every *_from_json() assign fresh ids, so a loaded or replaced layout has new ids and any
// ui-side map keyed by id (BrowserModel per tab, ...) must be rebuilt. value == 0 is "no id".
// Command::arg carries `value` for tab/pane commands (see commands.hpp).

struct PaneId {
    std::uint32_t value = 0;
    [[nodiscard]] constexpr bool valid() const noexcept { return value != 0; }
    friend constexpr bool operator==(PaneId, PaneId) = default;
    friend constexpr auto operator<=>(PaneId, PaneId) = default;
};

struct TabId {
    std::uint32_t value = 0;
    [[nodiscard]] constexpr bool valid() const noexcept { return value != 0; }
    friend constexpr bool operator==(TabId, TabId) = default;
    friend constexpr auto operator<=>(TabId, TabId) = default;
};

struct SplitId {
    std::uint32_t value = 0;
    [[nodiscard]] constexpr bool valid() const noexcept { return value != 0; }
    friend constexpr bool operator==(SplitId, SplitId) = default;
    friend constexpr auto operator<=>(SplitId, SplitId) = default;
};

// ---------------------------------------------------------------------------------------------
// Per-tab state (what is persisted for a tab)
// ---------------------------------------------------------------------------------------------
//
// Runtime-only tab data (listing, selection, back/forward history, pending work) lives in the
// ui's BrowserModel, not here. The ui copies these fields in and out of the BrowserModel (see
// docs/architecture.md "Phase 3").
struct TabState {
    // Folder shown by the tab. Absolute, raw bytes (need not be UTF-8). Never checked for
    // existence by this module: a missing folder is reported by the listing when the tab is
    // activated (Phase 2 rule: empty list + error notice).
    Path path;
    // Sort of this tab (all five fields are persisted).
    fs::SortOptions sort;
    bool show_hidden = false;
    // Name (raw bytes, no directory part) of the cursor row to restore; empty: first row.
    std::string cursor_name;
    // First visible row, in rows (fractional allowed), so the value survives density and font
    // changes. nullopt: scroll so that the cursor row is visible.
    std::optional<double> scroll_rows;

    friend bool operator==(const TabState&, const TabState&) = default;
};

// ---------------------------------------------------------------------------------------------
// Layout tree
// ---------------------------------------------------------------------------------------------

// Horizontal: children are laid out left to right (the splitters are vertical bars).
// Vertical:   children are laid out top to bottom (the splitters are horizontal bars).
enum class SplitAxis : std::uint8_t {
    Horizontal = 0,
    Vertical = 1,
};

// Where a dragged tab (or pane.split_* command) puts the new pane relative to the target pane.
// Left/Right split along SplitAxis::Horizontal, Top/Bottom along SplitAxis::Vertical.
enum class SplitEdge : std::uint8_t {
    Left = 0,
    Right = 1,
    Top = 2,
    Bottom = 3,
};

[[nodiscard]] constexpr SplitAxis axis_of(SplitEdge edge) noexcept {
    return edge == SplitEdge::Left || edge == SplitEdge::Right ? SplitAxis::Horizontal
                                                               : SplitAxis::Vertical;
}

struct Tab {
    TabId id;
    TabState state;
};

struct PaneNode {
    PaneId id;
    std::vector<Tab> tabs;      // left-to-right order in the tab bar
    std::size_t active_tab = 0; // index into tabs; 0 when tabs is empty
};

struct LayoutNode;

struct SplitNode {
    SplitId id;
    SplitAxis axis = SplitAxis::Horizontal;
    // Share of this split's extent along `axis` taken by each child, same order as children.
    // Each > 0 and finite; they sum to 1. Splitter k sits between children k and k+1.
    std::vector<double> sizes;
    std::vector<LayoutNode> children; // >= 2 in a valid Layout
};

struct LayoutNode {
    std::variant<PaneNode, SplitNode> node;
};

// Smallest share move_splitter() lets a child shrink to when the caller passes no better value.
// The ui passes min_pane_px / split_extent_px instead (design: a pane must keep its tab bar and a
// few rows usable).
inline constexpr double kDefaultMinFraction = 0.05;

// Deepest tree from_tree()/JSON loading keeps; deeper subtrees are dropped (never a crash or an
// unbounded recursion on hostile input).
inline constexpr std::size_t kMaxTreeDepth = 32;

// Where a tab currently is.
struct TabLocation {
    PaneId pane;
    std::size_t index = 0;
};

// Result of the split operations: the new pane and the tab it shows (the moved tab, or the new
// one).
struct SplitResult {
    PaneId pane;
    TabId tab;
};

// The pane tree plus the active pane. Always valid: every member function either applies its
// whole change or returns an error and leaves the layout unchanged.
//
// Invariants (checked by validate()):
//   I1  There is at least one pane. The root is a PaneNode or a SplitNode.
//   I2  Every pane has >= 1 tab, except that the layout's ONLY pane may have 0 tabs (e.g. a
//       freshly constructed Layout, or a loaded layout whose tabs were all invalid). The ui never
//       shows an empty layout: it adds a tab right away (docs/architecture.md).
//   I3  PaneNode::active_tab < tabs.size() (0 when tabs is empty).
//   I4  Every split has >= 2 children, and sizes.size() == children.size().
//   I5  Sizes are finite, > 0 and sum to 1 (within 1e-9 after every operation).
//   I6  No split has a child split with the same axis (same-axis nesting is flattened into the
//       parent, scaling the grandchildren's sizes by the child's size).
//   I7  All PaneId/TabId/SplitId values in the tree are valid and distinct.
//   I8  active_pane() names a pane of the tree.
//   I9  Depth (root = 1) <= kMaxTreeDepth.
//
// Normalization applied by every operation that removes something ("collapse"):
//   - A pane that loses its last tab is removed, unless it is the only pane.
//   - A removed child's size is added to the sibling that shared the splitter with it: the
//     previous sibling, or the next one when the removed child was first.
//   - A split left with one child is replaced by that child (which inherits the split's size in
//     its parent); if that child is a split with its new parent's axis it is flattened (I6).
//   - When the active pane is removed, the pane that now holds the same position in
//     pane_order() becomes active (the next pane), or the last pane if it was the last.
//
// Tab activation rules:
//   - Closing the active tab activates its right neighbour, or the left one if it was last.
//   - Closing an inactive tab keeps the active tab (its index is adjusted).
//   - add_tab(activate = true), move_tab, split_pane, activate_tab make that tab the active tab
//     of its pane AND make its pane the active pane.
class Layout {
public:
    // One pane with no tabs (I2 exception). The ui adds the first tab.
    Layout();

    // One pane with one tab. An empty or relative first_tab.path gives Layout() instead.
    explicit Layout(TabState first_tab);

    // Builds a valid layout from an arbitrary tree, as the JSON loader does. Ids in `root` are
    // ignored and fresh ids are assigned. Never fails; repairs instead:
    //   - tabs with an empty or relative path are dropped;
    //   - panes with no (remaining) tabs are dropped; active_tab out of range becomes 0;
    //   - splits with no remaining child are dropped, single-child splits collapse, same-axis
    //     nesting is flattened;
    //   - sizes: a missing, non-finite or <= 0 entry, or a size count that differs from the child
    //     count, makes the whole split equal-sized; otherwise sizes are rescaled to sum to 1 and
    //     then raised to at least kDefaultMinFraction where the child count allows it (and
    //     rescaled again);
    //   - subtrees deeper than kMaxTreeDepth are dropped;
    //   - if nothing remains, the result is Layout() (one empty pane).
    // `active_pane_index` is an index into the resulting pane_order(); out of range -> 0.
    [[nodiscard]] static Layout from_tree(const LayoutNode& root,
                                          std::size_t active_pane_index = 0);

    // ---- Queries ------------------------------------------------------------------------------

    [[nodiscard]] const LayoutNode& root() const noexcept;
    [[nodiscard]] PaneId active_pane() const noexcept;
    // Active tab of the active pane; invalid TabId when that pane has no tabs.
    [[nodiscard]] TabId active_tab() const noexcept;

    [[nodiscard]] std::size_t pane_count() const noexcept;
    [[nodiscard]] std::size_t tab_count() const noexcept;

    // nullptr when not in the tree. Valid until the next non-const call.
    [[nodiscard]] const PaneNode* find_pane(PaneId pane) const noexcept;
    [[nodiscard]] const SplitNode* find_split(SplitId split) const noexcept;
    [[nodiscard]] std::optional<TabLocation> find_tab(TabId tab) const noexcept;
    [[nodiscard]] const TabState* tab_state(TabId tab) const noexcept;
    // Mutable access to a tab's persisted state (the ui writes path/cursor/scroll/sort back as
    // the user browses). Changing TabState never affects an invariant.
    [[nodiscard]] TabState* tab_state(TabId tab) noexcept;

    // Panes in reading order: depth-first, children in order (left to right, top to bottom).
    // This is the focus-cycling order (pane.focus_next / pane.focus_previous) and the order the
    // JSON "active_pane" index refers to.
    [[nodiscard]] std::vector<PaneId> pane_order() const;

    // The pane `delta` steps away from `pane` in pane_order(), wrapping around. Invalid PaneId
    // when `pane` is not in the tree. delta 0 returns `pane`.
    [[nodiscard]] PaneId neighbor_pane(PaneId pane, int delta) const;

    // The tab `delta` steps away from `tab` in its pane, wrapping around (tab.next/previous).
    // Invalid TabId when `tab` is not in the tree.
    [[nodiscard]] TabId neighbor_tab(TabId tab, int delta) const;

    // Checks I1-I9. InvalidArgument with a description of the first violation. For tests and
    // debug assertions; a Layout obtained through this API always passes.
    [[nodiscard]] Result<void> validate() const;

    // ---- Edits (each one all-or-nothing) --------------------------------------------------------

    // Inserts a new tab into `pane` at `index` (clamped to [0, tabs.size()]).
    // Errors: NotFound (pane), InvalidArgument (empty or relative state.path).
    Result<TabId> add_tab(PaneId pane, TabState state, std::size_t index, bool activate);

    // Removes a tab and returns its state (the ui pushes it onto ClosedTabs). Removing the last
    // tab of the only pane leaves an empty pane (I2); the ui's policy is to refuse that case
    // before calling (docs/keymap.md: Ctrl+W on the last tab is a no-op).
    // Errors: NotFound (tab).
    Result<TabState> close_tab(TabId tab);

    // Closes every tab left of `tab` in its pane and returns their states in left-to-right
    // order (possibly empty). `tab` becomes active if the active tab was among them; otherwise
    // the active tab stays. Errors: NotFound (tab).
    Result<std::vector<TabState>> close_tabs_left_of(TabId tab);

    // Removes a pane with all its tabs (returned in tab order) and collapses the tree.
    // Errors: NotFound (pane), InvalidArgument (it is the only pane).
    Result<std::vector<TabState>> close_pane(PaneId pane);

    // Moves a tab to `dest` so that afterwards it sits at index min(index, final_count - 1) of
    // dest (same pane: reorder). The source pane is removed and the tree collapsed when it
    // becomes empty (and is not the only pane, which cannot happen here: dest still exists).
    // The tab becomes active and dest becomes the active pane.
    // Errors: NotFound (tab or dest).
    Result<void> move_tab(TabId tab, PaneId dest, std::size_t index);

    // Drag-to-split: moves `tab` (from any pane, including `target`) into a NEW pane placed at
    // `edge` of `target`:
    //   - if target's parent split has axis_of(edge), the new pane is inserted next to target in
    //     that split (before it for Left/Top, after it for Right/Bottom);
    //   - otherwise target is replaced by a new split of axis_of(edge) holding target and the
    //     new pane in edge order;
    //   - the new pane takes half of target's previous size; target keeps the other half;
    //   - the source pane collapses if it became empty.
    // The new pane is active and shows the tab.
    // Errors: NotFound (target or tab), InvalidArgument (tab is target's only tab: splitting a
    // pane with its own sole tab would leave target empty; the ui treats it as a no-op drop).
    Result<SplitResult> split_pane(PaneId target, SplitEdge edge, TabId tab);

    // Same placement, with a new tab created from `state` (pane.split_right / split_down).
    // Errors: NotFound (target), InvalidArgument (empty or relative state.path).
    Result<SplitResult> split_pane(PaneId target, SplitEdge edge, TabState state);

    // Makes `tab` the active tab of its pane and that pane the active pane. Errors: NotFound.
    Result<void> activate_tab(TabId tab);
    // Makes `pane` the active pane (its active tab is unchanged). Errors: NotFound.
    Result<void> activate_pane(PaneId pane);

    // Splitter drag. `splitter` k is the boundary between children k and k+1 of `split`;
    // `position` is the desired boundary as a fraction of the split's extent measured from its
    // start (left or top edge). Only children k and k+1 change size; their combined size is kept
    // and the boundary is clamped so each keeps >= min_fraction of the split's extent (if both
    // cannot, they get half of the combined size each).
    // Errors: NotFound (split), InvalidArgument (splitter >= children - 1, position or
    // min_fraction not finite, min_fraction < 0 or >= 0.5).
    Result<void> move_splitter(SplitId split, std::size_t splitter, double position,
                               double min_fraction = kDefaultMinFraction);

    // Gives every child of `split` the same size (splitter double-click). Errors: NotFound.
    Result<void> equalize(SplitId split);

private:
    LayoutNode root_;
    PaneId active_pane_;
    std::uint32_t next_id_ = 1;
};

// ---------------------------------------------------------------------------------------------
// Recently closed tabs (tab.reopen_closed, Search Tabs)
// ---------------------------------------------------------------------------------------------

inline constexpr std::size_t kDefaultClosedTabCapacity = 25;

struct ClosedTab {
    TabState state;
    // Where the tab was, so reopening can put it back: the pane (may no longer exist, and is
    // always invalid after loading a session) and its index in that pane at close time.
    PaneId pane;
    std::size_t index = 0;
};

// Bounded most-recent-last list. Pushing onto a full list drops the oldest entry.
// Reopen policy (ui): pop() the newest; if its pane still exists, add_tab(pane, state, index,
// true), otherwise add it after the active tab of the active pane.
class ClosedTabs {
public:
    ClosedTabs() = default; // capacity kDefaultClosedTabCapacity
    // capacity 0 is treated as 1.
    explicit ClosedTabs(std::size_t capacity);

    void push(ClosedTab tab);
    // Newest entry, removed. nullopt when empty.
    std::optional<ClosedTab> pop();
    // Removes and returns entry `index` (index into entries(), Search Tabs pick).
    // Errors: NotFound (index >= size()).
    Result<ClosedTab> take(std::size_t index);
    void clear() noexcept;

    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::size_t capacity() const noexcept;
    // Oldest first, newest last. Valid until the next non-const call.
    [[nodiscard]] Span<const ClosedTab> entries() const noexcept;

private:
    std::vector<ClosedTab> entries_;
    std::size_t capacity_ = kDefaultClosedTabCapacity;
};

// ---------------------------------------------------------------------------------------------
// Session
// ---------------------------------------------------------------------------------------------

// Window placement in logical (DPI-independent) pixels, as reported by the windowing system for
// the restored (non-maximized) window.
// Wayland does not let clients read or set the global window position: there has_position stays
// false and only the size and maximized state are restored. X11 reports and restores x/y.
struct WindowGeometry {
    bool has_position = false;
    int x = 0; // may be negative (monitor left of / above the primary one)
    int y = 0;
    int width = 1280;
    int height = 800;
    bool maximized = false;

    friend constexpr bool operator==(const WindowGeometry&, const WindowGeometry&) = default;
};

// Phase 4. One sidebar per window, left of the whole pane tree; it is not a layout node. Stored
// in the session only: named layouts never contain it, and loading a layout leaves it alone
// (docs/review/phase-3.md). The automatic icon-only collapse on narrow windows is derived by the
// ui from the window width every frame and is not persisted; `visible` is the user's choice
// (view.toggle_sidebar).
enum class SidebarSection : std::uint8_t {
    Bookmarks = 0,
    Recents = 1,
    Places = 2,
    Storage = 3,
};
inline constexpr std::size_t kSidebarSectionCount = 4;

// Logical pixels (dp) at scale 1.0 (design review: 220 default, 160 minimum).
inline constexpr double kSidebarDefaultWidth = 220.0;
inline constexpr double kSidebarMinWidth = 160.0;
inline constexpr double kSidebarMaxWidth = 480.0;

struct SidebarState {
    bool visible = true;
    double width = kSidebarDefaultWidth; // clamped to [kSidebarMinWidth, kSidebarMaxWidth]
    // Indexed by SidebarSection.
    std::array<bool, kSidebarSectionCount> collapsed{};

    [[nodiscard]] constexpr bool is_collapsed(SidebarSection section) const noexcept {
        return collapsed[static_cast<std::size_t>(section)];
    }
    constexpr void set_collapsed(SidebarSection section, bool value) noexcept {
        collapsed[static_cast<std::size_t>(section)] = value;
    }

    friend constexpr bool operator==(const SidebarState&, const SidebarState&) = default;
};

struct Session {
    // Format version written by to_json(). See "JSON format" below for the load rules.
    static constexpr int kVersion = 1;

    int version = kVersion; // version the session was loaded from (to_json always writes 1)
    WindowGeometry window;
    Layout layout; // includes the active pane
    ClosedTabs closed_tabs;
    SidebarState sidebar; // Phase 4 (additive "sidebar" field, still version 1)
};

// ---------------------------------------------------------------------------------------------
// JSON format (version 1)
// ---------------------------------------------------------------------------------------------
//
// Session file:
//   { "version": 1,
//     "window": { "x": 10, "y": 20, "width": 1280, "height": 800, "maximized": false },
//     "layout": <layout>,
//     "closed_tabs": [ <tab>, ... ],                        oldest first
//     "sidebar": { "visible": true, "width": 220,
//                  "collapsed": [ "recents", "storage" ] } }  Phase 4; collapsed sections in
//                                                             SidebarSection order
// Named layout file:
//   { "version": 1, "layout": <layout> }
// <layout> = { "active_pane": 0, "root": <node> }          active_pane: index in pane_order()
// <node>   = { "type": "pane", "active_tab": 0, "tabs": [ <tab>, ... ] }
//          | { "type": "split", "axis": "horizontal" | "vertical",
//              "sizes": [ 0.5, 0.5 ], "children": [ <node>, ... ] }
// <tab>    = { "path": <bytes>, "show_hidden": false, "cursor": <bytes>, "scroll": 12.5,
//              "sort": { "key": "name" | "size" | "modified" | "extension" | "count",
//                        "descending": false, "folders_first": true, "natural": true,
//                        "case_insensitive": true } }
// <bytes>  = a JSON string when the raw bytes are valid UTF-8, otherwise
//            { "hex": "<lowercase hex of the raw bytes>" }  (paths need not be UTF-8)
// "x"/"y" are written only when has_position; "cursor" only when non-empty; "scroll" only when
// set. Ids are never written. Output is UTF-8, 2-space indented, keys in the order shown, and
// deterministic: equal sessions serialize to identical text.
//
// Loading is lenient and never crashes (features.md: unknown fields ignored):
//   - The text must be a JSON object, else InvalidArgument. Everything below is repair, not
//     failure.
//   - Unknown fields at any level are ignored. A missing or wrong-typed field takes its default.
//   - "version": missing, not an integer or < 1 -> treated as 1. A higher version is still
//     loaded with the rules of this version (format rule: later versions only ADD fields; an
//     incompatible change must use new field names). Session::version records what was read.
//   - "window": width/height that are not integers in [1, 32768] fall back to the defaults;
//     has_position is true only when both x and y are integers in [-32768, 32768].
//   - <layout> is rebuilt with Layout::from_tree() (all repairs listed there); a missing or
//     invalid <layout> gives Layout(). Unknown "type"/"axis" values drop that node; an unknown
//     sort key becomes "name"; a non-finite or negative "scroll" is dropped; a <bytes> value
//     that is neither form (or bad hex) counts as missing.
//   - "closed_tabs": invalid entries (empty / relative path) are dropped; only the newest
//     kDefaultClosedTabCapacity are kept; pane is invalid, index 0.
//   - "sidebar" (Phase 4): missing (older files) or not an object -> SidebarState{}. "width"
//     not a finite number -> kSidebarDefaultWidth, else clamped to [kSidebarMinWidth,
//     kSidebarMaxWidth]; "visible" not a bool -> true; "collapsed": strings "bookmarks",
//     "recents", "places", "storage"; unknown or non-string items are ignored. "width" is
//     written as a JSON number (integral values without a fraction).

// Errors: OutOfMemory.
[[nodiscard]] Result<std::string> to_json(const Session& session);
// Errors: InvalidArgument (not JSON, or the top level is not an object).
[[nodiscard]] Result<Session> session_from_json(std::string_view text);

// Named layout documents (no window, no closed tabs).
[[nodiscard]] Result<std::string> layout_to_json(const Layout& layout);
[[nodiscard]] Result<Layout> layout_from_json(std::string_view text);

// ---------------------------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------------------------
//
// This module never looks up XDG directories itself (src/core does not depend on
// src/platform). The caller passes the Navigator config root, normally
//     platform::user_dir(platform::UserDir::Config) / kConfigDirName      (~/.config/navigator)
// or AppConfig::config_dir when set (tests). Inside it:
//     <root>/session.json                 session
//     <root>/layouts/<name>.json          named layouts

inline constexpr std::string_view kConfigDirName = "navigator";
inline constexpr std::string_view kSessionFileName = "session.json";
inline constexpr std::string_view kLayoutsDirName = "layouts";

// Files larger than this are rejected (InvalidArgument) without being parsed.
inline constexpr std::size_t kMaxDocumentBytes = std::size_t{16} * 1024U * 1024U;

[[nodiscard]] inline Path session_file(const Path& config_root) {
    return config_root / kSessionFileName;
}
[[nodiscard]] inline Path layouts_dir(const Path& config_root) {
    return config_root / kLayoutsDirName;
}

// Reads and parses a session file with session_from_json().
// Errors: InvalidArgument (empty path; file larger than kMaxDocumentBytes; not a JSON object --
// in that case the file is first renamed to "<file>.bad", replacing an older one, so the next
// save does not destroy what may be recoverable), NotFound (no file: first start),
// PermissionDenied, IoError, OutOfMemory.
[[nodiscard]] Result<Session> load_session(const Path& file);

// Serializes with to_json() and writes atomically: missing parent directories are created
// (mode 0700), the text goes to a unique temporary file in the same directory (mode 0600),
// which is fsync'ed and then rename()d over `file`; on any failure the temporary file is
// removed and the previous file is untouched. Readers therefore see the old or the new file,
// never a partial one.
// Errors: InvalidArgument (empty path), PermissionDenied, IoError (includes ENOSPC/EROFS),
// OutOfMemory.
Result<void> save_session(const Path& file, const Session& session);

// Layout names are user-visible and become file names "<name>.json". Valid when: 1..64 bytes,
// valid UTF-8, no '/', no NUL or other control characters (< 0x20, 0x7F), not starting with
// '.', and no leading or trailing space. Case-sensitive (raw bytes). Pure.
[[nodiscard]] bool is_valid_layout_name(std::string_view name) noexcept;

// Writes <dir>/<name>.json atomically (as save_session), replacing an existing layout of that
// name (the ui asks before overwriting). Errors: InvalidArgument (invalid name or empty dir),
// PermissionDenied, IoError, OutOfMemory.
Result<void> save_named_layout(const Path& dir, std::string_view name, const Layout& layout);

// Errors: InvalidArgument (invalid name; file too large or not a JSON object -- not renamed),
// NotFound, PermissionDenied, IoError, OutOfMemory.
[[nodiscard]] Result<Layout> load_named_layout(const Path& dir, std::string_view name);

// Names of the regular files "<name>.json" in `dir` whose name is valid, sorted with
// fs::compare_names (default SortOptions) then raw bytes. A missing `dir` is an empty list, not
// an error. Files are not parsed. Errors: PermissionDenied, IoError, OutOfMemory.
[[nodiscard]] Result<std::vector<std::string>> list_named_layouts(const Path& dir);

// Errors: InvalidArgument (invalid name), NotFound, PermissionDenied, IoError.
Result<void> delete_named_layout(const Path& dir, std::string_view name);

} // namespace navigator::layout
