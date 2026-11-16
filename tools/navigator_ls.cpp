// navigator-ls: prints a directory through navigator::fs and, with --watch, keeps the listing in
// sync with inotify events. --volumes prints the volume table. Phase 1 integration smoke tool.
#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>
#include <string_view>
#include <vector>

#include <navigator/fs.hpp>
#include <navigator/types.hpp>
#include <navigator/volume.hpp>
#include <navigator/watch.hpp>

namespace {

namespace fs = navigator::fs;

volatile std::sig_atomic_t g_stop = 0;

extern "C" void on_signal(int /*sig*/) { g_stop = 1; }

struct Options {
    navigator::Path path = ".";
    fs::ListOptions list;
    fs::SortOptions sort;
    bool watch = false;
    bool volumes = false;
    bool help = false;
    int watch_seconds = 0; // 0 = until Ctrl+C
};

void print_usage() {
    std::puts("usage: navigator-ls [options] [PATH]\n"
              "  -a, --all            show hidden (dot) files\n"
              "  -L, --dereference    describe symlink targets instead of the links\n"
              "  -s, --sort KEY       name | size | time | ext (default: name)\n"
              "  -r, --reverse        descending order\n"
              "      --mixed          do not group folders first\n"
              "  -w, --watch          keep the listing in sync with filesystem changes\n"
              "      --seconds N      with --watch: exit after N seconds\n"
              "      --volumes        list mounted volumes instead of a directory\n"
              "  -h, --help           this help");
}

bool parse_args(int argc, char** argv, Options& o) {
    const std::vector<std::string_view> args(argv + 1, argv + argc);
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view a = args[i];
        if (a == "-a" || a == "--all") {
            o.list.show_hidden = true;
        } else if (a == "-L" || a == "--dereference") {
            o.list.follow_symlinks = true;
        } else if (a == "-r" || a == "--reverse") {
            o.sort.descending = true;
        } else if (a == "--mixed") {
            o.sort.folders_first = false;
        } else if (a == "-w" || a == "--watch") {
            o.watch = true;
        } else if (a == "--volumes") {
            o.volumes = true;
        } else if (a == "-h" || a == "--help") {
            o.help = true;
        } else if ((a == "-s" || a == "--sort") && i + 1 < args.size()) {
            const std::string_view k = args[++i];
            if (k == "name") {
                o.sort.key = fs::SortKey::Name;
            } else if (k == "size") {
                o.sort.key = fs::SortKey::Size;
            } else if (k == "time") {
                o.sort.key = fs::SortKey::Modified;
            } else if (k == "ext") {
                o.sort.key = fs::SortKey::Extension;
            } else {
                std::fprintf(stderr, "navigator-ls: unknown sort key '%.*s'\n",
                             static_cast<int>(k.size()), k.data());
                return false;
            }
        } else if (a == "--seconds" && i + 1 < args.size()) {
            o.watch_seconds = std::stoi(std::string(args[++i]));
        } else if (!a.empty() && a.front() == '-') {
            std::fprintf(stderr, "navigator-ls: unknown option '%.*s' (try --help)\n",
                         static_cast<int>(a.size()), a.data());
            return false;
        } else {
            o.path = navigator::Path(std::string(a));
        }
    }
    return true;
}

std::string human_size(std::uint64_t bytes) {
    static constexpr std::string_view units[] = {"B", "K", "M", "G", "T", "P"};
    auto value = static_cast<double>(bytes);
    std::size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < std::size(units)) {
        value /= 1024.0;
        ++unit;
    }
    char buf[32];
    if (unit == 0) {
        std::snprintf(buf, sizeof buf, "%llu%s", static_cast<unsigned long long>(bytes),
                      units[unit].data());
    } else {
        std::snprintf(buf, sizeof buf, "%.1f%s", value, units[unit].data());
    }
    return buf;
}

char kind_char(const fs::DirEntry& e) {
    switch (e.kind) {
    case fs::EntryKind::Directory: return 'd';
    case fs::EntryKind::Symlink: return 'l';
    case fs::EntryKind::File: return '-';
    case fs::EntryKind::Other: return 'o';
    case fs::EntryKind::Unknown: break;
    }
    return '?';
}

std::string format_time(navigator::Timestamp t) {
    if (t == navigator::Timestamp{}) {
        return "                ";
    }
    const std::time_t secs = std::chrono::system_clock::to_time_t(
        std::chrono::time_point_cast<std::chrono::system_clock::duration>(t));
    std::tm tm{};
    localtime_r(&secs, &tm);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", &tm);
    return buf;
}

void print_listing(std::vector<fs::DirEntry>& entries, const Options& o) {
    fs::sort_entries(navigator::Span<fs::DirEntry>(entries), o.sort);
    for (const auto& e : entries) {
        std::string name = fs::to_display_name(e.name);
        if (fs::is_directory_like(e)) {
            name += '/';
        }
        if (e.kind == fs::EntryKind::Symlink && e.dangling) {
            name += "  (dangling)";
        }
        if (e.metadata_error) {
            name += "  [";
            name += navigator::to_string(*e.metadata_error);
            name += ']';
        }
        std::printf("%c %8s  %s  %s\n", kind_char(e), human_size(e.size).c_str(),
                    format_time(e.mtime).c_str(), name.c_str());
    }
    std::printf("%zu entries\n", entries.size());
}

int report(const navigator::Error& err, std::string_view what) {
    std::fprintf(stderr, "navigator-ls: %.*s: %.*s (%s)\n", static_cast<int>(what.size()),
                 what.data(), static_cast<int>(err.message.size()), err.message.data(),
                 std::string(navigator::to_string(err.code)).c_str());
    return 1;
}

std::string_view kind_name(navigator::VolumeKind k) {
    using navigator::VolumeKind;
    switch (k) {
    case VolumeKind::Root: return "root";
    case VolumeKind::Home: return "home";
    case VolumeKind::Internal: return "internal";
    case VolumeKind::Removable: return "removable";
    case VolumeKind::Network: return "network";
    case VolumeKind::Virtual: return "virtual";
    case VolumeKind::Other: break;
    }
    return "other";
}

int run_volumes() {
    auto volumes = navigator::list_volumes();
    if (!volumes) {
        return report(volumes.error(), "list_volumes");
    }
    std::printf("%-10s %-8s %8s %8s  %-3s %s\n", "KIND", "FS", "SIZE", "AVAIL", "RW",
                "MOUNT POINT (LABEL)");
    for (const auto& v : *volumes) {
        std::string where = v.mount_point.string();
        if (!v.label.empty()) {
            where += "  (" + v.label + ")";
        }
        std::printf("%-10s %-8s %8s %8s  %-3s %s%s\n", std::string(kind_name(v.kind)).c_str(),
                    v.fs_type.c_str(), v.has_space ? human_size(v.space.total_bytes).c_str() : "-",
                    v.has_space ? human_size(v.space.available_bytes).c_str() : "-",
                    v.read_only ? "ro" : "rw", where.c_str(), v.removable ? "  [removable]" : "");
    }
    return 0;
}

std::string_view event_name(navigator::WatchEventKind k) {
    using navigator::WatchEventKind;
    switch (k) {
    case WatchEventKind::Created: return "created";
    case WatchEventKind::Deleted: return "deleted";
    case WatchEventKind::Modified: return "modified";
    case WatchEventKind::Attrib: return "attrib";
    case WatchEventKind::MovedFrom: return "moved-out";
    case WatchEventKind::MovedTo: return "moved-in";
    case WatchEventKind::Renamed: return "renamed";
    case WatchEventKind::Overflow: return "overflow";
    case WatchEventKind::WatchedDirGone: return "directory-gone";
    }
    return "?";
}

int run_watch(const Options& o) {
    auto watcher = navigator::create_inotify_watcher();
    if (!watcher) {
        return report(watcher.error(), "watcher");
    }
    auto handle = (*watcher)->add(o.path);
    if (!handle) {
        return report(handle.error(), o.path.string());
    }
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(o.watch_seconds);
    std::vector<navigator::WatchEvent> events;
    while (g_stop == 0) {
        if (o.watch_seconds > 0 && std::chrono::steady_clock::now() >= deadline) {
            break;
        }
        if (!(*watcher)->wait(std::chrono::milliseconds(200))) {
            continue;
        }
        events.clear();
        if (auto n = (*watcher)->poll(events); !n) {
            return report(n.error(), "poll");
        }
        bool gone = false;
        for (const auto& ev : events) {
            if (ev.kind == navigator::WatchEventKind::Renamed) {
                std::printf("-- %s: %s -> %s\n", std::string(event_name(ev.kind)).c_str(),
                            fs::to_display_name(ev.old_name).c_str(),
                            fs::to_display_name(ev.name).c_str());
            } else {
                std::printf("-- %s: %s\n", std::string(event_name(ev.kind)).c_str(),
                            fs::to_display_name(ev.name).c_str());
            }
            gone = gone || ev.kind == navigator::WatchEventKind::WatchedDirGone;
        }
        if (gone) {
            std::puts("navigator-ls: watched directory is gone");
            return 0;
        }
        // Simple and always correct: re-list after each batch (Phase 2 applies events
        // incrementally to the view model).
        auto entries = fs::list_directory(o.path, o.list);
        if (!entries) {
            return report(entries.error(), o.path.string());
        }
        print_listing(*entries, o);
        std::fflush(stdout);
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    Options o;
    if (!parse_args(argc, argv, o)) {
        return 2;
    }
    if (o.help) {
        print_usage();
        return 0;
    }
    if (o.volumes) {
        return run_volumes();
    }
    auto entries = fs::list_directory(o.path, o.list);
    if (!entries) {
        return report(entries.error(), o.path.string());
    }
    print_listing(*entries, o);
    std::fflush(stdout);
    return o.watch ? run_watch(o) : 0;
}
