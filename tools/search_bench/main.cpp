// search_bench: Phase 5 search benchmark (docs/perf/phase5-search.md).
//
// Works in a fresh directory that it creates inside DIR (default: the temporary directory) and
// removes only that directory. Never touches system settings: the page cache is warm.
//   1. filter: filter_entries over synthetic listings (1k / 20k / 100k / 200k names), 1-, 3- and
//      8-character queries, Substring and Fuzzy;
//   2. walk:   on a generated tree (default about 300k entries, ~30k directories, depth up to 12,
//      ASCII / Turkish / NFD names, hidden and a few unreadable directories): fs::Walker
//      throughput, time to first batch, cancel latency;
//   3. index:  on the same tree: cold scan, save, file size, restart (load + verify) versus a
//      walk of the tree, query latency p50 / p95;
//   4. million: 1M synthetic entries (real directories, names only in the index file): load,
//      memory, query latency.
// Usage: search_bench [--entries N] [--dir DIR] [--only filter|walk|index|million]
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <malloc.h>
#include <sys/stat.h>
#include <unistd.h>

#include <navigator/fs.hpp>
#include <navigator/index.hpp>
#include <navigator/search.hpp>
#include <navigator/walk.hpp>

#include "index_tree.hpp"

namespace stdfs = std::filesystem;
using namespace navigator;
using Clock = std::chrono::steady_clock;

namespace {

double ms_since(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

double percentile(std::vector<double> v, double p) {
    if (v.empty()) {
        return 0;
    }
    std::sort(v.begin(), v.end());
    const auto idx = static_cast<std::size_t>(std::lround(p * static_cast<double>(v.size() - 1)));
    return v[std::min(idx, v.size() - 1)];
}

const std::vector<std::string> kStems = {
    "report", "Rapor",  "İstanbul", "ıslak",  "çiçek", "notes",   "e\xcc\x81te", "data",
    "Foto",   "README", "src",      "build",  "Kayıt", "photo",   "myFile",      "a_b-c",
    "Şehir",  "ğüzel",  "index",    "config", "main",  "invoice", "backup",      "Belge"};
const std::vector<std::string> kExt = {".txt",    ".md",  ".JPG", ".png", "",
                                       ".tar.gz", ".cpp", ".PDF", ".hpp", ".json"};

std::string make_name(std::mt19937& rng, std::uint64_t counter) {
    std::string n = kStems[rng() % kStems.size()];
    n += std::to_string(counter);
    n += kExt[rng() % kExt.size()];
    return n;
}

struct Generated {
    std::uint64_t entries = 0;
    std::uint64_t dirs = 0;
};

void touch(const stdfs::path& p) {
    const int fd = ::open(p.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd >= 0) {
        ::close(fd);
    }
}

Generated generate(const stdfs::path& root, std::uint64_t target) {
    std::mt19937 rng(20251117);
    Generated g;
    const std::uint64_t dir_target = std::max<std::uint64_t>(target / 10, 1);
    std::vector<stdfs::path> dirs{root};
    std::vector<std::uint32_t> depth{0};
    std::uint64_t counter = 0;
    while (g.dirs < dir_target) {
        // Prefer recent directories so the tree gets deep (up to 12).
        const std::size_t parent =
            (rng() % 3 == 0) ? rng() % dirs.size()
                             : dirs.size() - 1 - (rng() % std::min<std::size_t>(dirs.size(), 64));
        if (depth[parent] >= 12) {
            continue;
        }
        std::string name = kStems[rng() % kStems.size()] + "_" + std::to_string(counter++);
        if (rng() % 50 == 0) {
            name.insert(0, ".");
        }
        const stdfs::path p = dirs[parent] / name;
        ::mkdir(p.c_str(), 0755);
        dirs.push_back(p);
        depth.push_back(depth[parent] + 1);
        ++g.dirs;
        ++g.entries;
    }
    while (g.entries < target) {
        const stdfs::path& d = dirs[rng() % dirs.size()];
        std::string name = make_name(rng, counter++);
        if (rng() % 20 == 0) {
            name.insert(0, ".");
        }
        touch(d / name);
        ++g.entries;
    }
    // Age every directory so restart verification does not hit the 2 s coarse-timestamp guard.
    struct timespec ts[2];
    ::clock_gettime(CLOCK_REALTIME, &ts[0]);
    ts[0].tv_sec -= 7200;
    ts[1] = ts[0];
    for (const stdfs::path& d : dirs) {
        ::utimensat(AT_FDCWD, d.c_str(), ts, AT_SYMLINK_NOFOLLOW);
    }
    // A few unreadable directories (skipped as "permission"), last: aging needs search access.
    for (std::size_t i = 1; i <= 5 && i < dirs.size(); ++i) {
        ::chmod(dirs[i * (dirs.size() / 6)].c_str(), 0);
    }
    return g;
}

// --------------------------------------------------------------------------------------------
// 1. filter_entries
// --------------------------------------------------------------------------------------------

void bench_filter() {
    std::printf("\n## filter_entries (synthetic listings, median of runs)\n\n");
    std::printf("| entries | names | query | mode | median ms | hits |\n");
    std::printf("|---:|---|---|---|---:|---:|\n");
    std::mt19937 rng(7);
    for (const std::size_t n :
         {std::size_t{1000}, std::size_t{20000}, std::size_t{100000}, std::size_t{200000}}) {
        for (const bool ascii : {true, false}) {
            std::vector<fs::DirEntry> entries(n);
            for (std::size_t i = 0; i < n; ++i) {
                std::string name = make_name(rng, i);
                if (ascii) {
                    for (char& c : name) {
                        if ((static_cast<unsigned char>(c) & 0x80U) != 0) {
                            c = 'x';
                        }
                    }
                }
                entries[i].name = std::move(name);
                entries[i].kind = fs::EntryKind::File;
            }
            for (const char* q : {"r", "rep", "rapor12x"}) {
                for (const search::MatchMode mode :
                     {search::MatchMode::Substring, search::MatchMode::Fuzzy}) {
                    search::Matcher m(search::compile_query(q, mode));
                    std::vector<search::FilterHit> out;
                    std::vector<double> times;
                    const int runs = n <= 20000 ? 51 : 11;
                    for (int r = 0; r < runs; ++r) {
                        const auto t0 = Clock::now();
                        (void)search::filter_entries(entries, m, search::TypeFilter{}, out);
                        times.push_back(ms_since(t0));
                    }
                    std::printf("| %zu | %s | `%s` | %s | %.3f | %zu |\n", n,
                                ascii ? "ASCII" : "mixed", q,
                                std::string(search::to_string(mode)).c_str(),
                                percentile(times, 0.5), out.size());
                }
            }
        }
    }
}

// --------------------------------------------------------------------------------------------
// 2. Walker
// --------------------------------------------------------------------------------------------

struct WalkTiming {
    double total_ms = 0;
    double first_batch_ms = 0;
    std::uint64_t entries = 0;
    std::uint64_t skipped_permission = 0;
};

WalkTiming walk_once(const stdfs::path& root, bool metadata) {
    fs::WalkOptions o;
    o.root = root;
    o.metadata = metadata;
    o.show_hidden = true;
    o.max_entries = 0;
    WalkTiming t;
    const auto t0 = Clock::now();
    auto w = fs::Walker::open(o);
    if (!w) {
        std::printf("walker open failed: %s\n", w.error().message.c_str());
        return t;
    }
    fs::WalkBatch batch;
    bool first = true;
    while (true) {
        auto more = w->next_batch(batch, {});
        if (!more) {
            break;
        }
        if (first && !batch.entries.empty()) {
            t.first_batch_ms = ms_since(t0);
            first = false;
        }
        t.entries += batch.entries.size();
        if (!*more) {
            break;
        }
    }
    t.total_ms = ms_since(t0);
    t.skipped_permission = w->stats().skipped_permission;
    return t;
}

void bench_walk(const stdfs::path& root) {
    std::printf("\n## fs::Walker (whole generated tree, show_hidden)\n\n");
    std::printf("| metadata | cache | total ms | entries/s | first batch ms | entries | "
                "skipped (permission) |\n|---|---|---:|---:|---:|---:|---:|\n");
    for (const bool metadata : {false, true}) {
        (void)walk_once(root, metadata); // warm up
        const WalkTiming w = walk_once(root, metadata);
        std::printf("| %s | warm | %.1f | %.0f | %.2f | %llu | %llu |\n",
                    metadata ? "true" : "false", w.total_ms,
                    static_cast<double>(w.entries) / (w.total_ms / 1000.0), w.first_batch_ms,
                    static_cast<unsigned long long>(w.entries),
                    static_cast<unsigned long long>(w.skipped_permission));
    }
    // Cancel latency: request stop from another thread after a random delay.
    std::vector<double> lat;
    std::mt19937 rng(3);
    for (int i = 0; i < 40; ++i) {
        fs::WalkOptions o;
        o.root = root;
        o.metadata = (i % 2) == 0;
        o.show_hidden = true;
        o.max_entries = 0;
        auto w = fs::Walker::open(o);
        if (!w) {
            continue;
        }
        std::stop_source src;
        std::atomic<std::int64_t> requested_ns{0};
        const auto delay = std::chrono::microseconds(200 + (rng() % 20000));
        std::jthread stopper([&] {
            std::this_thread::sleep_for(delay);
            requested_ns = Clock::now().time_since_epoch().count();
            src.request_stop();
        });
        fs::WalkBatch batch;
        while (true) {
            auto more = w->next_batch(batch, src.get_token());
            if (!more || !*more) {
                break;
            }
        }
        stopper.join();
        const std::int64_t req = requested_ns.load();
        if (req != 0) {
            lat.push_back(static_cast<double>(Clock::now().time_since_epoch().count() - req) / 1e6);
        }
    }
    std::printf("\nCancel latency over %zu walks: p50 %.3f ms, p99 %.3f ms, max %.3f ms\n",
                lat.size(), percentile(lat, 0.5), percentile(lat, 0.99), percentile(lat, 1.0));
}

// --------------------------------------------------------------------------------------------
// 3. Index
// --------------------------------------------------------------------------------------------

bool wait_state(const search::IIndex& index, search::IndexState s,
                std::chrono::seconds timeout = std::chrono::seconds(1200)) {
    const auto until = Clock::now() + timeout;
    while (Clock::now() < until) {
        const search::IndexStatus st = index.status();
        if (st.state == s) {
            return true;
        }
        if (st.state == search::IndexState::Failed) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

void bench_queries(const search::IIndex& index, const char* label) {
    std::printf("\n### Query latency, %s (%llu entries, limit 1000, show_hidden false)\n\n", label,
                static_cast<unsigned long long>(index.status().entries));
    std::printf("| query | mode | p50 ms | p95 ms | matched |\n|---|---|---:|---:|---:|\n");
    for (const char* q : {"rep", "ist", "a", "rapor 12", "main.cpp", "zzzq"}) {
        for (const search::MatchMode mode :
             {search::MatchMode::Substring, search::MatchMode::Fuzzy}) {
            search::IndexQuery iq;
            iq.query = search::compile_query(q, mode);
            iq.limit = 1000;
            std::vector<double> times;
            std::uint64_t matched = 0;
            for (int r = 0; r < 21; ++r) {
                const auto t0 = Clock::now();
                auto res = index.query(iq, {});
                times.push_back(ms_since(t0));
                if (res) {
                    matched = res->matched;
                }
            }
            std::printf("| `%s` | %s | %.2f | %.2f | %llu |\n", q,
                        std::string(search::to_string(mode)).c_str(), percentile(times, 0.5),
                        percentile(times, 0.95), static_cast<unsigned long long>(matched));
        }
    }
}

search::NameIndexOptions bench_options(const stdfs::path& root, const stdfs::path& cache) {
    search::NameIndexOptions o;
    o.root = root;
    o.identity.device = "/dev/bench";
    o.identity.mount_point = root;
    o.file = search::index_file(cache, o.identity);
    o.rescan_interval = std::chrono::seconds(0);
    o.background_priority = false;
    return o;
}

// Starts an index and returns it once Ready (nullptr on failure), with the time that took.
std::unique_ptr<search::IIndex> ready_index(const search::NameIndexOptions& o, double& ms) {
    const auto t0 = Clock::now();
    auto index = search::create_name_index(o);
    if (!index) {
        return nullptr;
    }
    (*index)->start();
    if (!wait_state(**index, search::IndexState::Ready)) {
        return nullptr;
    }
    ms = ms_since(t0);
    return std::move(*index);
}

double resident_mb() {
    ::malloc_trim(0);
    std::ifstream f("/proc/self/statm");
    std::uint64_t size = 0;
    std::uint64_t resident = 0;
    f >> size >> resident;
    return static_cast<double>(resident * static_cast<std::uint64_t>(::sysconf(_SC_PAGESIZE))) /
           1e6;
}

void bench_index(const stdfs::path& root, const stdfs::path& cache) {
    std::printf("\n## Name index\n\n");
    const search::NameIndexOptions o = bench_options(root, cache);

    (void)walk_once(root, false); // same cache state for every measurement below
    double scan_ms = 0;
    auto index = ready_index(o, scan_ms);
    if (!index) {
        std::printf("index did not become ready\n");
        return;
    }
    const search::IndexStatus st1 = index->status();
    index->stop();

    auto bytes = search::detail::read_file(o.file);
    if (!bytes) {
        std::printf("no index file\n");
        return;
    }
    auto tree = search::detail::decode_index(*bytes, o.identity);
    if (!tree) {
        std::printf("index file does not decode\n");
        return;
    }
    std::vector<double> saves;
    for (int i = 0; i < 5; ++i) {
        const auto t0 = Clock::now();
        auto out = search::detail::encode_index(*tree, o.identity, tree->scan_started_at);
        if (!out || !search::detail::write_file_atomic(o.file, *out)) {
            std::printf("save failed\n");
            return;
        }
        saves.push_back(ms_since(t0));
    }
    tree->reset();

    // Alternate the two so that machine noise hits both alike.
    std::vector<double> restarts;
    std::vector<double> loads;
    std::vector<double> verifies;
    std::vector<double> walks;
    std::vector<double> scans{scan_ms};
    bool loaded = true;
    for (int i = 0; i < 7; ++i) {
        double ms = 0;
        index = ready_index(o, ms);
        if (!index) {
            std::printf("restart did not become ready\n");
            return;
        }
        const search::IndexStatus st = index->status();
        loaded = loaded && st.loaded_from_file;
        restarts.push_back(ms);
        loads.push_back(static_cast<double>(st.load_duration.count()));
        verifies.push_back(static_cast<double>(st.scan_duration.count()));
        if (i == 6) {
            bench_queries(*index, "300k generated tree, after restart");
        }
        index->stop();
        walks.push_back(walk_once(root, false).total_ms);
        if (i < 4) { // a scan without a file, i.e. without the save
            search::NameIndexOptions mem = o;
            mem.file.clear();
            if (auto scanned = ready_index(mem, ms)) {
                scans.push_back(ms);
            }
        }
    }
    const double restart = percentile(restarts, 0.5);
    const double walk = percentile(walks, 0.5);
    const double scan = percentile(scans, 0.5);
    std::printf("\n| measurement (median) | value |\n|---|---:|\n");
    std::printf("| entries indexed | %llu |\n", static_cast<unsigned long long>(st1.entries));
    std::printf("| directories | %llu |\n", static_cast<unsigned long long>(st1.directories));
    std::printf("| skipped (permission) | %llu |\n",
                static_cast<unsigned long long>(st1.skipped_permission));
    std::printf("| full index scan to Ready | %.1f ms |\n", scan);
    std::printf("| save (encode + fsync + rename) | %.1f ms |\n", percentile(saves, 0.5));
    std::printf("| index file size | %.2f MB (%.1f bytes/entry) |\n",
                static_cast<double>(bytes->size()) / 1e6,
                static_cast<double>(bytes->size()) /
                    static_cast<double>(std::max<std::uint64_t>(st1.entries, 1)));
    std::printf("| restart to Ready (load + verify) | %.1f ms |\n", restart);
    std::printf("| restart: load (read + parse + fold) | %.0f ms |\n", percentile(loads, 0.5));
    std::printf("| restart: verify pass | %.0f ms |\n", percentile(verifies, 0.5));
    std::printf("| plain fs::Walker walk, metadata false | %.1f ms |\n", walk);
    std::printf("| restart / full index scan | %.2f |\n", restart / scan);
    std::printf("| restart / plain walk | %.2f |\n", restart / walk);
    std::printf("| loaded_from_file every time | %s |\n", loaded ? "yes" : "NO");
}

// 1M names without a million inodes: the directories are real (so the verify pass finds their
// mtimes unchanged and lists nothing), the files exist only in the index file.
void bench_million(const stdfs::path& root, const stdfs::path& cache) {
    namespace sd = search::detail;
    constexpr std::uint32_t kDirs = 10'000;
    constexpr std::uint32_t kFilesPerDir = 99;
    stdfs::create_directories(root);
    std::vector<stdfs::path> paths{root};
    std::vector<std::uint32_t> parents{sd::kNoIndex};
    for (std::uint32_t i = 1; i <= kDirs; ++i) {
        const std::uint32_t parent = i <= 100 ? 0 : ((i - 1) % 100) + 1;
        paths.push_back(paths[parent] / ("dir_" + std::to_string(i)));
        parents.push_back(parent);
        ::mkdir(paths.back().c_str(), 0755);
    }
    struct timespec ts[2];
    ::clock_gettime(CLOCK_REALTIME, &ts[0]);
    ts[0].tv_sec -= 7200;
    ts[1] = ts[0];
    for (const stdfs::path& p : paths) {
        ::utimensat(AT_FDCWD, p.c_str(), ts, AT_SYMLINK_NOFOLLOW);
    }

    const search::NameIndexOptions o = bench_options(root, cache);
    {
        sd::Tree tree;
        tree.reset();
        tree.complete = true;
        tree.scan_started_at = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::system_clock::now().time_since_epoch())
                                   .count();
        std::string fold;
        std::mt19937 rng(11);
        const auto add = [&](std::uint32_t d, const std::string& name, fs::EntryKind kind,
                             std::uint32_t sub) {
            sd::Child c;
            if (sd::append_name(tree.dirs[d].pool, c, name, fold)) {
                c.kind = kind;
                c.dir = sub;
                tree.dirs[d].children.push_back(c);
                ++tree.entries;
            }
        };
        for (std::uint32_t i = 0; i < paths.size(); ++i) {
            const auto st = fs::stat(paths[i]);
            if (!st) {
                std::printf("cannot stat the synthetic tree\n");
                return;
            }
            if (i != 0) {
                const std::string name = paths[i].filename().native();
                const std::uint32_t d = tree.new_dir(parents[i], name);
                add(parents[i], name, fs::EntryKind::Directory, d);
            }
            tree.dirs[i].flags = sd::kDirScanned;
            tree.dirs[i].mtime_ns = st->mtime.time_since_epoch().count();
            tree.set_identity(i, st->id.device, st->id.inode);
            for (std::uint32_t f = 0; i != 0 && f < kFilesPerDir; ++f) {
                add(i, make_name(rng, (std::uint64_t{i} * kFilesPerDir) + f), fs::EntryKind::File,
                    sd::kNoIndex);
            }
        }
        auto bytes = sd::encode_index(tree, o.identity, 0);
        if (!bytes || !sd::write_file_atomic(o.file, *bytes)) {
            std::printf("cannot write the synthetic index\n");
            return;
        }
    }
    const double before = resident_mb();
    double ms = 0;
    auto index = ready_index(o, ms);
    if (!index) {
        std::printf("synthetic index did not become ready\n");
        return;
    }
    const double used = resident_mb() - before;
    const search::IndexStatus st = index->status();
    std::printf("\n## 1M synthetic entries\n\n| measurement | value |\n|---|---:|\n");
    std::printf("| entries | %llu |\n", static_cast<unsigned long long>(st.entries));
    std::printf("| load + verify (10k directories) | %.1f ms |\n", ms);
    std::printf("| resident memory | %.1f MB (%.1f bytes/entry) |\n", used,
                used * 1e6 / static_cast<double>(std::max<std::uint64_t>(st.entries, 1)));
    bench_queries(*index, "1M synthetic");
}

void restore_permissions(const stdfs::path& d) {
    std::error_code ec;
    for (const auto& e : stdfs::directory_iterator(d, ec)) {
        std::error_code ec2;
        if (e.is_directory(ec2) && !e.is_symlink(ec2)) {
            ::chmod(e.path().c_str(), 0755);
            restore_permissions(e.path());
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    std::uint64_t entries = 300'000;
    stdfs::path parent;
    std::string only;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        if (a == "--entries" && i + 1 < argc) {
            entries = std::strtoull(argv[++i], nullptr, 10);
        } else if (a == "--dir" && i + 1 < argc) {
            parent = argv[++i];
        } else if (a == "--only" && i + 1 < argc) {
            only = argv[++i];
        } else {
            std::fprintf(stderr, "usage: search_bench [--entries N] [--dir DIR] "
                                 "[--only filter|walk|index|million]\n");
            return 2;
        }
    }
    const auto wants = [&](std::string_view part) { return only.empty() || only == part; };
    if (!only.empty() && only != "filter" && only != "walk" && only != "index" &&
        only != "million") {
        std::fprintf(stderr, "search_bench: unknown part '%s'\n", only.c_str());
        return 2;
    }
    if (wants("filter")) {
        bench_filter();
    }
    if (only == "filter") {
        return 0;
    }

    std::error_code ec;
    if (parent.empty()) {
        parent = stdfs::temp_directory_path(ec);
    }
    parent = stdfs::absolute(parent, ec);
    std::string work = (parent / "navigator-search-bench-XXXXXX").native();
    // Only a directory created right here is ever removed, never one the caller named.
    if (::mkdtemp(work.data()) == nullptr) {
        std::fprintf(stderr, "search_bench: cannot create a work directory in %s\n",
                     parent.c_str());
        return 1;
    }
    const stdfs::path dir(work);

    if (wants("walk") || wants("index")) {
        const stdfs::path root = dir / "tree";
        stdfs::create_directories(root);
        const auto t0 = Clock::now();
        const Generated g = generate(root, entries);
        std::printf("\ngenerated %llu entries (%llu directories) in %.1f s; warm page cache\n",
                    static_cast<unsigned long long>(g.entries),
                    static_cast<unsigned long long>(g.dirs), ms_since(t0) / 1000.0);
        if (wants("walk")) {
            bench_walk(root);
        }
        if (wants("index")) {
            bench_index(root, dir / "cache");
        }
    }
    if (wants("million")) {
        bench_million(dir / "million", dir / "million-cache");
    }

    restore_permissions(dir);
    stdfs::remove_all(dir, ec);
    return ec ? 1 : 0;
}
