// list_volumes() / volume_for() and the pipeline of <navigator/volume.hpp> (steps 2-4, rules R,
// L, K). Only the injected IMountSource, sysfs root and devfs root are consulted.
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

#include <navigator/volume.hpp>

#include "volume_internal.hpp"

namespace navigator {

namespace platform_internal {

namespace {

namespace stdfs = std::filesystem;

std::string_view strip_trailing_slash(std::string_view path) noexcept {
    while (path.size() > 1 && path.back() == '/') {
        path.remove_suffix(1);
    }
    return path;
}

// Everything the per-entry rules need, computed once per pipeline run.
struct RuleContext {
    stdfs::path sysfs_root;
    std::string sysfs_canonical; // canonical sysfs root, to inspect only the part below it
    std::map<std::string, std::string> labels; // device basename -> decoded label
    std::map<std::string, std::string> uuids;  // device basename -> uuid
    std::string home;                          // normalized home dir, may be empty
};

std::string device_name(std::string_view source) {
    constexpr std::string_view kDev = "/dev/";
    if (source.substr(0, kDev.size()) != kDev) {
        return {};
    }
    const std::size_t slash = source.rfind('/');
    std::string name(source.substr(slash + 1));
    if (name == "." || name == "..") {
        return {};
    }
    return name;
}

bool is_device_mapper(std::string_view source, std::string_view name) noexcept {
    return source.substr(0, 12) == "/dev/mapper/" || name.substr(0, 3) == "dm-";
}

std::string read_small_file(const stdfs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    std::string text(64, '\0');
    in.read(text.data(), static_cast<std::streamsize>(text.size()));
    text.resize(static_cast<std::size_t>(in.gcount()));
    return text;
}

std::string_view trim(std::string_view text) noexcept {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\n' || text.front() == '\t' ||
                             text.front() == '\r')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\n' || text.back() == '\t' ||
                             text.back() == '\r')) {
        text.remove_suffix(1);
    }
    return text;
}

// A path component that starts with "usb" (e.g. ".../0000:00:14.0/usb2/2-1/...").
bool has_usb_component(std::string_view path) noexcept {
    return path.find("/usb") != std::string_view::npos;
}

// Rule R1: nullopt when the device does not resolve to a sysfs block device.
std::optional<bool> removable_by_sysfs(const RuleContext& ctx, const std::string& name) {
    std::error_code ec;
    const stdfs::path block = ctx.sysfs_root / "block";
    stdfs::path disk;
    if (stdfs::exists(block / name, ec)) {
        disk = block / name;
    } else {
        stdfs::directory_iterator it(block, ec);
        const stdfs::directory_iterator end;
        for (; !ec && it != end; it.increment(ec)) {
            std::error_code inner;
            if (stdfs::exists(it->path() / name, inner)) {
                disk = it->path();
                break;
            }
        }
    }
    if (disk.empty()) {
        return std::nullopt;
    }
    if (trim(read_small_file(disk / "removable")) == "1") {
        return true;
    }
    const stdfs::path canonical = stdfs::canonical(disk, ec);
    if (ec) {
        return false;
    }
    std::string_view rest = canonical.native();
    const std::string_view root = ctx.sysfs_canonical;
    if (!root.empty() && root != "/" && rest.substr(0, root.size()) == root &&
        (rest.size() == root.size() || rest[root.size()] == '/')) {
        rest.remove_prefix(root.size());
    }
    return has_usb_component(rest);
}

// Rule R (R1-R3). A device R1 resolves is decided by R1 alone; R3 only covers the rest.
bool is_removable(const RuleContext& ctx, const MountEntry& entry) {
    const std::string name = device_name(entry.source);
    if (!name.empty() && !is_device_mapper(entry.source, name)) {
        if (const std::optional<bool> by_sysfs = removable_by_sysfs(ctx, name)) {
            return *by_sysfs;
        }
    }
    const std::string_view mp = entry.mount_point.native();
    return (path_at_or_below(mp, "/media") && strip_trailing_slash(mp) != "/media") ||
           (path_at_or_below(mp, "/run/media") && strip_trailing_slash(mp) != "/run/media");
}

void scan_links(const stdfs::path& dir, bool decode, std::map<std::string, std::string>& out) {
    std::error_code ec;
    stdfs::directory_iterator it(dir, ec);
    const stdfs::directory_iterator end;
    for (; !ec && it != end; it.increment(ec)) {
        std::error_code inner;
        if (!it->is_symlink(inner) || inner) {
            continue;
        }
        const stdfs::path target = stdfs::read_symlink(it->path(), inner);
        if (inner) {
            continue;
        }
        const std::string device = target.filename().native();
        const std::string raw = it->path().filename().native();
        std::string value = decode ? decode_udev_escapes(raw) : raw;
        auto found = out.find(device);
        // Several links to one device: pick the smallest name, independent of readdir order.
        if (found == out.end()) {
            out.emplace(device, std::move(value));
        } else if (value < found->second) {
            found->second = std::move(value);
        }
    }
}

std::string resolve_home(const Path& configured) {
    std::string home = configured.native();
    if (home.empty()) {
        if (const char* env = std::getenv("HOME"); env != nullptr && *env != '\0') {
            home = env;
        } else {
            long size = ::sysconf(_SC_GETPW_R_SIZE_MAX);
            std::vector<char> buf(size > 0 ? static_cast<std::size_t>(size) : 16384U);
            passwd pw{};
            passwd* result = nullptr;
            if (::getpwuid_r(::getuid(), &pw, buf.data(), buf.size(), &result) == 0 &&
                result != nullptr && result->pw_dir != nullptr) {
                home = result->pw_dir;
            }
        }
    }
    if (home.empty()) {
        return home;
    }
    std::string normal = stdfs::path(home).lexically_normal().native();
    return std::string(strip_trailing_slash(normal));
}

RuleContext make_context(const VolumeQuery& query) {
    RuleContext ctx;
    ctx.sysfs_root = query.sysfs_root;
    std::error_code ec;
    const stdfs::path canonical = stdfs::canonical(query.sysfs_root, ec);
    if (!ec) {
        ctx.sysfs_canonical = canonical.native();
    }
    scan_links(query.devfs_root / "disk" / "by-label", true, ctx.labels);
    scan_links(query.devfs_root / "disk" / "by-uuid", false, ctx.uuids);
    ctx.home = resolve_home(query.home_dir);
    return ctx;
}

VolumeKind classify(const RuleContext& ctx, const MountEntry& entry, bool removable) {
    const std::string_view mp = strip_trailing_slash(entry.mount_point.native());
    if (is_pseudo_filesystem(entry.fs_type)) {
        return VolumeKind::Virtual;
    }
    if (is_network_filesystem(entry.fs_type)) {
        return VolumeKind::Network;
    }
    if (mp == "/") {
        return VolumeKind::Root;
    }
    if (!ctx.home.empty() && mp == ctx.home) {
        return VolumeKind::Home;
    }
    if (removable) {
        return VolumeKind::Removable;
    }
    if (entry.source.starts_with("/dev/") || entry.fs_type == "zfs" ||
        entry.fs_type == "bcachefs") {
        return VolumeKind::Internal;
    }
    return VolumeKind::Other;
}

VolumeInfo make_volume(const RuleContext& ctx, const MountEntry& entry, bool query_space,
                       IMountSource& source) {
    VolumeInfo info;
    info.mount_point = entry.mount_point;
    info.device = entry.source;
    info.fs_type = entry.fs_type;
    info.root = entry.root;
    info.mount_id = entry.mount_id;
    info.device_number = entry.device_number;
    info.read_only = entry.read_only;
    info.removable = is_removable(ctx, entry);
    if (const std::string name = device_name(entry.source); !name.empty()) {
        if (auto it = ctx.labels.find(name); it != ctx.labels.end()) {
            info.label = it->second;
        }
        if (auto it = ctx.uuids.find(name); it != ctx.uuids.end()) {
            info.uuid = it->second;
        }
    }
    info.kind = classify(ctx, entry, info.removable);
    if (query_space) {
        if (Result<SpaceInfo> space = source.query_space(entry.mount_point); space) {
            info.space = *space;
            info.has_space = true;
        }
    }
    return info;
}

struct PairKey {
    std::uint64_t device = 0;
    std::string root;
    friend bool operator<(const PairKey& a, const PairKey& b) {
        return a.device != b.device ? a.device < b.device : a.root < b.root;
    }
};

Error exception_error(const std::exception& e) {
    if (dynamic_cast<const std::bad_alloc*>(&e) != nullptr) {
        return Error{ErrorCode::OutOfMemory, "volume: allocation failed"};
    }
    return Error{ErrorCode::Unknown, std::string("volume: ") + e.what()};
}

} // namespace

bool path_at_or_below(std::string_view path, std::string_view base) noexcept {
    path = strip_trailing_slash(path);
    base = strip_trailing_slash(base);
    if (base == "/") {
        return !path.empty() && path.front() == '/';
    }
    if (path.size() < base.size() || path.substr(0, base.size()) != base) {
        return false;
    }
    return path.size() == base.size() || path[base.size()] == '/';
}

std::vector<MountEntry> collapse_mounts(std::vector<MountEntry> entries) {
    // Stacked: the last entry per mount_point is the visible one.
    std::unordered_map<std::string, std::size_t> last;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        last[std::string(strip_trailing_slash(entries[i].mount_point.native()))] = i;
    }
    std::vector<std::size_t> visible;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (last[std::string(strip_trailing_slash(entries[i].mount_point.native()))] == i) {
            visible.push_back(i);
        }
    }
    // Bind duplicates: shortest mount_point per (device_number, root); ties keep the first.
    std::map<PairKey, std::size_t> best;
    for (const std::size_t i : visible) {
        PairKey key{entries[i].device_number, entries[i].root};
        auto it = best.find(key);
        if (it == best.end()) {
            best.emplace(std::move(key), i);
        } else if (entries[i].mount_point.native().size() <
                   entries[it->second].mount_point.native().size()) {
            it->second = i;
        }
    }
    std::vector<MountEntry> kept;
    kept.reserve(best.size());
    for (const std::size_t i : visible) {
        if (best.at(PairKey{entries[i].device_number, entries[i].root}) == i) {
            kept.push_back(std::move(entries[i]));
        }
    }
    return kept;
}

bool is_hidden_mount(const MountEntry& entry) noexcept {
    const std::string_view mp = strip_trailing_slash(entry.mount_point.native());
    if (mp == "/") {
        return false;
    }
    if (is_pseudo_filesystem(entry.fs_type)) {
        return true;
    }
    if (path_at_or_below(mp, "/run/media")) {
        return false;
    }
    static constexpr std::string_view kSystem[] = {
        "/proc", "/sys", "/dev", "/run", "/snap", "/boot", "/var/lib", "/var/snap",
    };
    for (const std::string_view base : kSystem) {
        if (path_at_or_below(mp, base)) {
            return true;
        }
    }
    // Single-file bind mounts (e.g. Waydroid's waydroid.prop). Network mount points are not
    // stat()ed: a hung server must not block the filter.
    if (!is_network_filesystem(entry.fs_type)) {
        struct stat st {};
        if (::stat(entry.mount_point.c_str(), &st) == 0 && !S_ISDIR(st.st_mode)) {
            return true;
        }
    }
    return false;
}

Result<std::shared_ptr<IMountSource>> resolve_source(const VolumeQuery& query) {
    if (query.source) {
        return query.source;
    }
    return create_proc_mount_source();
}

Result<std::vector<VolumeInfo>> build_volumes(std::string_view mountinfo_text,
                                              const VolumeQuery& query, IMountSource& source) {
    try {
        std::vector<MountEntry> entries = collapse_mounts(parse_mountinfo(mountinfo_text));
        const RuleContext ctx = make_context(query);
        std::vector<VolumeInfo> volumes;
        volumes.reserve(entries.size());
        for (const MountEntry& entry : entries) {
            if (!query.include_hidden && is_hidden_mount(entry)) {
                continue;
            }
            volumes.push_back(make_volume(ctx, entry, query.query_space, source));
        }
        return volumes;
    } catch (const std::exception& e) {
        return Unexpected<Error>(exception_error(e));
    }
}

} // namespace platform_internal

Result<std::vector<VolumeInfo>> list_volumes(const VolumeQuery& query) {
    Result<std::shared_ptr<IMountSource>> source = platform_internal::resolve_source(query);
    if (!source) {
        return Unexpected<Error>(source.error());
    }
    Result<std::string> text = (*source)->read_mountinfo();
    if (!text) {
        return Unexpected<Error>(text.error());
    }
    return platform_internal::build_volumes(*text, query, **source);
}

Result<VolumeInfo> volume_for(const Path& path, const VolumeQuery& query) {
    if (path.empty()) {
        return fail(ErrorCode::InvalidArgument, "volume_for: empty path");
    }
    try {
        Result<std::shared_ptr<IMountSource>> source = platform_internal::resolve_source(query);
        if (!source) {
            return Unexpected<Error>(source.error());
        }
        Result<std::string> text = (*source)->read_mountinfo();
        if (!text) {
            return Unexpected<Error>(text.error());
        }
        namespace stdfs = std::filesystem;
        std::error_code ec;
        stdfs::path abs_path = path.is_absolute() ? path : stdfs::absolute(path, ec);
        if (ec) {
            return fail(ErrorCode::IoError, "volume_for: cannot make '" + path.native() +
                                                "' absolute: " + ec.message());
        }
        stdfs::path target = stdfs::weakly_canonical(abs_path, ec);
        if (ec) {
            target = abs_path.lexically_normal();
        }
        const std::string_view wanted = target.native();

        std::vector<MountEntry> entries =
            platform_internal::collapse_mounts(parse_mountinfo(*text));
        const MountEntry* best = nullptr;
        std::size_t best_len = 0;
        for (const MountEntry& entry : entries) {
            const std::string_view mp =
                platform_internal::strip_trailing_slash(entry.mount_point.native());
            if (!platform_internal::path_at_or_below(wanted, mp)) {
                continue;
            }
            if (best == nullptr || mp.size() > best_len) {
                best = &entry;
                best_len = mp.size();
            }
        }
        if (best == nullptr) {
            return fail(ErrorCode::NotFound,
                        "volume_for: no mount contains '" + target.native() + "'");
        }
        const platform_internal::RuleContext ctx = platform_internal::make_context(query);
        return platform_internal::make_volume(ctx, *best, query.query_space, **source);
    } catch (const std::exception& e) {
        return Unexpected<Error>(platform_internal::exception_error(e));
    }
}

} // namespace navigator
