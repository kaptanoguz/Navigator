// Compile-only check that the Phase 4 contracts (places: bookmarks, recents, places list,
// location resolution and files; path_input: expansion, case correction, completion, copy as
// path; platform user dirs; volume identity/usage; SidebarState in the session; the new
// CommandIds) are usable as documented. Nothing here is linked or run.
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <navigator/commands.hpp>
#include <navigator/fs.hpp>
#include <navigator/layout.hpp>
#include <navigator/path_input.hpp>
#include <navigator/places.hpp>
#include <navigator/platform.hpp>
#include <navigator/types.hpp>
#include <navigator/volume.hpp>

namespace navigator::contracts_check {

namespace pl = navigator::places;
namespace pi = navigator::path_input;
namespace lay = navigator::layout;

// Stable ids that must never change.
static_assert(static_cast<std::uint32_t>(CommandId::ToggleSidebar) == 5);
static_assert(static_cast<std::uint32_t>(CommandId::ToggleViewLock) == 6);
static_assert(static_cast<std::uint32_t>(CommandId::FocusAddressBar) == 1004);
static_assert(static_cast<std::uint32_t>(CommandId::GoTo) == 1005);
static_assert(static_cast<std::uint32_t>(CommandId::FocusSidebar) == 1260);
static_assert(static_cast<std::uint32_t>(CommandId::ToggleBookmark) == 1261);
static_assert(static_cast<std::uint32_t>(CommandId::RenameBookmark) == 1262);
static_assert(static_cast<std::uint32_t>(CommandId::MoveBookmarkUp) == 1263);
static_assert(static_cast<std::uint32_t>(CommandId::MoveBookmarkDown) == 1264);
static_assert(static_cast<std::uint32_t>(CommandId::ClearRecents) == 1270);
static_assert(static_cast<std::uint32_t>(CommandId::ToggleRecentsTracking) == 1271);
static_assert(static_cast<std::uint32_t>(CommandId::CopyPath) == 1280);
static_assert(CopyPathArg::CurrentFolder != CopyPathArg::None);

// Volume usage and identity helpers are constexpr / inline and pure.
static_assert(used_fraction(SpaceInfo{}) == 0.0);
static_assert(used_fraction(SpaceInfo{100, 0, 0}) == 1.0);
static_assert(used_fraction(SpaceInfo{100, 50, 50}) == 0.5);
// Reserved blocks (free > available) count neither as used nor as available.
static_assert(used_fraction(SpaceInfo{100, 60, 20}) == 40.0 / 60.0);

// Places labels and tracking rule.
static_assert(pl::label_key(pl::PlaceKind::Downloads) == "place.downloads");
static_assert(pl::label_key(pl::PlaceKind::FileSystem) == "place.file_system");
static_assert(!pl::BookmarkId{}.valid() && pl::BookmarkId{3}.valid());
static_assert(pl::BookmarkId{1} < pl::BookmarkId{2});
static_assert(!std::is_convertible_v<std::uint32_t, pl::BookmarkId>);
static_assert(pl::RecentsOptions{}.enabled && !pl::RecentsOptions{}.highlight);
static_assert(pl::kDefaultRecentsCapacity <= pl::kMaxRecentsCapacity);

// Sidebar state is a constexpr-friendly value with a default per the design review.
static_assert(lay::SidebarState{}.visible);
static_assert(lay::SidebarState{}.width == lay::kSidebarDefaultWidth);
static_assert(!lay::SidebarState{}.is_collapsed(lay::SidebarSection::Storage));
static_assert(lay::SidebarState{} == lay::SidebarState{});
static_assert(lay::kSidebarMinWidth < lay::kSidebarDefaultWidth);

// Stores are values (the ui hands copies to a background writer).
static_assert(std::is_copy_constructible_v<pl::BookmarkStore>);
static_assert(std::is_copy_constructible_v<pl::RecentsStore>);
static_assert(std::is_default_constructible_v<pi::ExpandContext>);

VolumeInfo usb_stick(const char* mount_point) {
    VolumeInfo volume;
    volume.mount_point = mount_point;
    volume.device = "/dev/sdb1";
    volume.fs_type = "vfat";
    volume.label = "STICK";
    volume.uuid = "1234-ABCD";
    volume.root = "/";
    volume.removable = true;
    volume.kind = VolumeKind::Removable;
    return volume;
}

// "A bookmark on an unmounted volume is not lost and shows offline; it resyncs on return."
std::size_t use_bookmarks(const Path& config_root) {
    std::size_t total = 0;
    std::vector<VolumeInfo> volumes{usb_stick("/media/u/STICK")};
    const Timestamp now{};

    pl::BookmarkStore store;
    const std::optional<VolumeIdentity> id = pl::identity_for("/media/u/STICK/photos", volumes);
    if (id.has_value()) {
        total += volume_matches(*id, volumes.front()) ? 1U : 0U;
        total += identity_of(volumes.front()) == *id ? 1U : 0U;
    }
    const Result<pl::BookmarkId> photos = store.add("/media/u/STICK/photos", id, now);
    const Result<pl::BookmarkId> home = store.add("/home/u/", std::nullopt, now, "Ev", 0);
    if (!photos || !home) {
        return 0;
    }
    if (Result<pl::BookmarkId> dup = store.add("/home/u", std::nullopt, now); !dup) {
        total += dup.error().code == ErrorCode::AlreadyExists ? 1U : 0U;
    }
    (void)store.rename(*photos, "Photos");
    (void)store.move(*photos, 0);
    total += store.contains("/home/u") ? 1U : 0U;
    total += store.index_of(*home).value_or(0);
    if (const pl::Bookmark* found = store.find(*photos)) {
        total += pl::default_label(found->path).size() + found->name.size();
    }

    // Unplugged: Offline, still listed.
    const std::vector<VolumeInfo> none;
    for (const pl::Bookmark& bookmark : store.list()) {
        const pl::ResolvedLocation where =
            pl::resolve_location(bookmark.path, bookmark.volume, none);
        total += where.status == pl::LocationStatus::Offline ? 1U : 0U;
    }
    // Replugged at another mount point: Online, rebased; resync rewrites the stored path.
    volumes.front().mount_point = "/media/u/STICK1";
    if (const pl::Bookmark* found = store.find(*photos)) {
        const pl::ResolvedLocation where =
            pl::resolve_location(found->path, found->volume, volumes);
        total += where.status == pl::LocationStatus::Online ? 1U : 0U;
        total += where.volume_index.value_or(0) + where.path.native().size();
    }
    total += store.resync(volumes);

    if (Result<std::string> text = pl::bookmarks_to_json(store)) {
        if (Result<pl::BookmarkStore> back = pl::bookmarks_from_json(*text)) {
            total += back->size();
        }
    }
    const Path file = pl::bookmarks_file(config_root);
    if (Result<void> saved = pl::save_bookmarks(file, store); !saved) {
        total += saved.error().message.size();
    }
    if (Result<pl::BookmarkStore> loaded = pl::load_bookmarks(file); !loaded) {
        total += loaded.error().code == ErrorCode::NotFound ? 1U : 0U;
    }
    if (Result<pl::Bookmark> removed = store.remove(*home)) {
        total += removed->id.value;
    }
    total += store.size() + (store.empty() ? 1U : 0U);

    // Highlight the row containing the active folder.
    const std::array<Path, 3> rows{Path("/"), Path("/home/u"), Path("/home/u/Music")};
    total += pl::deepest_containing("/home/u/Music/jazz", rows).value_or(0);
    total += pl::containing_volume("/media/u/STICK1/x", volumes).value_or(0);
    total += pl::is_tracked_volume(volumes.front()) ? 1U : 0U;
    return total;
}

std::size_t use_recents(const Path& config_root) {
    std::size_t total = 0;
    pl::RecentsStore recents(8);
    const Timestamp now{};
    total += recents.record("/home/u/Downloads", std::nullopt, now) ? 1U : 0U;
    total += recents.record("/home/u/Downloads/", std::nullopt, now) ? 1U : 0U;
    if (const pl::RecentEntry* entry = recents.find("/home/u/Downloads")) {
        total += entry->visit_count;
    }
    pl::RecentsOptions options = recents.options();
    options.enabled = false; // recents.toggle_tracking
    recents.set_options(options);
    total += recents.record("/tmp", std::nullopt, now) ? 0U : 1U;
    for (const pl::RecentEntry& entry : recents.entries()) {
        total += entry.path.native().size();
    }
    total += recents.resync(std::vector<VolumeInfo>{});
    total += recents.remove("/nope") ? 0U : 1U;
    if (Result<std::string> text = pl::recents_to_json(recents)) {
        if (Result<pl::RecentsStore> back = pl::recents_from_json(*text, 100)) {
            total += back->capacity();
        }
    }
    const Path file = pl::recents_file(config_root);
    (void)pl::save_recents(file, recents);
    if (Result<pl::RecentsStore> loaded = pl::load_recents(file)) {
        total += loaded->size();
    }
    recents.clear(); // recents.clear
    return total + recents.size() + (recents.empty() ? 1U : 0U);
}

std::size_t use_places() {
    std::size_t total = 0;
    if (Result<platform::UserDirs> dirs = platform::parse_user_dirs(
            "XDG_DOWNLOAD_DIR=\"$HOME/\xC4\xB0ndirilenler\"\nXDG_MUSIC_DIR=\"$HOME\"\n",
            "/home/u")) {
        for (const pl::Place& place : pl::build_places("/home/u", *dirs)) {
            total += pl::label_key(place.kind).size() + place.path.native().size();
        }
        total += dirs->desktop.empty() ? 0U : 1U;
    }
    if (Result<platform::UserDirs> read = platform::read_user_dirs("/home/u", "/home/u/.config");
        !read) {
        total += read.error().message.size();
    }
    if (Result<Path> downloads = platform::user_dir(platform::UserDir::Downloads)) {
        total += downloads->native().size();
    }
    if (Result<Path> other = platform::user_home("root")) {
        total += other->native().size();
    }
    return total;
}

pi::ExpandContext test_context(const std::map<std::string, std::string, std::less<>>& env) {
    pi::ExpandContext ctx;
    ctx.home = "/home/u";
    ctx.cwd = "/home/u/src";
    ctx.env = [&env](std::string_view name) -> std::optional<std::string> {
        if (const auto it = env.find(name); it != env.end()) {
            return it->second;
        }
        return std::nullopt;
    };
    ctx.user_home = [](std::string_view user) -> std::optional<Path> {
        if (user == "root") {
            return Path("/root");
        }
        return std::nullopt;
    };
    ctx.percent_variables = true;
    return ctx;
}

// "GoTo jumps to ~/Downloads and /."
std::size_t use_goto(const std::stop_token& stop) {
    std::size_t total = 0;
    const std::map<std::string, std::string, std::less<>> env{{"HOME", "/home/u"}};
    const pi::ExpandContext ctx = test_context(env);

    for (const std::string_view input : {"~/Downloads/", "/", "$HOME/Music", "${HOME}//x/./y",
                                         "%HOME%\n", "'/tmp/a b'", "..", "$NOPE/x"}) {
        if (Result<pi::ExpandedPath> expanded = pi::expand_user_path(input, ctx)) {
            total += expanded->path.native().size() + expanded->unresolved.size();
        }
    }
    total += pi::normalize_absolute("/a//b/../c/").native().size();
    if (Result<pi::CaseFix> fixed = pi::fix_path_case("/HOME/U/downloads", stop)) {
        total += (fixed->changed ? 1U : 0U) + (fixed->exists ? 1U : 0U);
    }
    if (Result<pi::ResolvedInput> target = pi::resolve_input("~/downloads", ctx, stop)) {
        total += target->exists() ? 1U : 0U;
        total += target->is_directory() ? 1U : 0U;
        total += target->case_corrected ? 1U : 0U;
        if (const std::optional<fs::DirEntry>& entry = target->entry; entry.has_value()) {
            total += entry->name.size();
        }
    }

    // Completion: cached listing on the UI thread, or the blocking helper on a worker.
    if (Result<pi::CompletionQuery> query = pi::parse_completion_input("~/Dow", ctx)) {
        std::vector<fs::DirEntry> listing(1);
        listing.front().name = "Downloads";
        listing.front().kind = fs::EntryKind::Directory;
        pi::CompletionOptions options;
        options.max_results = 16;
        for (const pi::Completion& c : pi::match_completions(*query, listing, options)) {
            total += c.text.size() + c.name.size() + c.path.native().size();
            total += c.match == pi::CompletionMatch::Prefix ? 1U : 0U;
            total += (c.is_directory ? 1U : 0U) + (c.is_symlink ? 1U : 0U);
        }
        total += query->typed_dir.size() + query->prefix.size() + query->dir.native().size();
    }
    if (Result<std::vector<pi::Completion>> list = pi::complete_path("/us", ctx, {}, stop)) {
        total += list->size();
    }

    // Copy as Path.
    const std::array<Path, 2> paths{Path("/tmp/a b"), Path("/tmp/it's")};
    total += pi::format_paths_for_copy(paths).size();
    total += pi::format_paths_for_copy(Span<const Path>(paths.data(), 1)).size();
    total += pi::format_paths_for_copy(paths, pi::PathQuoting::Never).size();
    total += pi::quote_for_shell("plain/path").size();
    return total;
}

std::size_t use_sidebar_session() {
    lay::Session session;
    session.sidebar.visible = false;
    session.sidebar.width = 260.0;
    session.sidebar.set_collapsed(lay::SidebarSection::Recents, true);
    std::size_t total = session.sidebar.is_collapsed(lay::SidebarSection::Recents) ? 1U : 0U;
    if (Result<std::string> text = lay::to_json(session)) {
        if (Result<lay::Session> back = lay::session_from_json(*text)) {
            total += back->sidebar == session.sidebar ? 1U : 0U;
        }
    }
    return total + lay::kSidebarSectionCount;
}

void enqueue_phase4(CommandQueue& queue) {
    queue.enqueue(CommandId::ToggleSidebar);
    queue.enqueue(CommandId::GoTo);
    queue.enqueue(CommandId::FocusAddressBar);
    queue.enqueue(CommandId::ToggleBookmark);
    queue.enqueue(CommandId::ToggleBookmark, pl::BookmarkId{4}.value);
    queue.enqueue(CommandId::CopyPath, CopyPathArg::CurrentFolder);
    queue.enqueue(CommandId::ClearRecents);
}

} // namespace navigator::contracts_check
