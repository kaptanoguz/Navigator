// Phase 9A contract usage test (compile only, C++20 and C++23).
#include <navigator/commands.hpp>
#include <navigator/view_settings.hpp>

namespace {

using namespace navigator;
using namespace navigator::view;

static_assert(clamp_zoom(10) == kMinZoomPercent);
static_assert(clamp_zoom(999) == kMaxZoomPercent);
static_assert(clamp_zoom(125) == 125);
static_assert(step_zoom(ViewMode::Details, 100, 1).first == ViewMode::Icons);
static_assert(step_zoom(ViewMode::Details, 100, -1).first == ViewMode::Details);
static_assert(step_zoom(ViewMode::Icons, kMinZoomPercent, -1).first == ViewMode::Details);
static_assert(reorder_column(kDefaultColumnOrder, 0, 2)[2] == ColumnId::Name);
static_assert(next_view_mode(ViewMode::Tree) == ViewMode::Details);
static_assert(next_group_by(GroupBy::Size) == GroupBy::None);
static_assert(next_view_scope(ViewScope::Hierarchy) == ViewScope::Global);

} // namespace

void navigator_phase9a_contract_check();
void navigator_phase9a_contract_check() {
    using namespace navigator;
    using namespace navigator::view;
    ViewSettingsStore store;
    ViewSettings s;
    s.mode = ViewMode::Icons;
    s.zoom_percent = 150;
    s.group_by = GroupBy::Date;
    s.show_hidden = true;
    s.columns = reorder_column(kDefaultColumnOrder, 3, 1);

    store.set_global(s);
    store.set_hierarchy(Path{"/home/user"}, s);
    store.set_folder(Path{"/home/user/photos"}, s);
    const ResolvedViewSettings resolved = store.resolve(Path{"/home/user/photos"});
    (void)resolved.scope;
    (void)resolved.rule_path;
    store.apply_change(Path{"/home/user/photos"}, s);
    (void)store.cycle_lock(Path{"/home/user/photos"}, s);
    store.set_lock(Path{"/home/user/photos"}, ViewScope::Global, s);
    (void)store.clear_override(Path{"/home/user/photos"});
    (void)store.rules();
    (void)store.empty();

    (void)to_string(ViewMode::Tree);
    (void)parse_view_mode("tree");
    (void)to_string(GroupBy::Size);
    (void)parse_group_by("size");
    (void)to_string(ColumnId::Modified);
    (void)parse_column_id("modified");
    (void)to_string(ViewScope::Hierarchy);
    (void)parse_view_scope("hierarchy");

    (void)view_settings_file(Path{"/tmp"});
    (void)to_json(store);
    (void)from_json("{}");
    (void)load_view_settings(Path{"/tmp/view_settings.json"});
    (void)save_view_settings(Path{"/tmp/view_settings.json"}, store);

    const CommandId ids[] = {
        CommandId::ToggleViewLock,    CommandId::ViewModeDetails, CommandId::ViewModeIcons,
        CommandId::ViewModeList,      CommandId::ViewModeTree,    CommandId::ViewCycleMode,
        CommandId::ViewZoomIn,        CommandId::ViewZoomOut,     CommandId::ViewZoomReset,
        CommandId::GroupByNone,       CommandId::GroupByDate,     CommandId::GroupByType,
        CommandId::GroupBySize,       CommandId::GroupCycle,      CommandId::ViewLockFolder,
        CommandId::ViewLockHierarchy, CommandId::ViewLockGlobal,  CommandId::TreeExpand,
        CommandId::TreeCollapse,      CommandId::TreeCollapseAll,
    };
    (void)ids;
}
