// Compile-only contract usage check for Phase 10 public headers.
#include <navigator/archive.hpp>
#include <navigator/commands.hpp>
#include <navigator/content_search.hpp>
#include <navigator/git.hpp>
#include <navigator/platform.hpp>
#include <navigator/plugin.h>
#include <navigator/plugins.hpp>
#include <navigator/ui.hpp>
#include <navigator/view_settings.hpp>

namespace {

[[maybe_unused]] void check_phase10_contracts() {
    using namespace navigator;

    // ViewMode::Columns + Phase 10 commands
    const view::ViewMode col_mode = view::ViewMode::Columns;
    (void)col_mode;
    const CommandId cmds[] = {
        CommandId::OpenTerminalHere,   CommandId::ViewModeColumns, CommandId::ContentSearch,
        CommandId::ArchiveExtractHere, CommandId::ReloadPlugins,   CommandId::UnmountVolume,
    };
    (void)cmds;

    // AppConfig Phase 10 fields
    AppConfig cfg;
    cfg.ipc_socket_path = "/tmp/nav.sock";
    cfg.plugin_dir = "/tmp/plugins";
    cfg.select_path = "/tmp/a.txt";
    cfg.enable_ipc = true;
    cfg.enable_plugins = true;
    cfg.enable_git = true;
    cfg.enable_gvfs = true;

    // Plugin C ABI + C++ PluginManager
    NavigatorPluginInfo pinfo{};
    pinfo.abi_version = NAVIGATOR_PLUGIN_ABI_VERSION;
    pinfo.struct_size = sizeof(NavigatorPluginInfo);
    (void)pinfo;
    plugins::PluginManager pm;
    (void)pm.plugins();

    // Archive helpers
    (void)archive::libarchive_available();
    (void)archive::is_archive_name("test.zip");
    (void)archive::parse_archive_path("/tmp/a.zip::dir");
    (void)archive::make_archive_path("/tmp/a.zip", "dir");

    // Content search
    search::ContentSearchOptions opts;
    opts.root = "/tmp";
    opts.query = "needle";
    search::ContentSearchSession session;
    (void)session.active();

    // Git status
    (void)git::git_state_badge(git::GitFileState::Modified);
    (void)git::merge_dir_state(git::GitFileState::Added, git::GitFileState::Conflict);

    // Platform GVfs & IPC
    (void)platform::gvfs_available();
    (void)platform::is_gvfs_uri("smb://server/share");
    platform::IpcMessage msg{platform::IpcAction::NewTab, "/tmp"};
    (void)msg;
}

} // namespace
