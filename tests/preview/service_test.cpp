// tests/preview/service_test.cpp: IPreviewService: delivery, priorities and epochs, coalescing,
// cancellation, LRU memory budget, invalidation, the Freedesktop cache in front of the
// providers (write, disk hit, failure markers, Regenerate, policy), max_pending, shutdown.
// Fake providers; temporary cache roots only. Run under TSan in the sanitizer gate.
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#include <navigator/preview_service.hpp>

#include "test_util.hpp"

using namespace navigator;
using namespace navigator::preview;
using namespace std::chrono_literals;

namespace {

// Produces an image of request.target (or 4x4) after an optional gate; records the order.
class GateProvider final : public IPreviewProvider {
public:
    std::string_view id() const noexcept override { return "test.gate"; }
    int can_handle(std::string_view mime, const fs::DirEntry&,
                   PreviewPurpose) const noexcept override {
        return mime == "image/x-test" ? kPriorityOverride : kCannotHandle;
    }
    Result<PreviewResult> produce(const PreviewRequest& r) const override {
        {
            std::unique_lock lock(mutex_);
            started_.push_back(r.path.filename().native());
            cv_.notify_all();
            if (r.path.filename().native().starts_with("block")) {
                cv_.wait(lock, r.stop, [&] { return open_; });
                if (r.stop.stop_requested()) {
                    ++cancelled_;
                    return fail(ErrorCode::Cancelled, "cancelled");
                }
            }
        }
        ++calls_;
        if (r.path.filename().native().starts_with("corrupt")) {
            return fail(ErrorCode::InvalidArgument, "corrupt");
        }
        ImagePreview ip;
        const PixelSize s = r.target.empty() ? PixelSize{4, 4} : r.target;
        ip.image = *make_image(s);
        ip.info.display = ip.info.stored = s;
        PreviewResult out;
        out.content = std::move(ip);
        return out;
    }
    void open() {
        {
            const std::lock_guard lock(mutex_);
            open_ = true;
        }
        cv_.notify_all();
    }
    bool wait_started(std::size_t n) const {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, 5s, [&] { return started_.size() >= n; });
    }
    std::vector<std::string> started() const {
        const std::lock_guard lock(mutex_);
        return started_;
    }
    int calls() const { return calls_.load(); }
    int cancelled() const { return cancelled_.load(); }

private:
    mutable std::mutex mutex_;
    mutable std::condition_variable_any cv_;
    mutable std::vector<std::string> started_;
    mutable std::atomic<int> calls_{0};
    mutable std::atomic<int> cancelled_{0};
    bool open_ = false;
};

struct Fixture {
    std::shared_ptr<ProviderRegistry> registry = std::make_shared<ProviderRegistry>();
    GateProvider* gate = nullptr;
    std::unique_ptr<IPreviewService> service;
    std::atomic<int> notified{0};

    explicit Fixture(PreviewServiceOptions opt = {}, bool builtin = false) {
        auto g = std::make_unique<GateProvider>();
        gate = g.get();
        REQUIRE(registry->add(std::move(g)));
        if (builtin) {
            REQUIRE(register_builtin_providers(*registry, {}));
        }
        opt.registry = registry;
        if (opt.worker_count == 0) {
            opt.worker_count = 1;
        }
        opt.on_completion = [this] { ++notified; };
        auto s = create_preview_service(std::move(opt));
        REQUIRE(s);
        service = std::move(*s);
    }
    ~Fixture() {
        if (gate != nullptr) {
            gate->open();
        }
        service->shutdown();
    }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;
    Fixture(Fixture&&) = delete;
    Fixture& operator=(Fixture&&) = delete;

    std::vector<Completion> wait(std::size_t n, std::chrono::milliseconds timeout = 5000ms) const {
        std::vector<Completion> out;
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (out.size() < n && std::chrono::steady_clock::now() < deadline) {
            service->poll(out);
            if (out.size() < n) {
                std::this_thread::sleep_for(1ms);
            }
        }
        return out;
    }
};

PreviewRequest req(const std::string& name, PixelSize target = {16, 16},
                   PreviewPurpose purpose = PreviewPurpose::Inspector) {
    PreviewRequest r;
    r.path = std::filesystem::path("/virtual") / name;
    r.entry.name = name;
    r.entry.kind = fs::EntryKind::File;
    r.entry.size = 10;
    r.mime = "image/x-test";
    r.target = target;
    r.purpose = purpose;
    return r;
}

} // namespace

TEST_CASE("Service delivers results, caches them and notifies", "[service]") {
    Fixture f;
    const Ticket t = f.service->submit(req("a"), RequestPriority::Visible, 1);
    REQUIRE(t.valid());
    auto done = f.wait(1);
    REQUIRE(done.size() == 1);
    CHECK(done[0].ticket == t);
    REQUIRE(done[0].result);
    CHECK((*done[0].result)->provider_id == "test.gate");
    CHECK(f.notified.load() >= 1);
    const PreviewKey key = make_key(req("a"));
    CHECK(done[0].key == key);
    auto hit = f.service->lookup(key);
    REQUIRE(hit);
    CHECK(hit.get() == done[0].result->get());
    CHECK(f.service->lookup(make_key(req("zzz"))) == nullptr);

    // A cache hit through submit: fresh ticket, delivered on the next poll without a job.
    const Ticket t2 = f.service->submit(req("a"), RequestPriority::Visible, 2);
    CHECK(t2.valid());
    CHECK_FALSE(t2 == t);
    std::vector<Completion> again;
    CHECK(f.service->poll(again) == 1);
    CHECK(f.gate->calls() == 1);
    const ServiceStats st = f.service->stats();
    CHECK(st.produced == 1);
    CHECK(st.memory_hits >= 2);
    CHECK(st.cache_entries == 1);

    PreviewRequest relative = req("a");
    relative.path = "rel/a";
    CHECK_FALSE(f.service->submit(relative, RequestPriority::Visible, 1).valid());
}

TEST_CASE("Priorities: Inspector > Visible > Prefetch, newest epoch first", "[service]") {
    Fixture f;
    f.service->submit(req("block0"), RequestPriority::Visible, 1);
    REQUIRE(f.gate->wait_started(1));
    f.service->submit(req("prefetch"), RequestPriority::Prefetch, 9);
    f.service->submit(req("visible-old"), RequestPriority::Visible, 1);
    f.service->submit(req("visible-old2"), RequestPriority::Visible, 1);
    f.service->submit(req("visible-new"), RequestPriority::Visible, 2);
    f.service->submit(req("inspector"), RequestPriority::Inspector, 0);
    f.gate->open();
    auto done = f.wait(6);
    REQUIRE(done.size() == 6);
    const auto order = f.gate->started();
    REQUIRE(order.size() == 6);
    CHECK(order[1] == "inspector");
    CHECK(order[2] == "visible-new");
    CHECK(order[3] == "visible-old");
    CHECK(order[4] == "visible-old2");
    CHECK(order[5] == "prefetch");
}

TEST_CASE("Identical keys coalesce and get the higher priority", "[service]") {
    Fixture f;
    f.service->submit(req("block0"), RequestPriority::Visible, 1);
    REQUIRE(f.gate->wait_started(1));
    const Ticket a = f.service->submit(req("same"), RequestPriority::Prefetch, 1);
    f.service->submit(req("other"), RequestPriority::Visible, 1);
    const Ticket b = f.service->submit(req("same"), RequestPriority::Inspector, 3);
    CHECK(a == b);
    CHECK(f.service->stats().pending == 2);
    f.gate->open();
    auto done = f.wait(3);
    REQUIRE(done.size() == 3);
    const auto order = f.gate->started();
    CHECK(order[1] == "same");
    CHECK(f.gate->calls() == 3);
    // A different target is a different key.
    const Ticket c = f.service->submit(req("same", {32, 32}), RequestPriority::Visible, 4);
    CHECK_FALSE(c == a);
    CHECK(f.wait(1).size() == 1);
}

TEST_CASE("cancel and cancel_older drop pending and running jobs silently", "[service][cancel]") {
    Fixture f;
    const Ticket running = f.service->submit(req("block-run"), RequestPriority::Visible, 1);
    REQUIRE(f.gate->wait_started(1));
    const Ticket pending = f.service->submit(req("p1"), RequestPriority::Visible, 1);
    f.service->submit(req("old"), RequestPriority::Prefetch, 1);
    f.service->submit(req("new"), RequestPriority::Prefetch, 5);
    f.service->cancel(pending);
    f.service->cancel_older(RequestPriority::Prefetch, 5);
    CHECK(f.service->stats().pending == 1);
    f.service->cancel(running); // stop token fires inside the provider
    auto done = f.wait(1);
    REQUIRE(done.size() == 1);
    REQUIRE(done[0].result);
    CHECK(done[0].key.path.filename() == "new");
    std::this_thread::sleep_for(20ms);
    std::vector<Completion> rest;
    f.service->poll(rest);
    CHECK(rest.empty());
    CHECK(f.gate->cancelled() == 1);
    f.service->cancel(Ticket{12345}); // unknown: no-op
    // The cancelled key can be submitted again and runs.
    f.gate->open();
    f.service->submit(req("block-run"), RequestPriority::Visible, 6);
    CHECK(f.wait(1).size() == 1);
}

TEST_CASE("Memory cache LRU evicts under the byte budget, never held entries", "[service][cache]") {
    PreviewServiceOptions opt;
    opt.cache_budget_bytes = 1; // raised to kMinCacheBudgetBytes (16 MiB)
    Fixture f(opt);
    CHECK(f.service->stats().cache_budget_bytes == kMinCacheBudgetBytes);
    // 1024 x 1024 RGBA = 4 MiB each.
    std::shared_ptr<const PreviewResult> held;
    for (int i = 0; i < 8; ++i) {
        f.service->submit(req("img" + std::to_string(i), {1024, 1024}), RequestPriority::Visible,
                          static_cast<std::uint64_t>(i));
        auto done = f.wait(1);
        REQUIRE(done.size() == 1);
        REQUIRE(done[0].result);
        if (i == 0) {
            held = *done[0].result; // the ui keeps the first one
        }
    }
    const ServiceStats st = f.service->stats();
    CHECK(st.cache_entries < 8);
    CHECK(f.service->lookup(make_key(req("img0", {1024, 1024}))) != nullptr); // held: kept
    CHECK(f.service->lookup(make_key(req("img1", {1024, 1024}))) == nullptr); // LRU: evicted
    CHECK(f.service->lookup(make_key(req("img7", {1024, 1024}))) != nullptr);
    held.reset();
    f.service->set_cache_budget(kMinCacheBudgetBytes);
    CHECK(f.service->stats().cache_bytes <= kMinCacheBudgetBytes);
    f.service->clear_cache();
    CHECK(f.service->stats().cache_entries == 0);
    CHECK(f.service->stats().cache_bytes == 0);
}

TEST_CASE("invalidate drops cached entries and pending jobs below a path", "[service][cache]") {
    Fixture f;
    PreviewRequest a = req("a");
    a.path = "/virtual/dir/a";
    PreviewRequest b = req("b");
    b.path = "/virtual/other/b";
    f.service->submit(a, RequestPriority::Visible, 1);
    f.service->submit(b, RequestPriority::Visible, 1);
    REQUIRE(f.wait(2).size() == 2);
    f.service->invalidate("/virtual/dir", InvalidateMode::Memory);
    CHECK(f.service->lookup(make_key(a)) == nullptr);
    CHECK(f.service->lookup(make_key(b)) != nullptr);

    f.service->submit(req("block0"), RequestPriority::Visible, 1);
    REQUIRE(f.gate->wait_started(3));
    PreviewRequest c = req("c");
    c.path = "/virtual/dir/sub/c";
    f.service->submit(c, RequestPriority::Visible, 1);
    f.service->invalidate("/virtual/dir", InvalidateMode::Memory);
    CHECK(f.service->stats().pending == 0);
}

TEST_CASE("max_pending drops the oldest Prefetch, then Visible jobs", "[service]") {
    PreviewServiceOptions opt;
    opt.max_pending = 2;
    Fixture f(opt);
    f.service->submit(req("block0"), RequestPriority::Visible, 1);
    REQUIRE(f.gate->wait_started(1));
    f.service->submit(req("pf-old"), RequestPriority::Prefetch, 1);
    f.service->submit(req("vis"), RequestPriority::Visible, 1);
    f.service->submit(req("pf-new"), RequestPriority::Prefetch, 2);
    CHECK(f.service->stats().pending == 2);
    f.service->submit(req("insp"), RequestPriority::Inspector, 1);
    CHECK(f.service->stats().pending == 2);
    f.gate->open();
    auto done = f.wait(3);
    CHECK(done.size() == 3); // block0, insp, vis
    std::vector<std::string> names;
    names.reserve(done.size());
    for (const auto& c : done) {
        names.push_back(c.key.path.filename().native());
    }
    CHECK(std::find(names.begin(), names.end(), "insp") != names.end());
    CHECK(std::find(names.begin(), names.end(), "vis") != names.end());
}

TEST_CASE("Shutdown cancels running jobs, is bounded and idempotent", "[service]") {
    Fixture f;
    f.service->submit(req("block0"), RequestPriority::Visible, 1);
    REQUIRE(f.gate->wait_started(1));
    f.service->submit(req("never"), RequestPriority::Visible, 1);
    const auto t0 = std::chrono::steady_clock::now();
    f.service->shutdown();
    CHECK(std::chrono::steady_clock::now() - t0 < 2500ms);
    f.service->shutdown();
    CHECK_FALSE(f.service->submit(req("late"), RequestPriority::Visible, 1).valid());
    std::vector<Completion> out;
    f.service->poll(out);
    CHECK(out.empty());
    CHECK(f.gate->cancelled() == 1);
}

TEST_CASE("Service rejects a missing registry", "[service]") {
    PreviewServiceOptions opt;
    CHECK(create_preview_service(opt).error().code == ErrorCode::InvalidArgument);
}

TEST_CASE("Concurrent submits, cancels and polls stay consistent", "[service][stress]") {
    PreviewServiceOptions opt;
    opt.worker_count = 4;
    Fixture f(opt);
    f.gate->open();
    std::atomic<bool> stop{false};
    std::vector<std::thread> threads;
    threads.reserve(3);
    for (int t = 0; t < 3; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < 300; ++i) {
                const auto ticket = f.service->submit(req("s" + std::to_string((i * 7 + t) % 50)),
                                                      static_cast<RequestPriority>(i % 3),
                                                      static_cast<std::uint64_t>(i));
                if (i % 5 == 0) {
                    f.service->cancel(ticket);
                }
                if (i % 17 == 0) {
                    f.service->cancel_older(RequestPriority::Prefetch,
                                            static_cast<std::uint64_t>(i));
                }
                if (i % 31 == 0) {
                    f.service->invalidate("/virtual", InvalidateMode::Memory);
                }
            }
        });
    }
    std::thread consumer([&] {
        std::vector<Completion> out;
        while (!stop.load()) {
            f.service->poll(out);
            (void)f.service->stats();
            std::this_thread::sleep_for(1ms);
        }
    });
    for (auto& t : threads) {
        t.join();
    }
    std::this_thread::sleep_for(50ms);
    stop = true;
    consumer.join();
    f.service->shutdown();
    SUCCEED();
}

// ---------------------------------------------------------------------------------------------
// Disk cache in front of the providers (real decoders, temporary cache root)
// ---------------------------------------------------------------------------------------------

namespace {

PreviewRequest file_req(const std::filesystem::path& p, std::string mime, PixelSize target) {
    PreviewRequest r;
    r.path = p;
    r.entry = *fs::stat(p, false);
    r.mime = std::move(mime);
    r.purpose = PreviewPurpose::Thumbnail;
    r.target = target;
    return r;
}

} // namespace

TEST_CASE("Thumbnails: write after decode, disk hit after, failure markers, Regenerate",
          "[service][thumbnail]") {
    test::TempDir tmp;
    const auto root = tmp / "thumbnails";
    const auto photos = tmp / "photos";
    std::filesystem::create_directories(photos);
    test::write_file(photos / "a.jpg", test::encode_jpeg(test::quadrants(800, 600)));
    test::write_text(photos / "bad.jpg", "\xFF\xD8\xFF garbage garbage garbage");
    PreviewServiceOptions opt;
    opt.thumbnails.root = root;
    Fixture f(opt, true);

    const auto a = file_req(photos / "a.jpg", "image/jpeg", {48, 48});
    f.service->submit(a, RequestPriority::Visible, 1);
    auto done = f.wait(1);
    REQUIRE(done.size() == 1);
    REQUIRE(done[0].result);
    {
        const auto& ip = std::get<ImagePreview>((*done[0].result)->content);
        CHECK(ip.image.size() == PixelSize{48, 36});
        CHECK(ip.origin == ImageOrigin::Decoded);
    }
    auto uri = thumbnail::file_uri(photos / "a.jpg");
    const auto thumb = thumbnail::thumbnail_path(root, thumbnail::ThumbnailSize::Normal, *uri);
    REQUIRE(std::filesystem::exists(thumb));
    CHECK(f.service->stats().thumbnails_written == 1);
    thumbnail::SourceInfo si;
    si.path = photos / "a.jpg";
    si.uri = *uri;
    si.mtime = a.entry.mtime;
    si.size = a.entry.size;
    auto stored = thumbnail::read_thumbnail(thumb, si, {});
    REQUIRE(stored);
    CHECK(stored->status == thumbnail::LookupStatus::Valid);
    CHECK(stored->image.size() == PixelSize{128, 96}); // class size, reusable by others
    CHECK(stored->original == PixelSize{800, 600});

    f.service->clear_cache();
    f.service->submit(a, RequestPriority::Visible, 2);
    done = f.wait(1);
    REQUIRE(done.size() == 1);
    REQUIRE(done[0].result);
    CHECK(std::get<ImagePreview>((*done[0].result)->content).origin == ImageOrigin::ThumbnailCache);
    CHECK(std::get<ImagePreview>((*done[0].result)->content).image.size() == PixelSize{48, 36});
    CHECK(f.service->stats().disk_hits == 1);

    // Corrupt file: error delivered, failure marker written; next time KnownFailure.
    const auto bad = file_req(photos / "bad.jpg", "image/jpeg", {48, 48});
    f.service->submit(bad, RequestPriority::Visible, 3);
    done = f.wait(1);
    REQUIRE(done.size() == 1);
    REQUIRE_FALSE(done[0].result);
    CHECK(done[0].result.error().code == ErrorCode::InvalidArgument);
    CHECK(f.service->stats().failures_written == 1);
    PreviewRequest bad2 = bad;
    bad2.target = {40, 40}; // new key, same file
    f.service->submit(bad2, RequestPriority::Visible, 4);
    done = f.wait(1);
    REQUIRE(done.size() == 1);
    REQUIRE(done[0].result);
    CHECK(std::get<PlaceholderPreview>((*done[0].result)->content).reason ==
          PlaceholderReason::KnownFailure);

    // Regenerate ignores the marker once (decodes again, fails again).
    f.service->invalidate(photos, InvalidateMode::Regenerate);
    f.service->submit(bad2, RequestPriority::Visible, 5);
    done = f.wait(1);
    REQUIRE(done.size() == 1);
    CHECK_FALSE(done[0].result);
    CHECK(f.service->stats().failures_written == 2);
    f.service->submit(bad2, RequestPriority::Visible, 6);
    done = f.wait(1);
    REQUIRE(done.size() == 1);
    REQUIRE(done[0].result); // once only: the marker applies again
    CHECK(std::get<PlaceholderPreview>((*done[0].result)->content).reason ==
          PlaceholderReason::KnownFailure);
}

TEST_CASE("Thumbnail policy: read-only on network volumes, skip inside the cache",
          "[service][thumbnail]") {
    test::TempDir tmp;
    const auto root = tmp / "thumbnails";
    test::write_file(tmp / "n.png", test::encode_png(test::quadrants(64, 64)));
    PreviewServiceOptions opt;
    opt.thumbnails.root = root;
    Fixture f(opt, true);
    PreviewRequest net = file_req(tmp / "n.png", "image/png", {64, 64});
    net.location.network = true;
    f.service->submit(net, RequestPriority::Visible, 1);
    auto done = f.wait(1);
    REQUIRE(done.size() == 1);
    REQUIRE(done[0].result);
    CHECK(std::get<PlaceholderPreview>((*done[0].result)->content).reason ==
          PlaceholderReason::NotGenerated);
    CHECK_FALSE(std::filesystem::exists(root));

    // An existing thumbnail is still used on a network volume.
    thumbnail::SourceInfo si;
    si.path = tmp / "n.png";
    si.uri = *thumbnail::file_uri(si.path);
    si.mtime = net.entry.mtime;
    si.size = net.entry.size;
    auto img = make_image({128, 128});
    REQUIRE(thumbnail::write_thumbnail(root, thumbnail::ThumbnailSize::Normal, si, *img, {}));
    PreviewRequest net2 = net;
    net2.target = {32, 32};
    f.service->submit(net2, RequestPriority::Visible, 2);
    done = f.wait(1);
    REQUIRE(done.size() == 1);
    REQUIRE(done[0].result);
    CHECK(std::get<ImagePreview>((*done[0].result)->content).origin == ImageOrigin::ThumbnailCache);

    // Files inside the cache are never thumbnailed.
    std::filesystem::create_directories(root / "large");
    test::write_file(root / "large" / "x.png", test::encode_png(test::quadrants(8, 8)));
    f.service->submit(file_req(root / "large" / "x.png", "image/png", {64, 64}),
                      RequestPriority::Visible, 3);
    done = f.wait(1);
    REQUIRE(done.size() == 1);
    REQUIRE(done[0].result);
    CHECK(std::get<PlaceholderPreview>((*done[0].result)->content).reason ==
          PlaceholderReason::NotGenerated);

    // Inspector purpose ignores the disk cache and always decodes.
    PreviewRequest insp = net;
    insp.purpose = PreviewPurpose::Inspector;
    f.service->submit(insp, RequestPriority::Inspector, 4);
    done = f.wait(1);
    REQUIRE(done.size() == 1);
    REQUIRE(done[0].result);
    CHECK(std::get<ImagePreview>((*done[0].result)->content).origin == ImageOrigin::Decoded);
}
