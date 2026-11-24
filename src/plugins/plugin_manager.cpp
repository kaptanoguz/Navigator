// C ABI plugin loader and registry (navigator/plugins.hpp, navigator/plugin.h).
// In-process dlopen() in v1 with strict abi_version and struct_size validation.
// Plugins are never invoked during filesystem write operations.
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <dlfcn.h>

#include <navigator/plugin.h>
#include <navigator/plugins.hpp>

namespace navigator::plugins {

namespace {

// Minimum struct_size required for the base header fields (abi_version..version).
constexpr std::size_t kMinInfoStructSize =
    offsetof(NavigatorPluginInfo, version) + sizeof(const char*);

bool has_field(std::uint32_t struct_size, std::size_t field_end) noexcept {
    return static_cast<std::size_t>(struct_size) >= field_end;
}

std::string safe_cstr(const char* s) { return s != nullptr ? std::string(s) : std::string{}; }

} // namespace

struct LoadedPluginRecord {
    void* handle = nullptr;
    const NavigatorPluginInfo* info = nullptr;
    LoadedPluginInfo meta;

    struct CommandEntry {
        PluginCommandDesc desc;
        NavigatorPluginCommandFn callback = nullptr;
        void* user_data = nullptr;
    };
    struct ColumnEntry {
        PluginColumnDesc desc;
        NavigatorPluginColumnFn value_fn = nullptr;
        void* user_data = nullptr;
    };

    std::vector<CommandEntry> commands;
    std::vector<ColumnEntry> columns;
    NavigatorPluginOverlayFn overlay_fn = nullptr;
    void* overlay_user_data = nullptr;
    NavigatorPluginPreviewFn preview_fn = nullptr;
    void* preview_user_data = nullptr;
    NavigatorPluginShutdownFn shutdown_fn = nullptr;
    void* shutdown_user_data = nullptr;
};

struct PluginManager::Impl {
    std::vector<LoadedPluginRecord> loaded;

    ~Impl() { unload_all(); }

    void unload_all() noexcept {
        for (auto it = loaded.rbegin(); it != loaded.rend(); ++it) {
            if (it->shutdown_fn != nullptr) {
                it->shutdown_fn(it->shutdown_user_data);
            }
            if (it->handle != nullptr) {
                ::dlclose(it->handle);
                it->handle = nullptr;
            }
        }
        loaded.clear();
    }
};

PluginManager::PluginManager() : impl_(std::make_unique<Impl>()) {}
PluginManager::~PluginManager() = default;
PluginManager::PluginManager(PluginManager&&) noexcept = default;
PluginManager& PluginManager::operator=(PluginManager&&) noexcept = default;

Result<LoadedPluginInfo> PluginManager::load_file(const Path& so_path) {
    try {
        if (!impl_) {
            return fail(ErrorCode::InvalidArgument, "plugins: uninitialized manager");
        }
        if (so_path.empty()) {
            return fail(ErrorCode::InvalidArgument, "plugins: empty library path");
        }
        std::error_code ec;
        if (!std::filesystem::exists(so_path, ec)) {
            return fail(ErrorCode::NotFound, "plugins: file not found: " + so_path.native());
        }

        void* handle = ::dlopen(so_path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (handle == nullptr) {
            const char* err = ::dlerror();
            return fail(ErrorCode::PlatformError, std::string("plugins: dlopen failed: ") +
                                                      (err != nullptr ? err : "unknown error"));
        }

        // Clear any previous dlerror, then resolve entry symbol.
        (void)::dlerror();
        void* sym = ::dlsym(handle, NAVIGATOR_PLUGIN_ENTRY_SYMBOL);
        if (sym == nullptr) {
            ::dlclose(handle);
            return fail(ErrorCode::InvalidArgument,
                        "plugins: missing symbol " NAVIGATOR_PLUGIN_ENTRY_SYMBOL);
        }

        auto get_info = reinterpret_cast<NavigatorPluginGetInfoFn>(sym);
        const NavigatorPluginInfo* info = get_info();
        if (info == nullptr) {
            ::dlclose(handle);
            return fail(ErrorCode::InvalidArgument,
                        "plugins: entry point returned NULL (refused to load)");
        }
        if (info->abi_version != NAVIGATOR_PLUGIN_ABI_VERSION) {
            ::dlclose(handle);
            return fail(ErrorCode::Unsupported, "plugins: ABI version mismatch (expected " +
                                                    std::to_string(NAVIGATOR_PLUGIN_ABI_VERSION) +
                                                    ", got " + std::to_string(info->abi_version) +
                                                    ")");
        }
        if (info->struct_size < kMinInfoStructSize) {
            ::dlclose(handle);
            return fail(ErrorCode::Unsupported,
                        "plugins: NavigatorPluginInfo struct_size too small");
        }
        if (info->id == nullptr || info->id[0] == '\0' || info->name == nullptr ||
            info->name[0] == '\0') {
            ::dlclose(handle);
            return fail(ErrorCode::InvalidArgument, "plugins: id and name must be non-empty");
        }

        const std::string plugin_id(info->id);
        for (const auto& existing : impl_->loaded) {
            if (existing.meta.id == plugin_id) {
                ::dlclose(handle);
                return fail(ErrorCode::AlreadyExists,
                            "plugins: duplicate plugin id '" + plugin_id + "'");
            }
        }

        LoadedPluginRecord rec;
        rec.handle = handle;
        rec.info = info;
        rec.meta.id = plugin_id;
        rec.meta.name = safe_cstr(info->name);
        rec.meta.version = safe_cstr(info->version);
        rec.meta.library_path = so_path;

        if (has_field(info->struct_size,
                      offsetof(NavigatorPluginInfo, description) + sizeof(info->description))) {
            rec.meta.description = safe_cstr(info->description);
        }
        if (has_field(info->struct_size,
                      offsetof(NavigatorPluginInfo, command_count) + sizeof(info->command_count)) &&
            info->commands != nullptr && info->command_count > 0) {
            for (std::uint32_t i = 0; i < info->command_count; ++i) {
                const NavigatorPluginCommand& c = info->commands[i];
                if (c.struct_size >= sizeof(NavigatorPluginCommand) && c.id != nullptr &&
                    c.id[0] != '\0' && c.title != nullptr && c.callback != nullptr) {
                    LoadedPluginRecord::CommandEntry ce;
                    ce.desc.plugin_id = plugin_id;
                    ce.desc.id = c.id;
                    ce.desc.title = c.title;
                    ce.desc.description = safe_cstr(c.description);
                    ce.callback = c.callback;
                    ce.user_data = c.user_data;
                    rec.commands.push_back(std::move(ce));
                }
            }
            rec.meta.command_count = static_cast<std::uint32_t>(rec.commands.size());
        }
        if (has_field(info->struct_size, offsetof(NavigatorPluginInfo, overlay_user_data) +
                                             sizeof(info->overlay_user_data))) {
            rec.overlay_fn = info->overlay_fn;
            rec.overlay_user_data = info->overlay_user_data;
            rec.meta.has_overlay = (rec.overlay_fn != nullptr);
        }
        if (has_field(info->struct_size,
                      offsetof(NavigatorPluginInfo, column_count) + sizeof(info->column_count)) &&
            info->columns != nullptr && info->column_count > 0) {
            for (std::uint32_t i = 0; i < info->column_count; ++i) {
                const NavigatorPluginColumn& col = info->columns[i];
                if (col.struct_size >= sizeof(NavigatorPluginColumn) && col.id != nullptr &&
                    col.id[0] != '\0' && col.title != nullptr && col.value_fn != nullptr) {
                    LoadedPluginRecord::ColumnEntry ce;
                    ce.desc.plugin_id = plugin_id;
                    ce.desc.id = col.id;
                    ce.desc.title = col.title;
                    ce.desc.width_dp = col.width_dp > 0 ? col.width_dp : 120U;
                    ce.value_fn = col.value_fn;
                    ce.user_data = col.user_data;
                    rec.columns.push_back(std::move(ce));
                }
            }
            rec.meta.column_count = static_cast<std::uint32_t>(rec.columns.size());
        }
        if (has_field(info->struct_size, offsetof(NavigatorPluginInfo, preview_user_data) +
                                             sizeof(info->preview_user_data))) {
            rec.preview_fn = info->preview_fn;
            rec.preview_user_data = info->preview_user_data;
            rec.meta.has_preview = (rec.preview_fn != nullptr);
        }
        if (has_field(info->struct_size, offsetof(NavigatorPluginInfo, shutdown_user_data) +
                                             sizeof(info->shutdown_user_data))) {
            rec.shutdown_fn = info->shutdown_fn;
            rec.shutdown_user_data = info->shutdown_user_data;
        }

        LoadedPluginInfo meta_copy = rec.meta;
        impl_->loaded.push_back(std::move(rec));
        return meta_copy;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "plugins: out of memory");
    }
}

std::size_t PluginManager::load_directory(const Path& dir, std::vector<std::string>* errors_out) {
    if (!impl_ || dir.empty()) {
        return 0;
    }
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) {
        return 0;
    }
    std::vector<Path> files;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (entry.is_regular_file(ec) && entry.path().extension() == ".so") {
            files.push_back(entry.path());
        }
    }
    std::ranges::sort(files);
    std::size_t count = 0;
    for (const Path& so : files) {
        auto res = load_file(so);
        if (res) {
            ++count;
        } else if (errors_out != nullptr) {
            errors_out->push_back(so.filename().native() + ": " + res.error().message);
        }
    }
    return count;
}

void PluginManager::unload_all() noexcept {
    if (impl_) {
        impl_->unload_all();
    }
}

std::vector<LoadedPluginInfo> PluginManager::plugins() const {
    std::vector<LoadedPluginInfo> out;
    if (!impl_) {
        return out;
    }
    out.reserve(impl_->loaded.size());
    for (const auto& rec : impl_->loaded) {
        out.push_back(rec.meta);
    }
    return out;
}

std::vector<PluginCommandDesc> PluginManager::commands() const {
    std::vector<PluginCommandDesc> out;
    if (!impl_) {
        return out;
    }
    for (const auto& rec : impl_->loaded) {
        for (const auto& cmd : rec.commands) {
            out.push_back(cmd.desc);
        }
    }
    return out;
}

std::vector<PluginColumnDesc> PluginManager::columns() const {
    std::vector<PluginColumnDesc> out;
    if (!impl_) {
        return out;
    }
    for (const auto& rec : impl_->loaded) {
        for (const auto& col : rec.columns) {
            out.push_back(col.desc);
        }
    }
    return out;
}

Result<void> PluginManager::invoke_command(std::string_view command_id, const Path& current_dir,
                                           const std::vector<Path>& selected_paths) const {
    if (!impl_) {
        return fail(ErrorCode::NotFound, "plugins: uninitialized manager");
    }
    for (const auto& rec : impl_->loaded) {
        for (const auto& cmd : rec.commands) {
            if (cmd.desc.id == command_id) {
                std::vector<const char*> ptrs;
                ptrs.reserve(selected_paths.size());
                for (const Path& p : selected_paths) {
                    ptrs.push_back(p.c_str());
                }
                const int rc =
                    cmd.callback(current_dir.c_str(), ptrs.empty() ? nullptr : ptrs.data(),
                                 static_cast<std::uint32_t>(ptrs.size()), cmd.user_data);
                if (rc != 0) {
                    return fail(ErrorCode::PlatformError,
                                "plugins: command '" + std::string(command_id) +
                                    "' returned error code " + std::to_string(rc));
                }
                return ok();
            }
        }
    }
    return fail(ErrorCode::NotFound, "plugins: unknown command '" + std::string(command_id) + "'");
}

std::optional<PluginOverlayResult> PluginManager::query_overlay(const Path& path) const {
    if (!impl_ || path.empty()) {
        return std::nullopt;
    }
    for (const auto& rec : impl_->loaded) {
        if (rec.overlay_fn == nullptr) {
            continue;
        }
        NavigatorPluginOverlay raw{};
        if (rec.overlay_fn(path.c_str(), &raw, rec.overlay_user_data) != 0) {
            raw.badge_text[sizeof(raw.badge_text) - 1] = '\0';
            raw.tooltip[sizeof(raw.tooltip) - 1] = '\0';
            PluginOverlayResult res;
            res.badge_text = raw.badge_text;
            res.color_rgba = raw.color_rgba;
            res.tooltip = raw.tooltip;
            return res;
        }
    }
    return std::nullopt;
}

std::optional<std::string> PluginManager::query_column(std::string_view column_id,
                                                       const Path& path) const {
    if (!impl_ || path.empty()) {
        return std::nullopt;
    }
    for (const auto& rec : impl_->loaded) {
        for (const auto& col : rec.columns) {
            if (col.desc.id == column_id) {
                std::array<char, 256> buf{};
                if (col.value_fn(path.c_str(), buf.data(), static_cast<std::uint32_t>(buf.size()),
                                 col.user_data) != 0) {
                    buf.back() = '\0';
                    return std::string(buf.data());
                }
                return std::nullopt;
            }
        }
    }
    return std::nullopt;
}

std::optional<std::string> PluginManager::query_preview(const Path& path) const {
    if (!impl_ || path.empty()) {
        return std::nullopt;
    }
    for (const auto& rec : impl_->loaded) {
        if (rec.preview_fn == nullptr) {
            continue;
        }
        std::array<char, 4096> buf{};
        const int n = rec.preview_fn(path.c_str(), buf.data(),
                                     static_cast<std::uint32_t>(buf.size()), rec.preview_user_data);
        if (n > 0) {
            const auto len = std::min(static_cast<std::size_t>(n), buf.size() - 1);
            buf[len] = '\0';
            return std::string(buf.data(), len);
        }
    }
    return std::nullopt;
}

} // namespace navigator::plugins
