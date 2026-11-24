// navigator/plugins.hpp
//
// Purpose: C++ host-side loader and registry for Navigator C ABI plugins (plugin.h).
//          Loads .so plugins via dlopen/dlsym, validates abi_version and struct_size, and exposes
//          plugin commands, file overlays, custom columns, and text previews to the app and UI.
// Owner:   contracts (header); implementation in src/plugins (role: plugins).
// Thread-safety: load_file(), load_directory(), unload_all(), and invoke_command() must be called
//                on the UI thread. Read-only queries (plugins(), commands(), columns(),
//                query_overlay(), query_column(), query_preview()) are safe while no load/unload
//                is running.
// Errors:  navigator::Result (types.hpp). No exceptions escape these functions.
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <navigator/types.hpp>

namespace navigator::plugins {

struct PluginCommandDesc {
    std::string plugin_id;
    std::string id;
    std::string title;
    std::string description;
};

struct PluginOverlayResult {
    std::string badge_text;
    std::uint32_t color_rgba = 0; // 0xRRGGBBAA; 0 = theme accent
    std::string tooltip;
};

struct PluginColumnDesc {
    std::string plugin_id;
    std::string id;
    std::string title;
    std::uint32_t width_dp = 120;
};

struct LoadedPluginInfo {
    std::string id;
    std::string name;
    std::string version;
    std::string description;
    Path library_path;
    std::uint32_t command_count = 0;
    std::uint32_t column_count = 0;
    bool has_overlay = false;
    bool has_preview = false;
};

// Manages the lifecycle of loaded C ABI plugins.
class PluginManager {
public:
    PluginManager();
    ~PluginManager();
    PluginManager(const PluginManager&) = delete;
    PluginManager& operator=(const PluginManager&) = delete;
    PluginManager(PluginManager&&) noexcept;
    PluginManager& operator=(PluginManager&&) noexcept;

    // Loads a single shared library (.so). Fails with:
    //   - NotFound if the file cannot be opened
    //   - InvalidArgument if the entry symbol is missing, returns NULL, has empty id/name, or
    //     duplicates an already-loaded plugin id
    //   - Unsupported if abi_version != NAVIGATOR_PLUGIN_ABI_VERSION or struct_size is too small
    [[nodiscard]] Result<LoadedPluginInfo> load_file(const Path& so_path);

    // Scans `dir` (non-recursively) for `*.so` files and loads each. A missing directory is not an
    // error (returns 0 loaded). Individual plugin load failures are recorded in `errors_out` (if
    // non-null) and do not abort loading the remaining plugins.
    [[nodiscard]] std::size_t load_directory(const Path& dir,
                                             std::vector<std::string>* errors_out = nullptr);

    // Calls shutdown_fn (if any) and dlclose() on every loaded plugin in reverse load order.
    void unload_all() noexcept;

    [[nodiscard]] std::vector<LoadedPluginInfo> plugins() const;
    [[nodiscard]] std::vector<PluginCommandDesc> commands() const;
    [[nodiscard]] std::vector<PluginColumnDesc> columns() const;

    // Invokes a registered plugin command by `command_id`.
    // Errors: NotFound (unknown command), PlatformError (plugin returned non-zero).
    [[nodiscard]] Result<void> invoke_command(std::string_view command_id, const Path& current_dir,
                                              const std::vector<Path>& selected_paths) const;

    // Queries plugins in load order; returns the first overlay produced for `path`.
    [[nodiscard]] std::optional<PluginOverlayResult> query_overlay(const Path& path) const;

    // Queries a specific plugin column by `column_id` for `path`.
    [[nodiscard]] std::optional<std::string> query_column(std::string_view column_id,
                                                          const Path& path) const;

    // Queries plugins in load order; returns the first UTF-8 text preview produced for `path`.
    [[nodiscard]] std::optional<std::string> query_preview(const Path& path) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace navigator::plugins
