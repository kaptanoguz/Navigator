// navigator/places.hpp
//
// Purpose: the headless data behind the Phase 4 sidebar and GoTo sources:
//            - Places: Home + XDG user dirs + File System, built from platform::UserDirs.
//            - Bookmarks: an ordered, user-edited list that survives unmounted volumes.
//            - Recents: a bounded most-recently-used list of folders Navigator visited.
//            - Location resolution against the current volume list (Online / Offline), resync
//              when a volume returns at another mount point, and "which row contains this
//              folder" for highlighting.
//          JSON (de)serialization and the bookmarks.json / recents.json files.
//          No SDL, no renderer, no JSON library types, no XDG lookups.
// Owner:   contracts (header); implementation in src/core (role: core-fs), tests in tests/core.
//          The header uses value types from volume.hpp (VolumeInfo, VolumeIdentity; inline
//          helpers only) and platform.hpp (UserDirs); src/core calls no src/platform function.
// Thread-safety: Bookmark, BookmarkStore, RecentEntry, RecentsStore, Place and the result
//                structs are plain values (types.hpp rule). The live stores are owned by the UI
//                thread; the background writer receives copies. All member functions and the
//                non-file free functions are pure CPU work (no I/O) and may run on the UI thread.
//                load_* / save_* perform blocking file I/O (see docs/architecture.md "Phase 4"
//                for where the ui calls them). Reentrant: concurrent calls on distinct objects or
//                files are safe; concurrent saves of one file never tear it (last rename wins).
// Errors:  navigator::Result (types.hpp). No exceptions escape; allocation failure is
//          ErrorCode::OutOfMemory where a Result is returned.
//
// Locations: every stored path is absolute and path_input::normalize_absolute()d (lexical, no
// symlink resolution), compared as raw bytes (case-sensitive). Nothing here checks existence:
// a missing folder is never dropped, it is shown Offline / Missing (see "Location status").
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <navigator/platform.hpp>
#include <navigator/types.hpp>
#include <navigator/volume.hpp>

namespace navigator::places {

// ---------------------------------------------------------------------------------------------
// Location status
// ---------------------------------------------------------------------------------------------
//
// A bookmark or recent entry stores the identity of the volume it was on (volume.hpp,
// VolumeIdentity) when that volume can go away. Resolution against the current volume list
// (IVolumeMonitor snapshot) is pure; whether the folder itself still exists needs a stat and is
// a separate worker step:
//   Online   the volume is mounted (or the location has no recorded volume); `path` is where
//            the folder is now (rebased when the volume came back at another mount point).
//   Offline  the recorded volume is not mounted. The entry stays listed, greyed with an
//            "offline" badge; activating it shows a notice instead of navigating. Another
//            volume mounted at the old mount point does NOT make it Online (wrong disk).
//   Missing  set by the ui's probe only: Online, but fs::stat(path, follow) failed with
//            NotFound or the target is not a directory (folder deleted or renamed). Shown
//            greyed with a "missing" badge; never removed automatically.
// The probe runs on a worker after each volume snapshot change and after add/load, and is
// skipped for VolumeKind::Network volumes (a hung share must not stall it; they stay Online).
// Other stat errors (PermissionDenied, IoError) leave the status Online.
enum class LocationStatus : std::uint8_t {
    Online = 0,
    Offline = 1,
    Missing = 2,
};

struct ResolvedLocation {
    LocationStatus status = LocationStatus::Online;
    // Online: the effective path (== the stored path unless rebased). Offline: the stored path.
    Path path;
    // Index into the volume list of the matching volume (rebasing source, usage bar); nullopt
    // when Offline or when the location has no recorded volume.
    std::optional<std::size_t> volume_index;
};

// Volumes whose identity is worth recording: everything except VolumeKind::Root, Home and
// Virtual. Root and Home are always mounted while Navigator runs, and their UUID changes on a
// reinstall, which would otherwise turn every bookmark Offline forever.
[[nodiscard]] constexpr bool is_tracked_volume(const VolumeInfo& volume) noexcept {
    return volume.kind != VolumeKind::Root && volume.kind != VolumeKind::Home &&
           volume.kind != VolumeKind::Virtual;
}

// The volume of `volumes` whose mount_point is the longest component-wise prefix of `path`
// (lexical; no symlink resolution, no I/O -- unlike volume_for()). nullopt when none matches
// (an empty list, or a relative path). Pure.
[[nodiscard]] std::optional<std::size_t> containing_volume(const Path& path,
                                                           Span<const VolumeInfo> volumes);

// The identity to store for a new bookmark / recent entry at `path`: identity_of() the
// containing volume when is_tracked_volume() holds for it and its uuid or device is non-empty,
// else nullopt. Pure; the ui passes the volume monitor's snapshot.
[[nodiscard]] std::optional<VolumeIdentity> identity_for(const Path& path,
                                                         Span<const VolumeInfo> volumes);

// Resolution rule:
//   - no `volume`: Online, path unchanged, volume_index = containing_volume(path).
//   - `volume` set: the first entry of `volumes` with volume_matches(*volume, v). None: Offline.
//     Found: Online; if v.mount_point != volume->mount_point and `path` lies under
//     volume->mount_point, path = v.mount_point / (path relative to volume->mount_point).
// Pure.
[[nodiscard]] ResolvedLocation resolve_location(const Path& path,
                                                const std::optional<VolumeIdentity>& volume,
                                                Span<const VolumeInfo> volumes);

// Index of the entry of `roots` that is the deepest component-wise prefix of `folder` (equal
// counts), for highlighting the sidebar row that contains the active pane's folder: the ui
// passes the effective paths of all visible rows (bookmarks, places, volume mount points) and
// highlights the winner only. Ties (equal paths in two sections): the lowest index. Lexical,
// pure. nullopt when nothing contains `folder`.
[[nodiscard]] std::optional<std::size_t> deepest_containing(const Path& folder,
                                                            Span<const Path> roots);

// Text for a location without a custom name: fs::to_display_name of the last component, "/" for
// the root. Pure.
[[nodiscard]] std::string default_label(const Path& path);

// ---------------------------------------------------------------------------------------------
// Places
// ---------------------------------------------------------------------------------------------

enum class PlaceKind : std::uint8_t {
    Home = 0,
    Desktop = 1,
    Documents = 2,
    Downloads = 3,
    Music = 4,
    Pictures = 5,
    Videos = 6,
    Trash = 7,      // reserved: ops phase (trash:// browsing); build_places() does not emit it
    FileSystem = 8, // "/"
};

// i18n key of the row label ("place.home", "place.desktop", "place.documents",
// "place.downloads", "place.music", "place.pictures", "place.videos", "place.trash",
// "place.file_system"). Places are labelled by kind in the UI language, not by the folder name
// (which is the install language's).
[[nodiscard]] constexpr std::string_view label_key(PlaceKind kind) noexcept {
    switch (kind) {
    case PlaceKind::Home: return "place.home";
    case PlaceKind::Desktop: return "place.desktop";
    case PlaceKind::Documents: return "place.documents";
    case PlaceKind::Downloads: return "place.downloads";
    case PlaceKind::Music: return "place.music";
    case PlaceKind::Pictures: return "place.pictures";
    case PlaceKind::Videos: return "place.videos";
    case PlaceKind::Trash: return "place.trash";
    case PlaceKind::FileSystem: return "place.file_system";
    }
    return "place.home";
}

struct Place {
    PlaceKind kind = PlaceKind::Home;
    Path path; // absolute, normalized
};

// The Places section, in this order: Home, Desktop, Documents, Downloads, Music, Pictures,
// Videos, File System ("/"). A user dir is left out when it is empty, relative, equal to `home`
// (xdg "disabled"), or equal to an earlier entry. Home is left out only when `home` is empty or
// relative. Existence is not checked (the ui may hide rows whose folder does not exist, using
// the worker probe). The File System row carries the "/" volume's usage bar and Home the
// VolumeKind::Home volume's, so the Storage section lists neither (docs/architecture.md).
// Pure.
[[nodiscard]] std::vector<Place> build_places(const Path& home, const platform::UserDirs& dirs);

// ---------------------------------------------------------------------------------------------
// Bookmarks
// ---------------------------------------------------------------------------------------------

// Runtime identity of a bookmark, unique within ONE BookmarkStore for its lifetime, never
// reused, never 0. NOT persisted: loading assigns fresh ids (as layout ids). Command::arg
// carries `value` for bookmark commands (commands.hpp).
struct BookmarkId {
    std::uint32_t value = 0;
    [[nodiscard]] constexpr bool valid() const noexcept { return value != 0; }
    friend constexpr bool operator==(BookmarkId, BookmarkId) = default;
    friend constexpr auto operator<=>(BookmarkId, BookmarkId) = default;
};

struct Bookmark {
    BookmarkId id;
    // Custom display name (UTF-8); empty: default_label(path).
    std::string name;
    // Absolute, normalized; raw bytes.
    Path path;
    // Volume the folder was on when added (identity_for), or nullopt (Root/Home/untracked).
    std::optional<VolumeIdentity> volume;
    // When it was added (persisted with second precision).
    Timestamp added{};

    friend bool operator==(const Bookmark&, const Bookmark&) = default;
};

// Most bookmarks a store holds; add() beyond it fails, loading keeps the first ones.
inline constexpr std::size_t kMaxBookmarks = 1000;

// Ordered list (sidebar order, user-defined). Every edit is all-or-nothing.
class BookmarkStore {
public:
    BookmarkStore() = default;

    // Inserts a bookmark at `index` (clamped; nullopt: at the end). `path` is normalized first.
    // Errors: InvalidArgument (empty or relative path; name not valid UTF-8 or containing
    // control characters < 0x20 / 0x7F; kMaxBookmarks reached), AlreadyExists (a bookmark
    // with the same normalized path exists; bookmarks.toggle removes it instead), OutOfMemory.
    Result<BookmarkId> add(const Path& path, std::optional<VolumeIdentity> volume, Timestamp added,
                           std::string name = {}, std::optional<std::size_t> index = std::nullopt);

    // Removes and returns the bookmark. Errors: NotFound.
    Result<Bookmark> remove(BookmarkId id);

    // Sets the custom name; empty restores the default label. Errors: NotFound,
    // InvalidArgument (as add()).
    Result<void> rename(BookmarkId id, std::string name);

    // Moves the bookmark so it ends up at min(index, size() - 1). Errors: NotFound.
    Result<void> move(BookmarkId id, std::size_t index);

    // nullptr when absent. Valid until the next non-const call.
    [[nodiscard]] const Bookmark* find(BookmarkId id) const noexcept;
    // By normalized path (the argument is normalized first).
    [[nodiscard]] const Bookmark* find(const Path& path) const;
    [[nodiscard]] bool contains(const Path& path) const { return find(path) != nullptr; }
    // Position in list(); nullopt when absent.
    [[nodiscard]] std::optional<std::size_t> index_of(BookmarkId id) const noexcept;

    // Sidebar order. Valid until the next non-const call.
    [[nodiscard]] Span<const Bookmark> list() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] bool empty() const noexcept;

    // "Resync when the volume returns": for each bookmark whose volume matches an entry of
    // `volumes` (volume_matches), rewrite path (rebased as in resolve_location) and the
    // identity's mount_point and device to the volume's current values. A rewrite that would
    // duplicate another bookmark's path keeps the bookmark unchanged. Offline bookmarks are
    // untouched. Returns how many bookmarks changed (the ui saves when > 0).
    std::size_t resync(Span<const VolumeInfo> volumes);

private:
    std::vector<Bookmark> bookmarks_;
    std::uint32_t next_id_ = 1;
};

// ---------------------------------------------------------------------------------------------
// Recents
// ---------------------------------------------------------------------------------------------
//
// Navigator's own navigation history (docs/review/phase-3.md: not a File Pilot claim, and not
// GTK's recently-used.xbel). The ui records a folder when a tab's listing of a NEW folder
// succeeds (navigation, GoTo, sidebar, breadcrumb, opening a tab); refresh, re-activating a tab
// and session restore do not record.

struct RecentEntry {
    Path path; // absolute, normalized
    std::optional<VolumeIdentity> volume;
    Timestamp last_visit{};        // persisted with second precision
    std::uint32_t visit_count = 1; // >= 1, saturating

    friend bool operator==(const RecentEntry&, const RecentEntry&) = default;
};

// Persisted preferences of the Recents feature (docs/features.md: tracking can be disabled,
// highlighting, "Sort Recents On Top").
struct RecentsOptions {
    // false: record() does nothing (existing entries stay until recents.clear).
    bool enabled = true;
    // Stub (Phase 4 persists it; no list behaviour yet): highlight recent folders in file lists
    // without moving them.
    bool highlight = false;
    // Stub (persisted only): sort recent folders to the top of file lists.
    bool sort_on_top = false;

    friend constexpr bool operator==(const RecentsOptions&, const RecentsOptions&) = default;
};

inline constexpr std::size_t kDefaultRecentsCapacity = 50;
inline constexpr std::size_t kMaxRecentsCapacity = 500;

// Bounded most-recently-used list, newest first. Offline entries are kept like bookmarks; when
// the list is full, record() drops the least recently visited entry, whatever its status.
class RecentsStore {
public:
    RecentsStore() = default; // capacity kDefaultRecentsCapacity
    // Clamped to [1, kMaxRecentsCapacity].
    explicit RecentsStore(std::size_t capacity);

    // Records a visit when options().enabled: an existing entry (same normalized path) moves to
    // the front, gets last_visit = now, visit_count + 1 and the new volume identity; otherwise a
    // new entry is inserted at the front (count 1) and the oldest entry beyond capacity() is
    // dropped. Empty or relative paths are ignored. Returns true when the list changed.
    bool record(const Path& path, std::optional<VolumeIdentity> volume, Timestamp now);

    // Removes the entry for `path` (normalized first). Returns false when absent.
    bool remove(const Path& path);
    // recents.clear. Options are kept.
    void clear() noexcept;

    [[nodiscard]] const RecentsOptions& options() const noexcept;
    void set_options(const RecentsOptions& options) noexcept;

    // Newest first. Valid until the next non-const call.
    [[nodiscard]] Span<const RecentEntry> entries() const noexcept;
    [[nodiscard]] const RecentEntry* find(const Path& path) const;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::size_t capacity() const noexcept;

    // As BookmarkStore::resync. When a rebased path equals another entry's path the two are
    // merged: the newer last_visit and the summed (saturating) visit_count are kept at the newer
    // entry's position. Returns how many entries changed or were merged away.
    std::size_t resync(Span<const VolumeInfo> volumes);

    // Loader support (recents_from_json): replaces all entries with `entries`, given newest
    // first. Entries with empty or relative paths are dropped, paths are normalized, duplicates
    // keep the first (newest) occurrence, visit_count is raised to at least 1, and the list is
    // truncated to capacity(). Options are kept.
    void restore(std::vector<RecentEntry> entries);

private:
    std::vector<RecentEntry> entries_;
    std::size_t capacity_ = kDefaultRecentsCapacity;
    RecentsOptions options_;
};

// ---------------------------------------------------------------------------------------------
// JSON format (version 1)
// ---------------------------------------------------------------------------------------------
//
// bookmarks.json:
//   { "version": 1,
//     "bookmarks": [ { "path": <bytes>, "name": "Work", "added": 1731800000,
//                      "volume": <volume> }, ... ] }                      sidebar order
// recents.json:
//   { "version": 1,
//     "options": { "enabled": true, "highlight": false, "sort_on_top": false },
//     "recents": [ { "path": <bytes>, "last_visit": 1731800000, "visits": 3,
//                    "volume": <volume> }, ... ] }                        newest first
// <volume> = { "uuid": "1234-ABCD", "root": "/", "device": "/dev/sdb1",
//              "mount_point": <bytes> }
// <bytes>  = as layout.hpp: a JSON string for valid UTF-8, else { "hex": "<lowercase hex>" }.
// "added" / "last_visit": integer seconds since the Unix epoch (UTC), sub-second part
// truncated. "name" is written only when non-empty, "volume" only when set, "uuid" only when
// non-empty. Ids are never written. Output: UTF-8, 2-space indent, keys in the order shown,
// deterministic.
//
// Loading is lenient (as layout.hpp) and never drops an entry for being offline or missing:
//   - The text must be a JSON object, else InvalidArgument.
//   - Unknown fields are ignored; missing or wrong-typed fields take their defaults; "version"
//     missing, < 1 or not an integer is 1, higher versions load with these rules.
//   - Entries with a missing, empty or relative path are dropped; paths are normalized; a later
//     entry with the same path as an earlier one is dropped.
//   - A <volume> with neither "uuid" nor "device" counts as absent; an invalid name (see
//     BookmarkStore::add) becomes empty; timestamps that are not integers become the epoch;
//     "visits" < 1 or not an integer becomes 1 (saturated at UINT32_MAX).
//   - Bookmarks beyond kMaxBookmarks and recents beyond the store capacity are dropped.

// Errors: OutOfMemory.
[[nodiscard]] Result<std::string> bookmarks_to_json(const BookmarkStore& store);
// Errors: InvalidArgument (not JSON, or not an object).
[[nodiscard]] Result<BookmarkStore> bookmarks_from_json(std::string_view text);

// Errors: OutOfMemory.
[[nodiscard]] Result<std::string> recents_to_json(const RecentsStore& store);
// The loaded store has `capacity` (clamped as the RecentsStore constructor).
// Errors: InvalidArgument (not JSON, or not an object).
[[nodiscard]] Result<RecentsStore>
recents_from_json(std::string_view text, std::size_t capacity = kDefaultRecentsCapacity);

// ---------------------------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------------------------
//
// Under the Navigator config root (layout.hpp: platform::user_dir(Config) / "navigator", or
// AppConfig::config_dir):
//     <root>/bookmarks.json
//     <root>/recents.json

inline constexpr std::string_view kBookmarksFileName = "bookmarks.json";
inline constexpr std::string_view kRecentsFileName = "recents.json";

[[nodiscard]] inline Path bookmarks_file(const Path& config_root) {
    return config_root / kBookmarksFileName;
}
[[nodiscard]] inline Path recents_file(const Path& config_root) {
    return config_root / kRecentsFileName;
}

// Same file rules as layout::load_session / save_session: files over layout::kMaxDocumentBytes
// (16 MiB) are rejected without parsing; a file that is not a JSON object is renamed to
// "<file>.bad" (replacing an older one) and InvalidArgument is returned; saves are atomic
// (temporary file in the same directory, mode 0600, fsync, rename; parent directories created
// with mode 0700; on failure the old file is untouched).
// Errors (load): InvalidArgument (empty path, too large, not an object), NotFound (no file:
// first start, empty store), PermissionDenied, IoError, OutOfMemory.
// Errors (save): InvalidArgument (empty path), PermissionDenied, IoError, OutOfMemory.
[[nodiscard]] Result<BookmarkStore> load_bookmarks(const Path& file);
Result<void> save_bookmarks(const Path& file, const BookmarkStore& store);

[[nodiscard]] Result<RecentsStore> load_recents(const Path& file,
                                                std::size_t capacity = kDefaultRecentsCapacity);
Result<void> save_recents(const Path& file, const RecentsStore& store);

} // namespace navigator::places
