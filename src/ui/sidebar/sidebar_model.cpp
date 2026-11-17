// src/ui/sidebar/sidebar_model.cpp — see sidebar_model.hpp.
#include "sidebar/sidebar_model.hpp"

#include <algorithm>

#include <navigator/fs.hpp>
#include <navigator/i18n.hpp>
#include <navigator/path_input.hpp>

namespace navigator::ui {
namespace {

using layout::SidebarSection;
using places::LocationStatus;

constexpr std::size_t kNpos = static_cast<std::size_t>(-1);

void fill_volume(SidebarRow& row, const VolumeInfo& v, std::size_t index) {
    row.volume = index;
    row.volume_kind = v.kind;
    row.removable = v.removable || v.kind == VolumeKind::Removable;
    row.network = v.kind == VolumeKind::Network;
    row.read_only = v.read_only;
    if (v.has_space && (v.space.total_bytes > 0 || v.space.available_bytes > 0)) {
        row.usage = used_fraction(v.space);
        row.total_bytes = v.space.total_bytes;
        row.available_bytes = v.space.available_bytes;
    }
}

SidebarRow header(SidebarSection s, const layout::SidebarState& state, std::string_view key) {
    SidebarRow h;
    h.kind = SidebarRowKind::Header;
    h.section = s;
    h.label = std::string(tr(key));
    h.collapsed = state.is_collapsed(s);
    return h;
}

} // namespace

std::string volume_label(const VolumeInfo& volume) {
    if (!volume.label.empty()) {
        return volume.label;
    }
    if (volume.kind == VolumeKind::Network && !volume.device.empty()) {
        return fs::to_display_name(volume.device);
    }
    return places::default_label(volume.mount_point);
}

std::vector<SidebarRow> build_sidebar_rows(const PlacesModel& model,
                                           const layout::SidebarState& state,
                                           const Path& active_folder, std::size_t max_recents) {
    std::vector<SidebarRow> rows;
    const std::vector<VolumeInfo>& volumes = model.volumes();

    // Bookmarks.
    {
        SidebarRow h = header(SidebarSection::Bookmarks, state, "sidebar.bookmarks");
        h.count = model.bookmarks().size();
        rows.push_back(std::move(h));
        if (!state.is_collapsed(SidebarSection::Bookmarks)) {
            for (const places::Bookmark& b : model.bookmarks().list()) {
                SidebarRow r;
                r.kind = SidebarRowKind::Bookmark;
                r.section = SidebarSection::Bookmarks;
                r.bookmark = b.id;
                r.stored_path = b.path;
                r.status = model.status(b.path, b.volume, &r.path);
                r.label = b.name.empty() ? places::default_label(b.path) : b.name;
                if (r.status != LocationStatus::Offline) {
                    if (const std::optional<std::size_t> v =
                            places::containing_volume(r.path, volumes);
                        v) {
                        r.network = volumes[*v].kind == VolumeKind::Network;
                        r.removable = volumes[*v].removable;
                    }
                }
                rows.push_back(std::move(r));
            }
            if (model.bookmarks().empty()) {
                SidebarRow e;
                e.kind = SidebarRowKind::Empty;
                e.section = SidebarSection::Bookmarks;
                e.label = std::string(tr("sidebar.bookmarks_empty"));
                rows.push_back(std::move(e));
            }
        }
    }

    // Recents.
    {
        SidebarRow h = header(SidebarSection::Recents, state, "sidebar.recents");
        h.count = model.recents().size();
        h.tracking_off = !model.recents().options().enabled;
        rows.push_back(std::move(h));
        if (!state.is_collapsed(SidebarSection::Recents)) {
            std::size_t shown = 0;
            for (const places::RecentEntry& e : model.recents().entries()) {
                if (shown >= max_recents) {
                    break;
                }
                SidebarRow r;
                r.kind = SidebarRowKind::Recent;
                r.section = SidebarSection::Recents;
                r.stored_path = e.path;
                r.status = model.status(e.path, e.volume, &r.path);
                r.label = places::default_label(e.path);
                rows.push_back(std::move(r));
                ++shown;
            }
            if (model.recents().empty()) {
                SidebarRow e;
                e.kind = SidebarRowKind::Empty;
                e.section = SidebarSection::Recents;
                e.tracking_off = !model.recents().options().enabled;
                e.label = std::string(
                    tr(e.tracking_off ? "sidebar.recents_off" : "sidebar.recents_empty"));
                rows.push_back(std::move(e));
            }
        }
    }

    // Places.
    std::optional<std::size_t> root_volume;
    std::optional<std::size_t> home_volume;
    for (std::size_t i = 0; i < volumes.size(); ++i) {
        if (volumes[i].kind == VolumeKind::Root && !root_volume) {
            root_volume = i;
        } else if (volumes[i].kind == VolumeKind::Home && !home_volume) {
            home_volume = i;
        }
    }
    {
        SidebarRow h = header(SidebarSection::Places, state, "sidebar.places");
        const std::size_t header_at = rows.size();
        rows.push_back(std::move(h));
        std::size_t count = 0;
        for (const places::Place& p : model.places()) {
            if (model.missing(p.path) && p.kind != places::PlaceKind::Home &&
                p.kind != places::PlaceKind::FileSystem) {
                continue; // a configured XDG dir that does not exist
            }
            ++count;
            if (state.is_collapsed(SidebarSection::Places)) {
                continue;
            }
            SidebarRow r;
            r.kind = SidebarRowKind::Place;
            r.section = SidebarSection::Places;
            r.place = p.kind;
            r.path = p.path;
            r.stored_path = p.path;
            r.label = std::string(tr(places::label_key(p.kind)));
            const std::optional<std::size_t> v = p.kind == places::PlaceKind::FileSystem
                                                     ? root_volume
                                                 : p.kind == places::PlaceKind::Home ? home_volume
                                                                                     : std::nullopt;
            if (v) {
                fill_volume(r, volumes[*v], *v);
            }
            rows.push_back(std::move(r));
        }
        rows[header_at].count = count;
    }

    // Storage.
    {
        SidebarRow h = header(SidebarSection::Storage, state, "sidebar.storage");
        const std::size_t header_at = rows.size();
        rows.push_back(std::move(h));
        std::size_t count = 0;
        for (std::size_t i = 0; i < volumes.size(); ++i) {
            const VolumeInfo& v = volumes[i];
            if (v.kind == VolumeKind::Root || v.kind == VolumeKind::Home ||
                v.kind == VolumeKind::Virtual) {
                continue;
            }
            ++count;
            if (state.is_collapsed(SidebarSection::Storage)) {
                continue;
            }
            SidebarRow r;
            r.kind = SidebarRowKind::Volume;
            r.section = SidebarSection::Storage;
            r.path = v.mount_point;
            r.stored_path = v.mount_point;
            r.label = volume_label(v);
            fill_volume(r, v, i);
            rows.push_back(std::move(r));
        }
        rows[header_at].count = count;
        if (count == 0) {
            rows.pop_back(); // no removable or extra volumes: no Storage section at all
        }
    }

    // Highlight the deepest visible row containing the active folder.
    if (!active_folder.empty()) {
        std::vector<Path> roots;
        std::vector<std::size_t> index;
        for (std::size_t i = 0; i < rows.size(); ++i) {
            const SidebarRow& r = rows[i];
            if ((r.kind == SidebarRowKind::Bookmark || r.kind == SidebarRowKind::Place ||
                 r.kind == SidebarRowKind::Volume) &&
                r.online() && !r.path.empty()) {
                roots.push_back(r.path);
                index.push_back(i);
            }
        }
        if (const std::optional<std::size_t> w =
                places::deepest_containing(path_input::normalize_absolute(active_folder), roots);
            w && *w < index.size()) {
            rows[index[*w]].highlighted = true;
        }
    }
    return rows;
}

std::size_t sidebar_focus_step(const std::vector<SidebarRow>& rows, std::size_t from,
                               std::ptrdiff_t delta) {
    if (rows.empty()) {
        return kNpos;
    }
    const auto n = static_cast<std::ptrdiff_t>(rows.size());
    std::ptrdiff_t at = 0;
    if (from == kNpos || from >= rows.size()) {
        at = delta >= 0 ? -1 : n;
    } else {
        at = static_cast<std::ptrdiff_t>(from);
    }
    const std::ptrdiff_t dir = delta >= 0 ? 1 : -1;
    std::ptrdiff_t steps = delta == 0 ? 0 : (delta > 0 ? delta : -delta);
    std::ptrdiff_t last_ok = (from != kNpos && from < rows.size() && rows[from].focusable())
                                 ? static_cast<std::ptrdiff_t>(from)
                                 : -1;
    std::ptrdiff_t i = at;
    while (steps > 0) {
        i += dir;
        if (i < 0 || i >= n) {
            break;
        }
        if (rows[static_cast<std::size_t>(i)].focusable()) {
            last_ok = i;
            --steps;
        }
    }
    return last_ok < 0 ? kNpos : static_cast<std::size_t>(last_ok);
}

std::size_t sidebar_initial_focus(const std::vector<SidebarRow>& rows) {
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (rows[i].highlighted) {
            return i;
        }
    }
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (rows[i].focusable()) {
            return i;
        }
    }
    return kNpos;
}

std::size_t sidebar_row_of(const std::vector<SidebarRow>& rows, places::BookmarkId id) {
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (rows[i].kind == SidebarRowKind::Bookmark && rows[i].bookmark == id) {
            return i;
        }
    }
    return kNpos;
}

std::size_t sidebar_bookmark_index(const std::vector<SidebarRow>& rows, std::size_t row) {
    std::size_t index = 0;
    for (std::size_t i = 0; i < rows.size() && i < row; ++i) {
        if (rows[i].kind == SidebarRowKind::Bookmark) {
            ++index;
        }
    }
    return index;
}

FocusStop cycle_focus(std::size_t pane_count, bool sidebar_visible, bool sidebar_focused,
                      std::size_t active_pane, int delta) {
    if (pane_count == 0) {
        return FocusStop{sidebar_visible, 0};
    }
    const std::size_t extra = sidebar_visible ? 1 : 0;
    const std::size_t stops = pane_count + extra;
    std::size_t at = 0;
    if (sidebar_visible && sidebar_focused) {
        at = 0;
    } else {
        at = extra + std::min(active_pane, pane_count - 1);
    }
    const std::size_t step = delta >= 0 ? 1 : stops - 1;
    const std::size_t next = (at + step) % stops;
    if (sidebar_visible && next == 0) {
        return FocusStop{true, std::min(active_pane, pane_count - 1)};
    }
    return FocusStop{false, next - extra};
}

} // namespace navigator::ui
