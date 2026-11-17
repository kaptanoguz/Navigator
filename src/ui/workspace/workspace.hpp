// src/ui/workspace/workspace.hpp
//
// The tabs-and-panes program state of one window (docs/architecture.md, "Phase 3"): the live
// layout::Session (layout tree, active pane, closed tabs, window geometry) plus one BrowserModel
// per tab, kept in step with the Layout. Headless: no SDL, no renderer; the App owns one and
// drives it at frame start (finished listings) and frame end (pointer intents, then commands).
//
// Rules implemented here:
//   - Only the active tab of each pane has a live model (listing worker + watch); every other
//     tab's model is suspended and keeps its state. Activating a tab resumes (re-lists) it.
//     Tabs restored from a session are created suspended and list on first activation.
//   - Every Layout edit is all-or-nothing. Before an edit the models' state is written back
//     into the tabs' layout::TabState (so closed tabs carry their last folder, cursor and
//     scroll); after it models are created, resumed, suspended or dropped to match the tree.
//   - The layout is never visibly empty: tab.close on the last tab of the last pane is a no-op,
//     and a loaded session or layout without tabs gets one tab on the start folder.
//   - Ids are fresh whenever the Layout object is replaced (named layout load); layout_epoch()
//     changes then, and every id-keyed cache of the caller (views, drag state) must be dropped.
// Thread-safety: UI thread only.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <navigator/commands.hpp>
#include <navigator/layout.hpp>
#include <navigator/types.hpp>

#include "browser/browser_model.hpp"
#include "workspace/drag_drop.hpp"

namespace navigator::ui {

class Workspace {
public:
    struct Options {
        // Template for every tab's model (watcher factory, launcher, wake). start_suspended is
        // managed by the workspace.
        BrowserModel::Options model;
        Path home;            // tab.new with NewTabArg::Home; start folder fallback
        float row_dp = 24.0F; // row height used to store scroll positions in rows
        // Navigator config root (session.json, layouts/). Empty: named layouts unavailable
        // (save/load/delete fail with Unsupported).
        Path config_root;
    };

    explicit Workspace(Options options);
    ~Workspace();
    Workspace(const Workspace&) = delete;
    Workspace& operator=(const Workspace&) = delete;
    Workspace(Workspace&&) = delete;
    Workspace& operator=(Workspace&&) = delete;

    // Startup: adopts `restored` (or a fresh session) and makes sure there is a tab. With a
    // non-empty `start_path` the session is kept and start_path opens as a new active tab
    // after the active tab of the active pane (the only tab without a session).
    void start(std::optional<layout::Session> restored, const Path& start_path);

    // ---- Frame start -------------------------------------------------------------------------
    bool frame_start(); // live models' finished work; true when anything visible changed

    // ---- State -------------------------------------------------------------------------------
    [[nodiscard]] const layout::Session& session() const noexcept { return session_; }
    [[nodiscard]] layout::WindowGeometry& window() noexcept { return session_.window; }
    [[nodiscard]] const layout::Layout& layout() const noexcept { return session_.layout; }
    [[nodiscard]] const layout::ClosedTabs& closed_tabs() const noexcept {
        return session_.closed_tabs;
    }
    [[nodiscard]] BrowserModel* model(layout::TabId tab) noexcept;
    [[nodiscard]] const BrowserModel* model(layout::TabId tab) const noexcept;
    [[nodiscard]] BrowserModel& active_model();
    [[nodiscard]] layout::TabId active_tab() const noexcept { return layout().active_tab(); }
    [[nodiscard]] layout::PaneId active_pane() const noexcept { return layout().active_pane(); }
    // Active tab of every pane, in pane order.
    [[nodiscard]] std::vector<layout::TabId> visible_tabs() const;
    // Every visible tab has listed once and none is listing.
    [[nodiscard]] bool settled() const;
    [[nodiscard]] std::uint64_t layout_epoch() const noexcept { return epoch_; }
    // Increases whenever something persisted changes (layout edits, closed tabs, a tab's folder,
    // sort or hidden setting); cursor and scroll alone do not count. See sync().
    [[nodiscard]] std::uint64_t persist_serial() const noexcept { return persist_serial_; }

    // ---- Frame end ---------------------------------------------------------------------------
    // Tab, pane and list.open_in_new_tab commands, and browser commands for the active tab.
    // Returns false for ids it does not handle (app level, and the popup commands tab.search,
    // layout.save/load/delete, which the shell turns into popups).
    bool execute(const Command& command);

    // Pointer intents.
    void activate_pane(layout::PaneId pane);
    void activate_tab(layout::TabId tab);
    bool drop(layout::TabId tab, const DropTarget& target);
    void move_splitter(layout::SplitId split, std::size_t splitter, double position,
                       double min_fraction);
    void equalize(layout::SplitId split);
    // Search Tabs picks.
    void reopen_closed_at(std::size_t closed_index);

    // Named layouts (synchronous; small files, user-initiated). Errors from layout.hpp;
    // Unsupported without a config root.
    [[nodiscard]] Result<std::vector<std::string>> list_layouts() const;
    [[nodiscard]] bool layout_exists(std::string_view name) const;
    Result<void> save_layout(std::string_view name);
    // Replaces the current layout; its tabs go to the closed-tabs list.
    Result<void> load_layout(std::string_view name);
    Result<void> delete_layout(std::string_view name) const;

    // Writes every model's state into its tab's TabState. Returns true (and bumps
    // persist_serial()) when a persisted field other than cursor/scroll changed.
    bool sync();
    // A copy for the background writer (call sync() first).
    [[nodiscard]] layout::Session snapshot() const { return session_; }

private:
    [[nodiscard]] layout::TabState new_tab_state(layout::PaneId pane, bool home) const;
    [[nodiscard]] layout::TabId target_tab(std::uint64_t arg) const;
    [[nodiscard]] layout::PaneId target_pane(std::uint64_t arg) const;
    void reconcile();
    void push_closed(layout::TabState state, layout::PaneId pane, std::size_t index);
    void reopen(layout::ClosedTab closed);
    void changed() noexcept { ++persist_serial_; }
    [[nodiscard]] Path fallback_dir() const;
    std::unique_ptr<BrowserModel> make_model() const;

    Options options_;
    layout::Session session_;
    std::map<std::uint32_t, std::unique_ptr<BrowserModel>> models_; // by TabId::value
    std::uint64_t epoch_ = 1;
    std::uint64_t persist_serial_ = 0;
    Path start_dir_;
};

// Absolute, lexically normal, no trailing separator (except "/"); relative paths resolve
// against the working directory.
[[nodiscard]] Path normalize_dir(const Path& dir);

} // namespace navigator::ui
