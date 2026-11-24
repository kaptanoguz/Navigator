// Phase 8 of <navigator/openwith.hpp>, part 2: shared-mime-info aliases / subclasses, MIME
// Applications associations (mimeapps.list, defaults.list, mimeinfo.cache), handler listing and
// set_default_handler(). No GIO.
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <new>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <navigator/openwith.hpp>
#include <navigator/platform.hpp>

#include "errno_util.hpp"
#include "openwith_internal.hpp"

namespace navigator::openwith {

namespace {

using namespace openwith_internal;
namespace stdfs = std::filesystem;

constexpr std::string_view kDefaultGroup = "Default Applications";
constexpr std::string_view kAddedGroup = "Added Associations";
constexpr std::string_view kRemovedGroup = "Removed Associations";
constexpr std::string_view kCacheGroup = "MIME Cache";
constexpr std::string_view kOctetStream = "application/octet-stream";

using IdLists = std::unordered_map<std::string, std::vector<std::string>>;

// ---------------------------------------------------------------------------------------------
// shared-mime-info
// ---------------------------------------------------------------------------------------------

std::vector<Path> mime_roots(const Options& options) {
    std::vector<Path> roots;
    if (!options.data_home.empty()) {
        roots.push_back(options.data_home / "mime");
    }
    for (const Path& dir : options.data_dirs) {
        roots.push_back(dir / "mime");
    }
    return roots;
}

struct MimeDb {
    std::unordered_map<std::string, std::string> aliases; // alias -> canonical (first wins)
    std::unordered_map<std::string, std::vector<std::string>> parents; // child -> parents
};

// "a b" pairs of one shared-mime-info text file.
template <class F> void for_each_pair(const Path& file, F&& f) {
    const auto text = read_text_file(file, kMaxMimeappsBytes);
    if (!text) {
        return;
    }
    for_each_line(*text, [&](std::string_view line) {
        line = trim(line);
        if (line.empty() || line.front() == '#') {
            return true;
        }
        const std::size_t space = line.find_first_of(" \t");
        if (space == std::string_view::npos) {
            return true;
        }
        const std::string_view first = line.substr(0, space);
        const std::string_view second = trim(line.substr(space + 1));
        if (valid_mime(first) && valid_mime(second)) {
            f(first, second);
        }
        return true;
    });
}

MimeDb load_mime_db(const Options& options) {
    MimeDb db;
    for (const Path& root : mime_roots(options)) {
        for_each_pair(root / "aliases", [&](std::string_view alias, std::string_view canonical) {
            db.aliases.emplace(std::string(alias), std::string(canonical));
        });
        for_each_pair(root / "subclasses", [&](std::string_view child, std::string_view parent) {
            auto& list = db.parents[std::string(child)];
            if (std::find(list.begin(), list.end(), parent) == list.end()) {
                list.emplace_back(parent);
            }
        });
    }
    return db;
}

std::string canonical_of(const MimeDb& db, const std::string& mime) {
    std::string current = mime;
    for (int hops = 0; hops < 4; ++hops) { // aliases of aliases do not occur; bound loops
        const auto it = db.aliases.find(current);
        if (it == db.aliases.end() || it->second == current) {
            break;
        }
        current = it->second;
    }
    return current;
}

// The types of one lookup: `primary` are M, its canonical name and the aliases of that canonical
// name; `parents` follow, nearest first.
struct TypeSet {
    std::string canonical;
    std::vector<std::string> primary;
    std::vector<std::string> parents;
};

bool ends_with_xml_suffix(std::string_view mime) {
    return mime.size() > 4 && mime.ends_with("+xml");
}

TypeSet resolve_types(std::string_view mime, const Options& options) {
    const MimeDb db = load_mime_db(options);
    TypeSet types;
    const std::string original(mime);
    const std::string canonical = canonical_of(db, original);
    types.canonical = canonical;
    types.primary.push_back(original);
    if (canonical != original) {
        types.primary.push_back(canonical);
    }
    std::vector<std::string> siblings; // other names of the same type, sorted for determinism
    for (const auto& [alias, target] : db.aliases) {
        if (target == canonical && alias != original && alias != canonical) {
            siblings.push_back(alias);
        }
    }
    std::sort(siblings.begin(), siblings.end());
    types.primary.insert(types.primary.end(), siblings.begin(), siblings.end());

    std::unordered_set<std::string> seen(types.primary.begin(), types.primary.end());
    std::vector<std::string> queue{canonical};
    if (canonical != original) {
        queue.push_back(original);
    }
    for (std::size_t head = 0; head < queue.size() && types.parents.size() < kMaxMimeParents;
         ++head) {
        const std::string node = queue[head];
        std::vector<std::string> next;
        if (const auto it = db.parents.find(node); it != db.parents.end()) {
            next = it->second;
        } else if (node != "text/plain" && node.starts_with("text/")) {
            next.emplace_back("text/plain");
        } else if (ends_with_xml_suffix(node)) {
            next.emplace_back("application/xml");
        }
        for (std::string& parent : next) {
            parent = canonical_of(db, parent);
            if (parent == kOctetStream || !seen.insert(parent).second) {
                continue;
            }
            if (types.parents.size() >= kMaxMimeParents) {
                break;
            }
            types.parents.push_back(parent);
            queue.push_back(parent);
        }
    }
    return types;
}

// ---------------------------------------------------------------------------------------------
// mimeapps.list, defaults.list, mimeinfo.cache
// ---------------------------------------------------------------------------------------------

struct MimeApps {
    IdLists defaults;
    IdLists added;
    IdLists removed;
};

MimeApps parse_mimeapps(std::string_view text, bool defaults_only) {
    MimeApps apps;
    IdLists* target = nullptr;
    for_each_line(text, [&](std::string_view line) {
        if (const auto header = group_header(line)) {
            if (*header == kDefaultGroup) {
                target = &apps.defaults;
            } else if (!defaults_only && *header == kAddedGroup) {
                target = &apps.added;
            } else if (!defaults_only && *header == kRemovedGroup) {
                target = &apps.removed;
            } else {
                target = nullptr;
            }
            return true;
        }
        if (target == nullptr) {
            return true;
        }
        if (const auto kv = key_value(line)) {
            if (valid_mime(kv->key)) {
                auto ids = split_semicolons(kv->value);
                auto& list = (*target)[std::string(kv->key)];
                list.insert(list.end(), std::make_move_iterator(ids.begin()),
                            std::make_move_iterator(ids.end()));
            }
        }
        return true;
    });
    return apps;
}

// Paths of the mimeapps.list files, highest precedence first.
std::vector<Path> mimeapps_paths(const Options& options) {
    std::vector<std::string> desktops;
    desktops.reserve(options.current_desktops.size());
    for (const std::string& desktop : options.current_desktops) {
        desktops.push_back(ascii_lower(desktop));
    }
    std::vector<Path> out;
    const auto add_dir = [&](const Path& dir) {
        if (dir.empty()) {
            return;
        }
        for (const std::string& desktop : desktops) {
            out.push_back(dir / (desktop + "-mimeapps.list"));
        }
        out.push_back(dir / "mimeapps.list");
    };
    add_dir(options.config_home);
    for (const Path& dir : options.config_dirs) {
        add_dir(dir);
    }
    if (!options.data_home.empty()) {
        add_dir(options.data_home / "applications");
    }
    for (const Path& dir : options.data_dirs) {
        add_dir(dir / "applications");
    }
    return out;
}

std::vector<Path> applications_dirs(const Options& options) {
    std::vector<Path> out;
    if (!options.data_home.empty()) {
        out.push_back(options.data_home / "applications");
    }
    for (const Path& dir : options.data_dirs) {
        out.push_back(dir / "applications");
    }
    return out;
}

// The lines of the [MIME Cache] group of one mimeinfo.cache for the wanted types.
IdLists parse_mimeinfo_cache(std::string_view text, const std::unordered_set<std::string>& wanted) {
    IdLists out;
    bool in_group = false;
    for_each_line(text, [&](std::string_view line) {
        if (const auto header = group_header(line)) {
            in_group = *header == kCacheGroup;
            return true;
        }
        if (!in_group) {
            return true;
        }
        if (const auto kv = key_value(line)) {
            const std::string key(kv->key);
            if (wanted.contains(key)) {
                auto ids = split_semicolons(kv->value);
                auto& list = out[key];
                list.insert(list.end(), std::make_move_iterator(ids.begin()),
                            std::make_move_iterator(ids.end()));
            }
        }
        return true;
    });
    return out;
}

// ---------------------------------------------------------------------------------------------
// Desktop id resolution
// ---------------------------------------------------------------------------------------------

bool exists_regular_file(const Path& path) {
    struct stat st {};
    return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

std::optional<Path> lookup_relative(const std::vector<Path>& dirs, std::string name,
                                    std::size_t from, int& budget) {
    if (--budget < 0) {
        return std::nullopt;
    }
    for (const Path& dir : dirs) {
        Path candidate = dir / name;
        if (exists_regular_file(candidate)) {
            return candidate;
        }
    }
    for (std::size_t i = from; i < name.size(); ++i) {
        if (name[i] != '-' || i == 0 || name[i - 1] == '/' || i + 1 == name.size()) {
            continue;
        }
        std::string replaced = name;
        replaced[i] = '/';
        if (auto found = lookup_relative(dirs, std::move(replaced), i + 1, budget)) {
            return found;
        }
    }
    return std::nullopt;
}

std::optional<Path> find_desktop_file(const Options& options, std::string_view id) {
    const std::vector<Path> dirs = applications_dirs(options);
    int budget = 64;
    auto found = lookup_relative(dirs, std::string(id), 0, budget);
    if (found && !found->is_absolute()) {
        std::error_code ec;
        Path absolute = stdfs::absolute(*found, ec);
        if (!ec) {
            return absolute;
        }
    }
    return found;
}

bool program_available(const Options& options, std::string_view program) {
    if (options.program_exists) {
        return options.program_exists(program);
    }
    return platform::find_program(program).has_value();
}

bool valid_desktop_id(std::string_view id) {
    return id.size() > 8 && id.ends_with(".desktop") && id.find('/') == std::string_view::npos &&
           id.find('\0') == std::string_view::npos;
}

// Resolves and filters (F1..F5); nullopt: not installed / not shown.
std::optional<Handler> resolve_handler(const Options& options, std::string_view id) {
    if (!valid_desktop_id(id)) {
        return std::nullopt;
    }
    const auto file = find_desktop_file(options, id);
    if (!file) {
        return std::nullopt;
    }
    const auto text = read_text_file(*file, kMaxDesktopFileBytes + 1);
    if (!text) {
        return std::nullopt;
    }
    auto parsed = parse_desktop_entry(*text, id, *file, options.locale);
    if (!parsed) {
        return std::nullopt;
    }
    Handler& h = *parsed;
    if (h.no_display && !options.include_no_display) {
        return std::nullopt;
    }
    if (!shown_in(h, options.current_desktops)) {
        return std::nullopt;
    }
    if (!h.try_exec.empty() && !program_available(options, h.try_exec)) {
        return std::nullopt;
    }
    const ExecCodes codes = scan_exec_codes(h.exec);
    if (!(codes.f || codes.F || codes.u || codes.U)) {
        return std::nullopt;
    }
    return std::move(*parsed);
}

class Resolver {
public:
    explicit Resolver(const Options& options) : options_(options) {}

    const std::optional<Handler>& get(const std::string& id) {
        const auto it = cache_.find(id);
        if (it != cache_.end()) {
            return it->second;
        }
        return cache_.emplace(id, resolve_handler(options_, id)).first->second;
    }

private:
    const Options& options_;
    std::unordered_map<std::string, std::optional<Handler>> cache_;
};

void append_unique(std::vector<std::string>& out, const std::string& id) {
    if (std::find(out.begin(), out.end(), id) == out.end()) {
        out.push_back(id);
    }
}

const std::vector<std::string>* find_ids(const IdLists& lists, const std::string& type) {
    const auto it = lists.find(type);
    return it == lists.end() ? nullptr : &it->second;
}

struct Sources {
    std::vector<MimeApps> mimeapps;       // precedence order
    std::vector<MimeApps> legacy;         // defaults.list files, lowest precedence
    std::vector<std::string> cache_texts; // mimeinfo.cache contents, directory order
};

Sources load_sources(const Options& options) {
    Sources sources;
    for (const Path& path : mimeapps_paths(options)) {
        if (const auto text = read_text_file(path, kMaxMimeappsBytes)) {
            sources.mimeapps.push_back(parse_mimeapps(*text, false));
        }
    }
    for (const Path& dir : applications_dirs(options)) {
        if (const auto text = read_text_file(dir / "defaults.list", kMaxMimeappsBytes)) {
            sources.legacy.push_back(parse_mimeapps(*text, true));
        }
        if (auto text = read_text_file(dir / "mimeinfo.cache", kMaxMimeinfoCacheBytes)) {
            sources.cache_texts.push_back(std::move(*text));
        }
    }
    return sources;
}

// Candidate ids for one group of equivalent type names. `default_id` is the first installed
// configured default (placed first).
std::vector<std::string> candidates_for(const Sources& sources, const std::vector<IdLists>& caches,
                                        const std::vector<std::string>& group, Resolver& resolver,
                                        std::string& default_id) {
    std::vector<std::string> ids;
    default_id.clear();
    const auto try_defaults = [&](const MimeApps& apps) {
        for (const std::string& type : group) {
            if (const auto* list = find_ids(apps.defaults, type)) {
                for (const std::string& id : *list) {
                    if (resolver.get(id)) {
                        default_id = id;
                        return true;
                    }
                }
            }
        }
        return false;
    };
    bool found = false;
    for (const MimeApps& apps : sources.mimeapps) {
        if (try_defaults(apps)) {
            found = true;
            break;
        }
    }
    for (const MimeApps& apps : sources.legacy) {
        if (found) {
            break;
        }
        found = try_defaults(apps);
    }
    if (found) {
        ids.push_back(default_id);
    }
    std::unordered_set<std::string> removed;
    for (const MimeApps& apps : sources.mimeapps) {
        for (const std::string& type : group) {
            if (const auto* list = find_ids(apps.added, type)) {
                for (const std::string& id : *list) {
                    if (!removed.contains(id)) {
                        append_unique(ids, id);
                    }
                }
            }
        }
        for (const std::string& type : group) {
            if (const auto* list = find_ids(apps.removed, type)) {
                removed.insert(list->begin(), list->end());
            }
        }
    }
    for (const IdLists& cache : caches) {
        for (const std::string& type : group) {
            if (const auto* list = find_ids(cache, type)) {
                for (const std::string& id : *list) {
                    if (!removed.contains(id)) {
                        append_unique(ids, id);
                    }
                }
            }
        }
    }
    return ids;
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------------------------

Result<std::vector<std::string>> mime_parents(std::string_view mime, const Options& options) {
    if (!valid_mime(mime)) {
        return fail(ErrorCode::InvalidArgument, "mime_parents: not a MIME type");
    }
    try {
        const TypeSet types = resolve_types(mime, options);
        std::vector<std::string> out;
        // The canonical name (when M is an alias) is the nearest "parent".
        if (types.canonical != mime) {
            out.push_back(types.canonical);
        }
        out.insert(out.end(), types.parents.begin(), types.parents.end());
        if (out.size() > kMaxMimeParents) {
            out.resize(kMaxMimeParents);
        }
        return out;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "mime_parents: out of memory");
    }
}

Result<std::vector<Handler>> list_handlers(std::string_view mime, const Options& options) {
    if (!valid_mime(mime)) {
        return fail(ErrorCode::InvalidArgument, "list_handlers: not a MIME type");
    }
    try {
        const TypeSet types = resolve_types(mime, options);
        const Sources sources = load_sources(options);
        std::unordered_set<std::string> wanted(types.primary.begin(), types.primary.end());
        wanted.insert(types.parents.begin(), types.parents.end());
        std::vector<IdLists> caches;
        caches.reserve(sources.cache_texts.size());
        for (const std::string& text : sources.cache_texts) {
            caches.push_back(parse_mimeinfo_cache(text, wanted));
        }
        Resolver resolver(options);
        std::vector<Handler> out;
        std::unordered_set<std::string> emitted;
        const auto emit = [&](const std::vector<std::string>& ids, const std::string& default_id,
                              bool primary) {
            for (const std::string& id : ids) {
                const auto& handler = resolver.get(id);
                if (!handler || !emitted.insert(handler->desktop_id).second) {
                    continue;
                }
                Handler copy = *handler;
                copy.via_parent_type = !primary;
                copy.is_default = primary && !default_id.empty() && id == default_id;
                out.push_back(std::move(copy));
            }
        };
        std::string default_id;
        emit(candidates_for(sources, caches, types.primary, resolver, default_id), default_id,
             true);
        for (const std::string& parent : types.parents) {
            const std::vector<std::string> group{parent};
            std::string ignored;
            emit(candidates_for(sources, caches, group, resolver, ignored), ignored, false);
        }
        return out;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "list_handlers: out of memory");
    }
}

Result<Handler> default_handler(std::string_view mime, const Options& options) {
    auto list = list_handlers(mime, options);
    if (!list) {
        return Unexpected<Error>(std::move(list.error()));
    }
    for (Handler& handler : *list) {
        if (handler.is_default) {
            return std::move(handler);
        }
    }
    for (Handler& handler : *list) {
        if (!handler.via_parent_type) {
            return std::move(handler);
        }
    }
    if (!list->empty()) {
        return std::move(list->front());
    }
    return fail(ErrorCode::NotFound, "default_handler: no application for " + std::string(mime));
}

Result<Handler> find_handler(std::string_view desktop_id, const Options& options) {
    if (!valid_desktop_id(desktop_id)) {
        return fail(ErrorCode::InvalidArgument, "find_handler: bad desktop id");
    }
    try {
        if (auto handler = resolve_handler(options, desktop_id)) {
            return std::move(*handler);
        }
        return fail(ErrorCode::NotFound, "find_handler: not installed: " + std::string(desktop_id));
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "find_handler: out of memory");
    }
}

// ---------------------------------------------------------------------------------------------
// set_default_handler
// ---------------------------------------------------------------------------------------------

namespace {

std::string join_ids(const std::vector<std::string>& ids) {
    std::string out;
    for (const std::string& id : ids) {
        out += id;
        out.push_back(';');
    }
    return out;
}

// Applies `transform` to the id list of `key` in `group` of the line vector. When the key has
// lines, the first is rewritten (or removed when the list becomes empty) and the duplicates are
// removed. Otherwise a non-empty list is inserted (the group is appended when missing) when
// `create` is true.
template <class Transform>
void edit_group(std::vector<std::string>& lines, std::string_view group, std::string_view key,
                bool create, Transform&& transform) {
    std::vector<std::size_t> matches;
    std::vector<std::string> ids;
    std::optional<std::size_t> header_line;
    std::optional<std::size_t> last_key_line;
    bool in_group = false;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (const auto header = group_header(lines[i])) {
            in_group = *header == group;
            if (in_group && !header_line) {
                header_line = i;
            } else if (in_group) {
                // A repeated group: keep editing the first one only.
                in_group = false;
            }
            continue;
        }
        if (!in_group) {
            continue;
        }
        if (const auto kv = key_value(lines[i])) {
            last_key_line = i;
            if (kv->key == key) {
                matches.push_back(i);
                for (std::string& id : split_semicolons(kv->value)) {
                    ids.push_back(std::move(id));
                }
            }
        }
    }
    std::vector<std::string> updated = transform(ids);
    if (!matches.empty()) {
        for (std::size_t k = matches.size(); k-- > 1;) {
            lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(matches[k]));
        }
        if (updated.empty()) {
            lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(matches[0]));
        } else {
            lines[matches[0]] = std::string(key) + "=" + join_ids(updated);
        }
        return;
    }
    if (updated.empty() || !create) {
        return;
    }
    const std::string line = std::string(key) + "=" + join_ids(updated);
    if (header_line) {
        const std::size_t at = last_key_line ? *last_key_line + 1 : *header_line + 1;
        lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(at), line);
        return;
    }
    if (!lines.empty() && !trim(lines.back()).empty()) {
        lines.emplace_back();
    }
    lines.push_back("[" + std::string(group) + "]");
    lines.push_back(line);
}

Result<void> make_dirs_0700(const Path& dir) {
    std::error_code ec;
    if (stdfs::is_directory(dir, ec)) {
        return ok();
    }
    std::vector<Path> missing;
    for (Path p = dir; !p.empty() && !stdfs::exists(p, ec); p = p.parent_path()) {
        missing.push_back(p);
        if (p == p.parent_path()) {
            break;
        }
    }
    std::reverse(missing.begin(), missing.end());
    for (const Path& p : missing) {
        if (::mkdir(p.c_str(), 0700) != 0 && errno != EEXIST) {
            return Unexpected<Error>(
                platform_internal::errno_error(errno, "mkdir", p, ErrorCode::IoError));
        }
    }
    return ok();
}

Result<void> write_atomically(const Path& target, std::string_view content) {
    const Path dir = target.parent_path();
    std::string pattern = (dir / (target.filename().string() + ".tmp.XXXXXX")).native();
    const int fd = ::mkstemp(pattern.data());
    if (fd < 0) {
        return Unexpected<Error>(
            platform_internal::errno_error(errno, "create temp file", dir, ErrorCode::IoError));
    }
    const Path temp(pattern);
    const auto cleanup = [&](int err, const char* what) -> Result<void> {
        ::close(fd);
        ::unlink(temp.c_str());
        return Unexpected<Error>(platform_internal::errno_error(err, what, temp));
    };
    if (::fchmod(fd, 0600) != 0) {
        return cleanup(errno, "chmod");
    }
    std::size_t done = 0;
    while (done < content.size()) {
        const ssize_t n = ::write(fd, content.data() + done, content.size() - done);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return cleanup(errno, "write");
        }
        done += static_cast<std::size_t>(n);
    }
    if (::fsync(fd) != 0) {
        return cleanup(errno, "fsync");
    }
    if (::close(fd) != 0) {
        const int err = errno;
        ::unlink(temp.c_str());
        return Unexpected<Error>(platform_internal::errno_error(err, "close", temp));
    }
    if (::rename(temp.c_str(), target.c_str()) != 0) {
        const int err = errno;
        ::unlink(temp.c_str());
        return Unexpected<Error>(platform_internal::errno_error(err, "rename", target));
    }
    const int dir_fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dir_fd >= 0) {
        (void)::fsync(dir_fd);
        ::close(dir_fd);
    }
    return ok();
}

} // namespace

Result<void> set_default_handler(std::string_view mime, std::string_view desktop_id,
                                 const Options& options) {
    if (!valid_mime(mime)) {
        return fail(ErrorCode::InvalidArgument, "set_default_handler: not a MIME type");
    }
    if (!valid_desktop_id(desktop_id)) {
        return fail(ErrorCode::InvalidArgument, "set_default_handler: bad desktop id");
    }
    if (options.config_home.empty() || !options.config_home.is_absolute()) {
        return fail(ErrorCode::InvalidArgument, "set_default_handler: config_home not set");
    }
    try {
        auto handler = find_handler(desktop_id, options);
        if (!handler) {
            return Unexpected<Error>(std::move(handler.error()));
        }
        Path target = options.config_home / "mimeapps.list";
        std::error_code ec;
        if (stdfs::is_symlink(target, ec)) {
            // Keep dotfile-manager symlinks intact: write the file they point to.
            const Path resolved = stdfs::weakly_canonical(target, ec);
            if (!ec) {
                target = resolved;
            }
        }
        std::string text;
        if (const auto existing = read_text_file(target, kMaxMimeappsBytes + 1)) {
            if (existing->size() > kMaxMimeappsBytes) {
                return fail(ErrorCode::IoError, "set_default_handler: mimeapps.list too large");
            }
            text = *existing;
        } else if (stdfs::exists(target, ec)) {
            return fail(ErrorCode::PermissionDenied,
                        "set_default_handler: cannot read " + target.string());
        }
        std::vector<std::string> lines;
        for_each_line(text, [&](std::string_view line) {
            lines.emplace_back(line);
            return true;
        });
        const std::string id(desktop_id);
        const std::string key(mime);
        edit_group(lines, kDefaultGroup, key, true,
                   [&](const std::vector<std::string>&) { return std::vector<std::string>{id}; });
        edit_group(lines, kAddedGroup, key, true, [&](const std::vector<std::string>& old) {
            std::vector<std::string> out{id};
            for (const std::string& other : old) {
                if (other != id) {
                    out.push_back(other);
                }
            }
            return out;
        });
        edit_group(lines, kRemovedGroup, key, false, [&](const std::vector<std::string>& old) {
            std::vector<std::string> out;
            for (const std::string& other : old) {
                if (other != id) {
                    out.push_back(other);
                }
            }
            return out;
        });
        std::string output;
        for (const std::string& line : lines) {
            output += line;
            output.push_back('\n');
        }
        auto dirs = make_dirs_0700(target.parent_path());
        if (!dirs) {
            return dirs;
        }
        return write_atomically(target, output);
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "set_default_handler: out of memory");
    }
}

} // namespace navigator::openwith
