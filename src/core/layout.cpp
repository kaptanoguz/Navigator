// Layout tree, Layout operations and ClosedTabs (navigator/layout.hpp). Pure CPU work, no I/O.
//
// Every edit runs on a copy of the layout; the copy is normalized, checked with validate() and
// only then moved into *this, so a failed edit (including std::bad_alloc) leaves the layout
// unchanged. Layouts are small (a handful of panes), so the copy is cheap.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <new>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <navigator/layout.hpp>

namespace navigator::layout {
namespace {

constexpr double kSumTolerance = 1e-9;
// renormalize() leaves a split alone when its sum is this close to 1, so untouched splits keep
// their exact values (stable save -> load -> save text) while drift stays far below I5's 1e-9.
constexpr double kDriftTolerance = 1e-12;

// Thrown (and caught inside this file) when the per-Layout id counter wraps.
struct IdSpaceExhausted {};

[[nodiscard]] bool usable_path(const Path& path) { return !path.empty() && path.is_absolute(); }

[[nodiscard]] bool valid_axis(SplitAxis axis) noexcept {
    return axis == SplitAxis::Horizontal || axis == SplitAxis::Vertical;
}

[[nodiscard]] bool valid_edge(SplitEdge edge) noexcept {
    return edge == SplitEdge::Left || edge == SplitEdge::Right || edge == SplitEdge::Top ||
           edge == SplitEdge::Bottom;
}

std::uint32_t allocate_id(std::uint32_t& next) {
    if (next == 0) {
        throw IdSpaceExhausted{};
    }
    return next++; // wraps to 0 after UINT32_MAX; the next allocation then throws
}

// Child indices from the root to a node.
using NodePath = std::vector<std::size_t>;

bool find_pane_path(const LayoutNode& node, PaneId id, NodePath& path) {
    if (const auto* pane = std::get_if<PaneNode>(&node.node)) {
        return pane->id == id;
    }
    const auto& split = std::get<SplitNode>(node.node);
    for (std::size_t i = 0; i < split.children.size(); ++i) {
        path.push_back(i);
        if (find_pane_path(split.children[i], id, path)) {
            return true;
        }
        path.pop_back();
    }
    return false;
}

LayoutNode& node_at(LayoutNode& root, const NodePath& path, std::size_t length) {
    LayoutNode* node = &root;
    for (std::size_t i = 0; i < length; ++i) {
        node = &std::get<SplitNode>(node->node).children[path[i]];
    }
    return *node;
}

template <class Node, class Visit> void for_each_pane(Node& node, Visit&& visit) {
    if (auto* pane = std::get_if<PaneNode>(&node.node)) {
        visit(*pane);
        return;
    }
    for (auto& child : std::get<SplitNode>(node.node).children) {
        for_each_pane(child, visit);
    }
}

const PaneNode* find_pane_in(const LayoutNode& node, PaneId id) noexcept {
    if (const auto* pane = std::get_if<PaneNode>(&node.node)) {
        return pane->id == id ? pane : nullptr;
    }
    for (const LayoutNode& child : std::get<SplitNode>(node.node).children) {
        if (const PaneNode* found = find_pane_in(child, id)) {
            return found;
        }
    }
    return nullptr;
}

PaneNode* find_pane_in(LayoutNode& node, PaneId id) noexcept {
    return const_cast<PaneNode*>(find_pane_in(std::as_const(node), id));
}

const SplitNode* find_split_in(const LayoutNode& node, SplitId id) noexcept {
    const auto* split = std::get_if<SplitNode>(&node.node);
    if (split == nullptr) {
        return nullptr;
    }
    if (split->id == id) {
        return split;
    }
    for (const LayoutNode& child : split->children) {
        if (const SplitNode* found = find_split_in(child, id)) {
            return found;
        }
    }
    return nullptr;
}

std::optional<TabLocation> find_tab_in(const LayoutNode& node, TabId id) noexcept {
    if (const auto* pane = std::get_if<PaneNode>(&node.node)) {
        for (std::size_t i = 0; i < pane->tabs.size(); ++i) {
            if (pane->tabs[i].id == id) {
                return TabLocation{pane->id, i};
            }
        }
        return std::nullopt;
    }
    for (const LayoutNode& child : std::get<SplitNode>(node.node).children) {
        if (auto found = find_tab_in(child, id)) {
            return found;
        }
    }
    return std::nullopt;
}

std::vector<PaneId> panes_of(const LayoutNode& root) {
    std::vector<PaneId> order;
    for_each_pane(root, [&](const PaneNode& pane) { order.push_back(pane.id); });
    return order;
}

// Divides every split's sizes by their sum (removes floating-point drift).
void renormalize(LayoutNode& node) {
    auto* split = std::get_if<SplitNode>(&node.node);
    if (split == nullptr) {
        return;
    }
    double sum = 0.0;
    for (const double size : split->sizes) {
        sum += size;
    }
    if (std::isfinite(sum) && sum > 0.0 && std::abs(sum - 1.0) > kDriftTolerance) {
        for (double& size : split->sizes) {
            size /= sum;
        }
    }
    for (LayoutNode& child : split->children) {
        renormalize(child);
    }
}

// If the node at `path` (non-empty) is a split with its parent's axis, splices its children into
// the parent, scaling their sizes by the node's size (I6).
void flatten_at(LayoutNode& root, const NodePath& path) {
    auto& parent = std::get<SplitNode>(node_at(root, path, path.size() - 1).node);
    const std::size_t index = path.back();
    auto* child = std::get_if<SplitNode>(&parent.children[index].node);
    if (child == nullptr || child->axis != parent.axis) {
        return;
    }
    const double scale = parent.sizes[index];
    SplitNode moved = std::move(*child);
    const auto at = static_cast<std::ptrdiff_t>(index);
    parent.children.erase(parent.children.begin() + at);
    parent.sizes.erase(parent.sizes.begin() + at);
    for (double& size : moved.sizes) {
        size *= scale;
    }
    parent.children.insert(parent.children.begin() + at,
                           std::make_move_iterator(moved.children.begin()),
                           std::make_move_iterator(moved.children.end()));
    parent.sizes.insert(parent.sizes.begin() + at, moved.sizes.begin(), moved.sizes.end());
}

// Removes the node at `path` (non-empty) from its parent split and collapses the tree.
void remove_node(LayoutNode& root, const NodePath& path) {
    const NodePath parent_path(path.begin(), path.end() - 1);
    LayoutNode& parent_node = node_at(root, parent_path, parent_path.size());
    auto& split = std::get<SplitNode>(parent_node.node);
    const std::size_t index = path.back();
    const std::size_t recipient = index > 0 ? index - 1 : index + 1;
    split.sizes[recipient] += split.sizes[index];
    const auto at = static_cast<std::ptrdiff_t>(index);
    split.children.erase(split.children.begin() + at);
    split.sizes.erase(split.sizes.begin() + at);
    if (split.children.size() == 1) {
        LayoutNode only = std::move(split.children.front());
        parent_node = std::move(only); // inherits the split's slot (and size) in its parent
        if (!parent_path.empty()) {
            flatten_at(root, parent_path);
        }
    }
}

// Removes a pane that is not the only pane; fixes the active pane (pane_order position rule).
void remove_pane(LayoutNode& root, PaneId& active, PaneId pane) {
    const std::vector<PaneId> before = panes_of(root);
    const auto pos =
        static_cast<std::size_t>(std::find(before.begin(), before.end(), active) - before.begin());
    NodePath path;
    find_pane_path(root, pane, path);
    remove_node(root, path);
    if (active == pane) {
        const std::vector<PaneId> after = panes_of(root);
        active = after[std::min(pos, after.size() - 1)];
    }
}

// Erases tabs[index] and applies the tab activation rules; returns the tab.
Tab erase_tab(PaneNode& pane, std::size_t index) {
    Tab tab = std::move(pane.tabs[index]);
    pane.tabs.erase(pane.tabs.begin() + static_cast<std::ptrdiff_t>(index));
    if (pane.tabs.empty()) {
        pane.active_tab = 0;
    } else if (index < pane.active_tab) {
        --pane.active_tab;
    } else if (index == pane.active_tab && pane.active_tab >= pane.tabs.size()) {
        pane.active_tab = pane.tabs.size() - 1; // was last: left neighbour
    }
    return tab;
}

// Places `fresh` at `edge` of `target` (split_pane placement rules).
void place_new_pane(LayoutNode& root, PaneId target, SplitEdge edge, PaneNode fresh,
                    std::uint32_t& next_id) {
    NodePath path;
    find_pane_path(root, target, path);
    const SplitAxis axis = axis_of(edge);
    const bool before = edge == SplitEdge::Left || edge == SplitEdge::Top;
    if (!path.empty()) {
        auto& parent = std::get<SplitNode>(node_at(root, path, path.size() - 1).node);
        if (parent.axis == axis) {
            const std::size_t index = path.back();
            const double half = parent.sizes[index] / 2.0;
            parent.sizes[index] = half;
            const auto at = static_cast<std::ptrdiff_t>(before ? index : index + 1);
            parent.children.insert(parent.children.begin() + at, LayoutNode{std::move(fresh)});
            parent.sizes.insert(parent.sizes.begin() + at, half);
            return;
        }
    }
    LayoutNode& slot = node_at(root, path, path.size());
    SplitNode split;
    split.id = SplitId{allocate_id(next_id)};
    split.axis = axis;
    split.sizes = {0.5, 0.5};
    LayoutNode old = std::move(slot);
    if (before) {
        split.children.push_back(LayoutNode{std::move(fresh)});
        split.children.push_back(std::move(old));
    } else {
        split.children.push_back(std::move(old));
        split.children.push_back(LayoutNode{std::move(fresh)});
    }
    slot = LayoutNode{std::move(split)};
}

// ---- validate() ----------------------------------------------------------------------------

struct Checker {
    std::vector<std::uint32_t> panes;
    std::vector<std::uint32_t> tabs;
    std::vector<std::uint32_t> splits;
    std::size_t pane_count = 0;
    std::size_t empty_panes = 0;
    std::size_t max_depth = 0;
    std::string error;

    bool fail_with(std::string message) {
        error = std::move(message);
        return false;
    }

    bool visit(const LayoutNode& node, std::size_t depth, const SplitNode* parent) {
        max_depth = std::max(max_depth, depth);
        if (depth > kMaxTreeDepth) {
            return fail_with("I9: tree deeper than kMaxTreeDepth");
        }
        if (const auto* pane = std::get_if<PaneNode>(&node.node)) {
            ++pane_count;
            panes.push_back(pane->id.value);
            if (pane->tabs.empty()) {
                ++empty_panes;
                if (pane->active_tab != 0) {
                    return fail_with("I3: empty pane " + std::to_string(pane->id.value) +
                                     " has active_tab != 0");
                }
            } else if (pane->active_tab >= pane->tabs.size()) {
                return fail_with("I3: active_tab out of range in pane " +
                                 std::to_string(pane->id.value));
            }
            for (const Tab& tab : pane->tabs) {
                tabs.push_back(tab.id.value);
            }
            return true;
        }
        const auto& split = std::get<SplitNode>(node.node);
        splits.push_back(split.id.value);
        const std::string where = "split " + std::to_string(split.id.value);
        if (!valid_axis(split.axis)) {
            return fail_with("invalid axis in " + where);
        }
        if (split.children.size() < 2) {
            return fail_with("I4: fewer than 2 children in " + where);
        }
        if (split.sizes.size() != split.children.size()) {
            return fail_with("I4: sizes/children count mismatch in " + where);
        }
        double sum = 0.0;
        for (const double size : split.sizes) {
            if (!std::isfinite(size) || size <= 0.0) {
                return fail_with("I5: non-finite or non-positive size in " + where);
            }
            sum += size;
        }
        if (std::abs(sum - 1.0) > kSumTolerance) {
            return fail_with("I5: sizes do not sum to 1 in " + where);
        }
        if (parent != nullptr && parent->axis == split.axis) {
            return fail_with("I6: same-axis nesting at " + where);
        }
        for (const LayoutNode& child : split.children) {
            if (!visit(child, depth + 1, &split)) {
                return false;
            }
        }
        return true;
    }

    bool distinct(std::vector<std::uint32_t>& ids, const char* kind) {
        std::sort(ids.begin(), ids.end());
        if (!ids.empty() && ids.front() == 0) {
            return fail_with(std::string("I7: invalid ") + kind + " id");
        }
        if (std::adjacent_find(ids.begin(), ids.end()) != ids.end()) {
            return fail_with(std::string("I7: duplicate ") + kind + " id");
        }
        return true;
    }
};

// ---- from_tree() ---------------------------------------------------------------------------

// Rescales to sum 1, raising entries to at least `minimum` (water-filling) when the count allows.
void raise_to_minimum(std::vector<double>& sizes, double minimum) {
    const std::size_t n = sizes.size();
    if (static_cast<double>(n) * minimum <= 1.0) {
        std::vector<bool> fixed(n, false);
        std::size_t fixed_count = 0;
        for (std::size_t round = 0; round <= n; ++round) {
            double free_sum = 0.0;
            for (std::size_t i = 0; i < n; ++i) {
                if (!fixed[i]) {
                    free_sum += sizes[i];
                }
            }
            const double available = 1.0 - static_cast<double>(fixed_count) * minimum;
            if (!(free_sum > 0.0)) {
                break;
            }
            bool changed = false;
            for (std::size_t i = 0; i < n; ++i) {
                if (!fixed[i] && sizes[i] / free_sum * available < minimum) {
                    fixed[i] = true;
                    ++fixed_count;
                    changed = true;
                }
            }
            if (!changed) {
                for (std::size_t i = 0; i < n; ++i) {
                    sizes[i] = fixed[i] ? minimum : sizes[i] / free_sum * available;
                }
                break;
            }
        }
    }
    double sum = 0.0;
    for (const double size : sizes) {
        sum += size;
    }
    bool ok = std::isfinite(sum) && sum > 0.0;
    if (ok) {
        for (double& size : sizes) {
            size /= sum;
            ok = ok && std::isfinite(size) && size > 0.0;
        }
    }
    if (!ok) {
        std::fill(sizes.begin(), sizes.end(), 1.0 / static_cast<double>(n));
    }
}

// Finite, > 0, summing to 1 (I5) and each >= kDefaultMinFraction where the count allows.
bool sizes_already_valid(const std::vector<double>& sizes) {
    const bool need_minimum = static_cast<double>(sizes.size()) * kDefaultMinFraction <= 1.0;
    double sum = 0.0;
    for (const double size : sizes) {
        if (!std::isfinite(size) || size <= 0.0 || (need_minimum && size < kDefaultMinFraction)) {
            return false;
        }
        sum += size;
    }
    return std::abs(sum - 1.0) <= kSumTolerance;
}

struct Builder {
    std::uint32_t next = 1;

    std::optional<LayoutNode> build(const LayoutNode& in, std::size_t depth) {
        if (depth > kMaxTreeDepth) {
            return std::nullopt;
        }
        if (const auto* pane = std::get_if<PaneNode>(&in.node)) {
            return build_pane(*pane);
        }
        return build_split(std::get<SplitNode>(in.node), depth);
    }

    std::optional<LayoutNode> build_pane(const PaneNode& in) {
        const auto usable = static_cast<std::size_t>(
            std::count_if(in.tabs.begin(), in.tabs.end(),
                          [](const Tab& tab) { return usable_path(tab.state.path); }));
        if (usable == 0) {
            return std::nullopt;
        }
        PaneNode out;
        out.id = PaneId{allocate_id(next)};
        out.tabs.reserve(usable);
        for (std::size_t i = 0; i < in.tabs.size(); ++i) {
            if (!usable_path(in.tabs[i].state.path)) {
                continue;
            }
            if (i == in.active_tab) {
                out.active_tab = out.tabs.size();
            }
            out.tabs.push_back(Tab{TabId{allocate_id(next)}, in.tabs[i].state});
        }
        return LayoutNode{std::move(out)};
    }

    std::optional<LayoutNode> build_split(const SplitNode& in, std::size_t depth) {
        if (!valid_axis(in.axis)) {
            return std::nullopt;
        }
        bool equal = in.sizes.size() != in.children.size();
        double largest = 0.0;
        for (const double size : in.sizes) {
            if (!std::isfinite(size) || size <= 0.0) {
                equal = true;
            } else {
                largest = std::max(largest, size);
            }
        }
        std::vector<LayoutNode> kept;
        std::vector<double> weights;
        for (std::size_t i = 0; i < in.children.size(); ++i) {
            if (std::optional<LayoutNode> child = build(in.children[i], depth + 1)) {
                kept.push_back(std::move(*child));
                weights.push_back(equal ? 1.0 : in.sizes[i] / largest);
            }
        }
        if (kept.empty()) {
            return std::nullopt;
        }
        if (kept.size() == 1) {
            return std::move(kept.front());
        }
        SplitNode out;
        out.axis = in.axis;
        if (!equal && kept.size() == in.children.size() && sizes_already_valid(in.sizes) &&
            std::none_of(kept.begin(), kept.end(), [&](const LayoutNode& child) {
                const auto* s = std::get_if<SplitNode>(&child.node);
                return s != nullptr && s->axis == in.axis;
            })) {
            // Nothing to repair: keep the exact values (stable round trip).
            out.children = std::move(kept);
            out.sizes = in.sizes;
            out.id = SplitId{allocate_id(next)};
            return LayoutNode{std::move(out)};
        }
        double total = 0.0;
        for (const double weight : weights) {
            total += weight;
        }
        for (std::size_t i = 0; i < kept.size(); ++i) {
            const double share = weights[i] / total;
            auto* child = std::get_if<SplitNode>(&kept[i].node);
            if (child != nullptr && child->axis == out.axis) {
                for (std::size_t g = 0; g < child->children.size(); ++g) {
                    out.children.push_back(std::move(child->children[g]));
                    out.sizes.push_back(share * child->sizes[g]);
                }
            } else {
                out.children.push_back(std::move(kept[i]));
                out.sizes.push_back(share);
            }
        }
        raise_to_minimum(out.sizes, kDefaultMinFraction);
        out.id = SplitId{allocate_id(next)};
        return LayoutNode{std::move(out)};
    }
};

template <class T, class Fn> Result<T> transact(Layout& self, Fn&& edit) {
    try {
        Layout work = self;
        Result<T> result = edit(work);
        if (!result) {
            return result;
        }
        if (Result<void> valid = work.validate(); !valid) {
            return fail(ErrorCode::InvalidArgument,
                        "edit would break a layout invariant: " + valid.error().message);
        }
        self = std::move(work);
        return result;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "out of memory while editing the layout");
    } catch (const IdSpaceExhausted&) {
        return fail(ErrorCode::InvalidArgument, "layout id space exhausted");
    }
}

Error not_found(const char* what, std::uint32_t id) {
    return Error{ErrorCode::NotFound, std::string(what) + " " + std::to_string(id) + " not found"};
}

} // namespace

// ---- Layout ------------------------------------------------------------------------------------

Layout::Layout() : root_{PaneNode{PaneId{1}, {}, 0}}, active_pane_{1}, next_id_{2} {}

Layout::Layout(TabState first_tab) : Layout() {
    if (usable_path(first_tab.path)) {
        std::get<PaneNode>(root_.node).tabs.push_back(Tab{TabId{next_id_++}, std::move(first_tab)});
    }
}

Layout Layout::from_tree(const LayoutNode& root, std::size_t active_pane_index) {
    try {
        Builder builder;
        std::optional<LayoutNode> built = builder.build(root, 1);
        if (!built) {
            return {};
        }
        Layout out;
        out.root_ = std::move(*built);
        out.next_id_ = builder.next;
        const std::vector<PaneId> order = out.pane_order();
        out.active_pane_ = order[active_pane_index < order.size() ? active_pane_index : 0];
        if (!out.validate()) {
            return {};
        }
        return out;
    } catch (const IdSpaceExhausted&) {
        return {};
    }
}

const LayoutNode& Layout::root() const noexcept { return root_; }

PaneId Layout::active_pane() const noexcept { return active_pane_; }

TabId Layout::active_tab() const noexcept {
    const PaneNode* pane = find_pane(active_pane_);
    if (pane == nullptr || pane->tabs.empty()) {
        return TabId{};
    }
    return pane->tabs[pane->active_tab].id;
}

std::size_t Layout::pane_count() const noexcept {
    std::size_t count = 0;
    for_each_pane(root_, [&](const PaneNode&) { ++count; });
    return count;
}

std::size_t Layout::tab_count() const noexcept {
    std::size_t count = 0;
    for_each_pane(root_, [&](const PaneNode& pane) { count += pane.tabs.size(); });
    return count;
}

const PaneNode* Layout::find_pane(PaneId pane) const noexcept {
    return pane.valid() ? find_pane_in(root_, pane) : nullptr;
}

const SplitNode* Layout::find_split(SplitId split) const noexcept {
    return split.valid() ? find_split_in(root_, split) : nullptr;
}

std::optional<TabLocation> Layout::find_tab(TabId tab) const noexcept {
    return tab.valid() ? find_tab_in(root_, tab) : std::nullopt;
}

const TabState* Layout::tab_state(TabId tab) const noexcept {
    const std::optional<TabLocation> where = find_tab(tab);
    if (!where) {
        return nullptr;
    }
    return &find_pane(where->pane)->tabs[where->index].state;
}

TabState* Layout::tab_state(TabId tab) noexcept {
    return const_cast<TabState*>(std::as_const(*this).tab_state(tab));
}

std::vector<PaneId> Layout::pane_order() const { return panes_of(root_); }

PaneId Layout::neighbor_pane(PaneId pane, int delta) const {
    const std::vector<PaneId> order = pane_order();
    const auto it = std::find(order.begin(), order.end(), pane);
    if (!pane.valid() || it == order.end()) {
        return PaneId{};
    }
    const auto n = static_cast<long long>(order.size());
    const long long at = ((it - order.begin()) + static_cast<long long>(delta) % n + n) % n;
    return order[static_cast<std::size_t>(at)];
}

TabId Layout::neighbor_tab(TabId tab, int delta) const {
    const std::optional<TabLocation> where = find_tab(tab);
    if (!where) {
        return TabId{};
    }
    const PaneNode* pane = find_pane(where->pane);
    const auto n = static_cast<long long>(pane->tabs.size());
    const long long at =
        (static_cast<long long>(where->index) + static_cast<long long>(delta) % n + n) % n;
    return pane->tabs[static_cast<std::size_t>(at)].id;
}

Result<void> Layout::validate() const {
    try {
        Checker checker;
        if (!checker.visit(root_, 1, nullptr)) {
            return fail(ErrorCode::InvalidArgument, checker.error);
        }
        if (checker.pane_count == 0) {
            return fail(ErrorCode::InvalidArgument, "I1: no pane");
        }
        if (checker.empty_panes > 0 && checker.pane_count > 1) {
            return fail(ErrorCode::InvalidArgument, "I2: empty pane in a multi-pane layout");
        }
        if (!checker.distinct(checker.panes, "pane") || !checker.distinct(checker.tabs, "tab") ||
            !checker.distinct(checker.splits, "split")) {
            return fail(ErrorCode::InvalidArgument, checker.error);
        }
        if (find_pane(active_pane_) == nullptr) {
            return fail(ErrorCode::InvalidArgument, "I8: active pane not in the tree");
        }
        return ok();
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "out of memory in validate");
    }
}

Result<TabId> Layout::add_tab(PaneId pane, TabState state, std::size_t index, bool activate) {
    return transact<TabId>(*this, [&](Layout& work) -> Result<TabId> {
        PaneNode* target = pane.valid() ? find_pane_in(work.root_, pane) : nullptr;
        if (target == nullptr) {
            return Unexpected<Error>(not_found("pane", pane.value));
        }
        if (!usable_path(state.path)) {
            return fail(ErrorCode::InvalidArgument, "tab path is empty or relative");
        }
        const std::size_t at = std::min(index, target->tabs.size());
        const TabId id{allocate_id(work.next_id_)};
        target->tabs.insert(target->tabs.begin() + static_cast<std::ptrdiff_t>(at),
                            Tab{id, std::move(state)});
        if (activate || target->tabs.size() == 1) {
            target->active_tab = at;
        } else if (at <= target->active_tab) {
            ++target->active_tab;
        }
        if (activate) {
            work.active_pane_ = pane;
        }
        return id;
    });
}

Result<TabState> Layout::close_tab(TabId tab) {
    return transact<TabState>(*this, [&](Layout& work) -> Result<TabState> {
        const std::optional<TabLocation> where = work.find_tab(tab);
        if (!where) {
            return Unexpected<Error>(not_found("tab", tab.value));
        }
        PaneNode* pane = find_pane_in(work.root_, where->pane);
        Tab removed = erase_tab(*pane, where->index);
        if (pane->tabs.empty() && work.pane_count() > 1) {
            remove_pane(work.root_, work.active_pane_, where->pane);
        }
        renormalize(work.root_);
        return std::move(removed.state);
    });
}

Result<std::vector<TabState>> Layout::close_tabs_left_of(TabId tab) {
    return transact<std::vector<TabState>>(
        *this, [&](Layout& work) -> Result<std::vector<TabState>> {
            const std::optional<TabLocation> where = work.find_tab(tab);
            if (!where) {
                return Unexpected<Error>(not_found("tab", tab.value));
            }
            PaneNode* pane = find_pane_in(work.root_, where->pane);
            const std::size_t count = where->index;
            std::vector<TabState> states;
            states.reserve(count);
            for (std::size_t i = 0; i < count; ++i) {
                states.push_back(std::move(pane->tabs[i].state));
            }
            pane->tabs.erase(pane->tabs.begin(),
                             pane->tabs.begin() + static_cast<std::ptrdiff_t>(count));
            pane->active_tab = pane->active_tab < count ? 0 : pane->active_tab - count;
            return states;
        });
}

Result<std::vector<TabState>> Layout::close_pane(PaneId pane) {
    return transact<std::vector<TabState>>(
        *this, [&](Layout& work) -> Result<std::vector<TabState>> {
            PaneNode* node = pane.valid() ? find_pane_in(work.root_, pane) : nullptr;
            if (node == nullptr) {
                return Unexpected<Error>(not_found("pane", pane.value));
            }
            if (work.pane_count() == 1) {
                return fail(ErrorCode::InvalidArgument, "cannot close the only pane");
            }
            std::vector<TabState> states;
            states.reserve(node->tabs.size());
            for (Tab& t : node->tabs) {
                states.push_back(std::move(t.state));
            }
            remove_pane(work.root_, work.active_pane_, pane);
            renormalize(work.root_);
            return states;
        });
}

Result<void> Layout::move_tab(TabId tab, PaneId dest, std::size_t index) {
    return transact<void>(*this, [&](Layout& work) -> Result<void> {
        const std::optional<TabLocation> where = work.find_tab(tab);
        if (!where) {
            return Unexpected<Error>(not_found("tab", tab.value));
        }
        PaneNode* target = dest.valid() ? find_pane_in(work.root_, dest) : nullptr;
        if (target == nullptr) {
            return Unexpected<Error>(not_found("pane", dest.value));
        }
        PaneNode* source = find_pane_in(work.root_, where->pane);
        Tab moved = erase_tab(*source, where->index);
        const std::size_t at = std::min(index, target->tabs.size());
        target->tabs.insert(target->tabs.begin() + static_cast<std::ptrdiff_t>(at),
                            std::move(moved));
        target->active_tab = at;
        work.active_pane_ = dest;
        if (source != target && source->tabs.empty()) {
            remove_pane(work.root_, work.active_pane_, where->pane);
        }
        renormalize(work.root_);
        return ok();
    });
}

Result<SplitResult> Layout::split_pane(PaneId target, SplitEdge edge, TabId tab) {
    return transact<SplitResult>(*this, [&](Layout& work) -> Result<SplitResult> {
        if (!valid_edge(edge)) {
            return fail(ErrorCode::InvalidArgument, "invalid split edge");
        }
        const PaneNode* target_node = target.valid() ? find_pane_in(work.root_, target) : nullptr;
        if (target_node == nullptr) {
            return Unexpected<Error>(not_found("pane", target.value));
        }
        const std::optional<TabLocation> where = work.find_tab(tab);
        if (!where) {
            return Unexpected<Error>(not_found("tab", tab.value));
        }
        if (where->pane == target && target_node->tabs.size() == 1) {
            return fail(ErrorCode::InvalidArgument, "tab is the target pane's only tab");
        }
        PaneNode fresh;
        fresh.id = PaneId{allocate_id(work.next_id_)};
        fresh.tabs.push_back(erase_tab(*find_pane_in(work.root_, where->pane), where->index));
        const PaneId fresh_id = fresh.id;
        place_new_pane(work.root_, target, edge, std::move(fresh), work.next_id_);
        work.active_pane_ = fresh_id;
        if (find_pane_in(work.root_, where->pane)->tabs.empty()) {
            remove_pane(work.root_, work.active_pane_, where->pane);
        }
        renormalize(work.root_);
        return SplitResult{fresh_id, tab};
    });
}

Result<SplitResult> Layout::split_pane(PaneId target, SplitEdge edge, TabState state) {
    return transact<SplitResult>(*this, [&](Layout& work) -> Result<SplitResult> {
        if (!valid_edge(edge)) {
            return fail(ErrorCode::InvalidArgument, "invalid split edge");
        }
        if (target.valid() ? find_pane_in(work.root_, target) == nullptr : true) {
            return Unexpected<Error>(not_found("pane", target.value));
        }
        if (!usable_path(state.path)) {
            return fail(ErrorCode::InvalidArgument, "tab path is empty or relative");
        }
        PaneNode fresh;
        fresh.id = PaneId{allocate_id(work.next_id_)};
        const TabId tab{allocate_id(work.next_id_)};
        fresh.tabs.push_back(Tab{tab, std::move(state)});
        const PaneId fresh_id = fresh.id;
        place_new_pane(work.root_, target, edge, std::move(fresh), work.next_id_);
        work.active_pane_ = fresh_id;
        renormalize(work.root_);
        return SplitResult{fresh_id, tab};
    });
}

Result<void> Layout::activate_tab(TabId tab) {
    const std::optional<TabLocation> where = find_tab(tab);
    if (!where) {
        return Unexpected<Error>(not_found("tab", tab.value));
    }
    find_pane_in(root_, where->pane)->active_tab = where->index;
    active_pane_ = where->pane;
    return ok();
}

Result<void> Layout::activate_pane(PaneId pane) {
    if (find_pane(pane) == nullptr) {
        return Unexpected<Error>(not_found("pane", pane.value));
    }
    active_pane_ = pane;
    return ok();
}

Result<void> Layout::move_splitter(SplitId split, std::size_t splitter, double position,
                                   double min_fraction) {
    return transact<void>(*this, [&](Layout& work) -> Result<void> {
        auto* node = const_cast<SplitNode*>(work.find_split(split));
        if (node == nullptr) {
            return Unexpected<Error>(not_found("split", split.value));
        }
        if (splitter + 1 >= node->children.size()) {
            return fail(ErrorCode::InvalidArgument, "splitter index out of range");
        }
        if (!std::isfinite(position) || !std::isfinite(min_fraction) || min_fraction < 0.0 ||
            min_fraction >= 0.5) {
            return fail(ErrorCode::InvalidArgument, "invalid splitter position or min_fraction");
        }
        double start = 0.0;
        for (std::size_t i = 0; i < splitter; ++i) {
            start += node->sizes[i];
        }
        const double combined = node->sizes[splitter] + node->sizes[splitter + 1];
        double first = combined / 2.0;
        if (combined >= 2.0 * min_fraction) {
            first = std::clamp(position - start, min_fraction, combined - min_fraction);
        }
        // Keep both strictly positive even with min_fraction == 0 (I5).
        const double floor = combined * 1e-6;
        first = std::clamp(first, floor, combined - floor);
        node->sizes[splitter] = first;
        node->sizes[splitter + 1] = combined - first;
        renormalize(work.root_);
        return ok();
    });
}

Result<void> Layout::equalize(SplitId split) {
    return transact<void>(*this, [&](Layout& work) -> Result<void> {
        auto* node = const_cast<SplitNode*>(work.find_split(split));
        if (node == nullptr) {
            return Unexpected<Error>(not_found("split", split.value));
        }
        std::fill(node->sizes.begin(), node->sizes.end(),
                  1.0 / static_cast<double>(node->sizes.size()));
        return ok();
    });
}

// ---- ClosedTabs --------------------------------------------------------------------------------

ClosedTabs::ClosedTabs(std::size_t capacity) : capacity_(capacity == 0 ? 1 : capacity) {}

void ClosedTabs::push(ClosedTab tab) {
    if (entries_.size() >= capacity_) {
        entries_.erase(entries_.begin(), entries_.begin() + static_cast<std::ptrdiff_t>(
                                                                entries_.size() - capacity_ + 1));
    }
    entries_.push_back(std::move(tab));
}

std::optional<ClosedTab> ClosedTabs::pop() {
    if (entries_.empty()) {
        return std::nullopt;
    }
    ClosedTab tab = std::move(entries_.back());
    entries_.pop_back();
    return tab;
}

Result<ClosedTab> ClosedTabs::take(std::size_t index) {
    if (index >= entries_.size()) {
        return fail(ErrorCode::NotFound, "closed tab index out of range");
    }
    ClosedTab tab = std::move(entries_[index]);
    entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(index));
    return tab;
}

void ClosedTabs::clear() noexcept { entries_.clear(); }

std::size_t ClosedTabs::size() const noexcept { return entries_.size(); }

bool ClosedTabs::empty() const noexcept { return entries_.empty(); }

std::size_t ClosedTabs::capacity() const noexcept { return capacity_; }

Span<const ClosedTab> ClosedTabs::entries() const noexcept { return entries_; }

} // namespace navigator::layout
