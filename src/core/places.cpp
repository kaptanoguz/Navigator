// navigator/places.hpp: location resolution against the volume list, the Places list, and the
// bookmark / recents stores. Pure CPU work; no I/O, no src/platform call (the JSON and file
// functions are in places_json.cpp).
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <navigator/fs.hpp>
#include <navigator/path_input.hpp>
#include <navigator/places.hpp>

#include "places_internal.hpp"
#include "utf8.hpp"

namespace navigator::places {
namespace {

using path_input::normalize_absolute;

// `root` is a component-wise prefix of `path` (both normalized absolute; root may be "/").
[[nodiscard]] bool is_under(std::string_view path, std::string_view root) noexcept {
    if (root.empty() || path.empty()) {
        return false;
    }
    if (root == "/") {
        return path.front() == '/';
    }
    return path.size() >= root.size() && path.substr(0, root.size()) == root &&
           (path.size() == root.size() || path[root.size()] == '/');
}

// An identity with neither uuid nor device matches nothing (volume.hpp): treated as absent.
[[nodiscard]] const VolumeIdentity*
usable_identity(const std::optional<VolumeIdentity>& volume) noexcept {
    if (!volume.has_value() || (volume->uuid.empty() && volume->device.empty())) {
        return nullptr;
    }
    return &*volume;
}

[[nodiscard]] std::optional<VolumeIdentity> usable(std::optional<VolumeIdentity> volume) {
    if (usable_identity(volume) == nullptr) {
        return std::nullopt;
    }
    return volume;
}

[[nodiscard]] std::optional<std::size_t> matching_volume(const VolumeIdentity& id,
                                                         Span<const VolumeInfo> volumes) {
    for (std::size_t i = 0; i < volumes.size(); ++i) {
        if (volume_matches(id, volumes[i])) {
            return i;
        }
    }
    return std::nullopt;
}

// Where `path` (recorded on a volume mounted at `old_mount`) is when that volume is mounted at
// `new_mount`. Unchanged when the mount point did not move or `path` is not below `old_mount`.
[[nodiscard]] Path rebase(const Path& path, const Path& old_mount, const Path& new_mount) {
    if (old_mount == new_mount) {
        return path;
    }
    const Path from = normalize_absolute(old_mount);
    const Path to = normalize_absolute(new_mount);
    const Path here = normalize_absolute(path);
    if (from.empty() || to.empty() || here.empty() || !is_under(here.native(), from.native())) {
        return path;
    }
    std::string_view rest(here.native());
    rest.remove_prefix(from.native() == "/" ? 1 : from.native().size());
    while (!rest.empty() && rest.front() == '/') {
        rest.remove_prefix(1);
    }
    return rest.empty() ? to : normalize_absolute(to / std::string(rest));
}

// What resync() writes for a location whose volume is `v`: the rebased path and the identity
// with the volume's current mount point and device.
struct Resynced {
    Path path;
    VolumeIdentity volume;
};

[[nodiscard]] std::optional<Resynced> resynced(const Path& path,
                                               const std::optional<VolumeIdentity>& volume,
                                               Span<const VolumeInfo> volumes) {
    const VolumeIdentity* id = usable_identity(volume);
    if (id == nullptr) {
        return std::nullopt;
    }
    const std::optional<std::size_t> index = matching_volume(*id, volumes);
    if (!index.has_value()) {
        return std::nullopt;
    }
    const VolumeInfo& v = volumes[*index];
    Resynced out{rebase(path, id->mount_point, v.mount_point), *id};
    out.volume.mount_point = v.mount_point;
    out.volume.device = v.device;
    if (out.path == path && out.volume == *id) {
        return std::nullopt;
    }
    return out;
}

[[nodiscard]] std::uint32_t saturating_add(std::uint32_t a, std::uint32_t b) noexcept {
    return a > std::numeric_limits<std::uint32_t>::max() - b
               ? std::numeric_limits<std::uint32_t>::max()
               : a + b;
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Location status
// ---------------------------------------------------------------------------------------------

std::optional<std::size_t> containing_volume(const Path& path, Span<const VolumeInfo> volumes) {
    const Path here = normalize_absolute(path);
    if (here.empty()) {
        return std::nullopt;
    }
    std::optional<std::size_t> best;
    std::size_t best_length = 0;
    for (std::size_t i = 0; i < volumes.size(); ++i) {
        const Path mount = normalize_absolute(volumes[i].mount_point);
        if (mount.empty() || !is_under(here.native(), mount.native())) {
            continue;
        }
        const std::size_t length = mount.native() == "/" ? 0 : mount.native().size();
        if (!best || length > best_length) {
            best = i;
            best_length = length;
        }
    }
    return best;
}

std::optional<VolumeIdentity> identity_for(const Path& path, Span<const VolumeInfo> volumes) {
    const std::optional<std::size_t> index = containing_volume(path, volumes);
    if (!index) {
        return std::nullopt;
    }
    const VolumeInfo& volume = volumes[*index];
    if (!is_tracked_volume(volume)) {
        return std::nullopt;
    }
    return usable(identity_of(volume));
}

ResolvedLocation resolve_location(const Path& path, const std::optional<VolumeIdentity>& volume,
                                  Span<const VolumeInfo> volumes) {
    ResolvedLocation out;
    out.path = path;
    const VolumeIdentity* id = usable_identity(volume);
    if (id == nullptr) {
        out.volume_index = containing_volume(path, volumes);
        return out;
    }
    const std::optional<std::size_t> index = matching_volume(*id, volumes);
    if (!index.has_value()) {
        out.status = LocationStatus::Offline;
        return out;
    }
    out.volume_index = index;
    out.path = rebase(path, id->mount_point, volumes[*index].mount_point);
    return out;
}

std::optional<std::size_t> deepest_containing(const Path& folder, Span<const Path> roots) {
    const Path here = normalize_absolute(folder);
    if (here.empty()) {
        return std::nullopt;
    }
    std::optional<std::size_t> best;
    std::size_t best_length = 0;
    for (std::size_t i = 0; i < roots.size(); ++i) {
        const Path root = normalize_absolute(roots[i]);
        if (root.empty() || !is_under(here.native(), root.native())) {
            continue;
        }
        // All candidates are prefixes of one path: longer text == more components.
        const std::size_t length = root.native() == "/" ? 0 : root.native().size();
        if (!best || length > best_length) {
            best = i;
            best_length = length;
        }
    }
    return best;
}

std::string default_label(const Path& path) {
    std::string_view text(path.native());
    const bool had_slash = !text.empty() && text.front() == '/';
    while (!text.empty() && text.back() == '/') {
        text.remove_suffix(1);
    }
    if (text.empty()) {
        return had_slash ? std::string("/") : std::string{};
    }
    const std::size_t slash = text.rfind('/');
    if (slash != std::string_view::npos) {
        text.remove_prefix(slash + 1);
    }
    return fs::to_display_name(text);
}

// ---------------------------------------------------------------------------------------------
// Places
// ---------------------------------------------------------------------------------------------

std::vector<Place> build_places(const Path& home, const platform::UserDirs& dirs) {
    std::vector<Place> out;
    const Path home_path = normalize_absolute(home);
    if (!home_path.empty()) {
        out.push_back(Place{PlaceKind::Home, home_path});
    }
    const std::pair<PlaceKind, const Path*> user_dirs[] = {
        {PlaceKind::Desktop, &dirs.desktop},     {PlaceKind::Documents, &dirs.documents},
        {PlaceKind::Downloads, &dirs.downloads}, {PlaceKind::Music, &dirs.music},
        {PlaceKind::Pictures, &dirs.pictures},   {PlaceKind::Videos, &dirs.videos},
    };
    for (const auto& [kind, dir] : user_dirs) {
        Path path = normalize_absolute(*dir);
        if (path.empty() || path == home_path ||
            std::any_of(out.begin(), out.end(), [&](const Place& p) { return p.path == path; })) {
            continue;
        }
        out.push_back(Place{kind, std::move(path)});
    }
    out.push_back(Place{PlaceKind::FileSystem, Path("/")});
    return out;
}

// ---------------------------------------------------------------------------------------------
// Bookmarks
// ---------------------------------------------------------------------------------------------

bool internal::is_valid_bookmark_name(std::string_view name) noexcept {
    for (const char c : name) {
        const auto b = static_cast<unsigned char>(c);
        if (b < 0x20 || b == 0x7F) {
            return false;
        }
    }
    return core::is_valid_utf8(name);
}

Result<BookmarkId> BookmarkStore::add(const Path& path, std::optional<VolumeIdentity> volume,
                                      Timestamp added, std::string name,
                                      std::optional<std::size_t> index) {
    try {
        Path normal = normalize_absolute(path);
        if (normal.empty()) {
            return fail(ErrorCode::InvalidArgument, "bookmark path must be absolute");
        }
        if (!internal::is_valid_bookmark_name(name)) {
            return fail(ErrorCode::InvalidArgument, "bookmark name is not valid UTF-8 text");
        }
        if (bookmarks_.size() >= kMaxBookmarks) {
            return fail(ErrorCode::InvalidArgument, "too many bookmarks");
        }
        if (find(normal) != nullptr) {
            return fail(ErrorCode::AlreadyExists, "already bookmarked: '" + normal.native() + "'");
        }
        Bookmark bookmark;
        bookmark.id = BookmarkId{next_id_};
        bookmark.name = std::move(name);
        bookmark.path = std::move(normal);
        bookmark.volume = usable(std::move(volume));
        bookmark.added = added;
        const std::size_t at = std::min(index.value_or(bookmarks_.size()), bookmarks_.size());
        bookmarks_.insert(bookmarks_.begin() + static_cast<std::ptrdiff_t>(at),
                          std::move(bookmark));
        ++next_id_;
        return BookmarkId{next_id_ - 1};
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "out of memory while adding a bookmark");
    }
}

Result<Bookmark> BookmarkStore::remove(BookmarkId id) {
    const std::optional<std::size_t> at = index_of(id);
    if (!at) {
        return fail(ErrorCode::NotFound, "no such bookmark");
    }
    Bookmark removed = std::move(bookmarks_[*at]);
    bookmarks_.erase(bookmarks_.begin() + static_cast<std::ptrdiff_t>(*at));
    return removed;
}

Result<void> BookmarkStore::rename(BookmarkId id, std::string name) {
    const std::optional<std::size_t> at = index_of(id);
    if (!at) {
        return fail(ErrorCode::NotFound, "no such bookmark");
    }
    if (!internal::is_valid_bookmark_name(name)) {
        return fail(ErrorCode::InvalidArgument, "bookmark name is not valid UTF-8 text");
    }
    bookmarks_[*at].name = std::move(name);
    return ok();
}

Result<void> BookmarkStore::move(BookmarkId id, std::size_t index) {
    const std::optional<std::size_t> at = index_of(id);
    if (!at) {
        return fail(ErrorCode::NotFound, "no such bookmark");
    }
    const std::size_t to = std::min(index, bookmarks_.size() - 1);
    const auto first = bookmarks_.begin();
    if (to < *at) {
        std::rotate(first + static_cast<std::ptrdiff_t>(to),
                    first + static_cast<std::ptrdiff_t>(*at),
                    first + static_cast<std::ptrdiff_t>(*at + 1));
    } else if (to > *at) {
        std::rotate(first + static_cast<std::ptrdiff_t>(*at),
                    first + static_cast<std::ptrdiff_t>(*at + 1),
                    first + static_cast<std::ptrdiff_t>(to + 1));
    }
    return ok();
}

const Bookmark* BookmarkStore::find(BookmarkId id) const noexcept {
    const auto it = std::find_if(bookmarks_.begin(), bookmarks_.end(),
                                 [&](const Bookmark& b) { return b.id == id; });
    return it == bookmarks_.end() ? nullptr : &*it;
}

const Bookmark* BookmarkStore::find(const Path& path) const {
    const Path normal = normalize_absolute(path);
    if (normal.empty()) {
        return nullptr;
    }
    const auto it = std::find_if(bookmarks_.begin(), bookmarks_.end(),
                                 [&](const Bookmark& b) { return b.path == normal; });
    return it == bookmarks_.end() ? nullptr : &*it;
}

std::optional<std::size_t> BookmarkStore::index_of(BookmarkId id) const noexcept {
    const auto it = std::find_if(bookmarks_.begin(), bookmarks_.end(),
                                 [&](const Bookmark& b) { return b.id == id; });
    if (it == bookmarks_.end()) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(it - bookmarks_.begin());
}

Span<const Bookmark> BookmarkStore::list() const noexcept { return bookmarks_; }
std::size_t BookmarkStore::size() const noexcept { return bookmarks_.size(); }
bool BookmarkStore::empty() const noexcept { return bookmarks_.empty(); }

std::size_t BookmarkStore::resync(Span<const VolumeInfo> volumes) {
    std::size_t changed = 0;
    for (Bookmark& bookmark : bookmarks_) {
        std::optional<Resynced> next = resynced(bookmark.path, bookmark.volume, volumes);
        if (!next) {
            continue;
        }
        if (next->path != bookmark.path &&
            std::any_of(bookmarks_.begin(), bookmarks_.end(),
                        [&](const Bookmark& other) { return other.path == next->path; })) {
            continue; // would duplicate another bookmark: keep this one unchanged
        }
        bookmark.path = std::move(next->path);
        bookmark.volume = std::move(next->volume);
        ++changed;
    }
    return changed;
}

// ---------------------------------------------------------------------------------------------
// Recents
// ---------------------------------------------------------------------------------------------

RecentsStore::RecentsStore(std::size_t capacity)
    : capacity_(std::clamp<std::size_t>(capacity, 1, kMaxRecentsCapacity)) {}

bool RecentsStore::record(const Path& path, std::optional<VolumeIdentity> volume, Timestamp now) {
    if (!options_.enabled) {
        return false;
    }
    try {
        Path normal = normalize_absolute(path);
        if (normal.empty()) {
            return false;
        }
        const auto it = std::find_if(entries_.begin(), entries_.end(),
                                     [&](const RecentEntry& e) { return e.path == normal; });
        if (it != entries_.end()) {
            it->last_visit = now;
            it->visit_count = saturating_add(it->visit_count, 1);
            it->volume = usable(std::move(volume));
            std::rotate(entries_.begin(), it, it + 1);
            return true;
        }
        RecentEntry entry;
        entry.path = std::move(normal);
        entry.volume = usable(std::move(volume));
        entry.last_visit = now;
        entry.visit_count = 1;
        entries_.insert(entries_.begin(), std::move(entry));
        if (entries_.size() > capacity_) {
            entries_.resize(capacity_);
        }
        return true;
    } catch (const std::bad_alloc&) {
        return false;
    }
}

bool RecentsStore::remove(const Path& path) {
    const Path normal = normalize_absolute(path);
    const auto it = std::find_if(entries_.begin(), entries_.end(),
                                 [&](const RecentEntry& e) { return e.path == normal; });
    if (normal.empty() || it == entries_.end()) {
        return false;
    }
    entries_.erase(it);
    return true;
}

void RecentsStore::clear() noexcept { entries_.clear(); }

const RecentsOptions& RecentsStore::options() const noexcept { return options_; }
void RecentsStore::set_options(const RecentsOptions& options) noexcept { options_ = options; }

Span<const RecentEntry> RecentsStore::entries() const noexcept { return entries_; }

const RecentEntry* RecentsStore::find(const Path& path) const {
    const Path normal = normalize_absolute(path);
    if (normal.empty()) {
        return nullptr;
    }
    const auto it = std::find_if(entries_.begin(), entries_.end(),
                                 [&](const RecentEntry& e) { return e.path == normal; });
    return it == entries_.end() ? nullptr : &*it;
}

std::size_t RecentsStore::size() const noexcept { return entries_.size(); }
bool RecentsStore::empty() const noexcept { return entries_.empty(); }
std::size_t RecentsStore::capacity() const noexcept { return capacity_; }

std::size_t RecentsStore::resync(Span<const VolumeInfo> volumes) {
    std::size_t changed = 0;
    for (std::size_t i = 0; i < entries_.size();) {
        std::optional<Resynced> found = resynced(entries_[i].path, entries_[i].volume, volumes);
        if (!found.has_value()) {
            ++i;
            continue;
        }
        Resynced next = std::move(*found);
        std::size_t other = entries_.size();
        for (std::size_t j = 0; j < entries_.size(); ++j) {
            if (j != i && entries_[j].path == next.path) {
                other = j;
                break;
            }
        }
        if (other == entries_.size()) {
            entries_[i].path = std::move(next.path);
            entries_[i].volume = std::move(next.volume);
            ++changed;
            ++i;
            continue;
        }
        // Merge into the newer entry's position (ties: the one nearer the front).
        const RecentEntry& a = entries_[i];
        const RecentEntry& b = entries_[other];
        const bool keep_i =
            a.last_visit > b.last_visit || (a.last_visit == b.last_visit && i < other);
        RecentEntry merged;
        merged.path = std::move(next.path);
        merged.volume = std::move(next.volume);
        merged.last_visit = std::max(a.last_visit, b.last_visit);
        merged.visit_count = saturating_add(a.visit_count, b.visit_count);
        const std::size_t survivor = keep_i ? i : other;
        const std::size_t gone = keep_i ? other : i;
        entries_[survivor] = std::move(merged);
        entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(gone));
        changed += 2; // the survivor changed, the other entry was merged away
        i = 0;        // indices moved; already resynced entries are stable on a rescan
    }
    return changed;
}

// ---------------------------------------------------------------------------------------------
// Loader access (places_internal.hpp)
// ---------------------------------------------------------------------------------------------
//
// recents_from_json() must restore visit counts and timestamps, which RecentsStore's public API
// cannot set (record() always counts one visit "now"), and places.hpp declares no friend. The
// explicit-instantiation access idiom below names the private member legally ([temp.spec]:
// access checking does not apply to the template arguments of an explicit instantiation).
// TODO(contracts): replace with a declared friend or a restore function in places.hpp.
namespace {

struct RecentsEntriesTag {
    using type = std::vector<RecentEntry> RecentsStore::*;
    friend type member_pointer(RecentsEntriesTag) noexcept;
};

template <class Tag, typename Tag::type Member> struct PrivateMember {
    friend typename Tag::type member_pointer(Tag) noexcept { return Member; }
};

template struct PrivateMember<RecentsEntriesTag, &RecentsStore::entries_>;

} // namespace

void internal::append_loaded(RecentsStore& store, RecentEntry entry) {
    (store.*member_pointer(RecentsEntriesTag{})).push_back(std::move(entry));
}

} // namespace navigator::places
