// Phase 5 index service and presentation, the types popup's keys, string coverage.
#include <memory>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <navigator/i18n.hpp>

#include "i18n/catalog.hpp"
#include "search/index_service.hpp"
#include "search/index_status.hpp"
#include "search_test_util.hpp"
#include "ui_fixture.hpp"
#include "workspace/workspace_view.hpp"

using namespace navigator;
using namespace navigator::ui;
using navigator::test::FakeIndex;
using navigator::test::kSearchWait;
using navigator::test::TempDir;

namespace {

struct LanguageGuard {
    LanguageGuard() = default;
    LanguageGuard(const LanguageGuard&) = delete;
    LanguageGuard& operator=(const LanguageGuard&) = delete;
    LanguageGuard(LanguageGuard&&) = delete;
    LanguageGuard& operator=(LanguageGuard&&) = delete;
    ~LanguageGuard() { set_language(Language::English); }
};

// IndexService owns a unique_ptr: forward to an index the test keeps.
class SharedIndex final : public search::IIndex {
public:
    explicit SharedIndex(std::shared_ptr<FakeIndex> fake) : fake_(std::move(fake)) {}
    void start() override { fake_->start(); }
    void stop() noexcept override { fake_->stop(); }
    void request_rescan(search::RescanKind kind) override { fake_->request_rescan(kind); }
    [[nodiscard]] search::IndexStatus status() const override { return fake_->status(); }
    [[nodiscard]] const Path& root() const noexcept override { return fake_->root(); }
    [[nodiscard]] Result<search::IndexResult> query(const search::IndexQuery& q,
                                                    std::stop_token stop) const override {
        return fake_->query(q, std::move(stop));
    }

private:
    std::shared_ptr<FakeIndex> fake_;
};

struct Made {
    std::vector<std::shared_ptr<FakeIndex>> fakes;
    std::vector<search::NameIndexOptions> options;

    IndexService::Options service_options(bool enabled) {
        IndexService::Options o;
        o.enabled = enabled;
        o.cache_root = "/cache/navigator";
        o.factory = [this](search::NameIndexOptions io) -> Result<std::unique_ptr<search::IIndex>> {
            fakes.push_back(std::make_shared<FakeIndex>(io.root));
            options.push_back(std::move(io));
            return std::unique_ptr<search::IIndex>(std::make_unique<SharedIndex>(fakes.back()));
        };
        return o;
    }
};

VolumeInfo volume(const char* mount, VolumeKind kind, const char* uuid) {
    VolumeInfo v;
    v.mount_point = mount;
    v.kind = kind;
    v.fs_type = "ext4";
    v.uuid = uuid;
    v.root = "/";
    return v;
}

} // namespace

TEST_CASE("The index service indexes default volumes after start", "[index]") {
    Made made;
    IndexService service(made.service_options(true));
    const VolumeInfo root = volume("/", VolumeKind::Root, "r");
    const VolumeInfo data = volume("/data", VolumeKind::Internal, "d");
    CHECK(service.set_volumes({root, data, volume("/media/usb", VolumeKind::Removable, "u"),
                               volume("/mnt/nas", VolumeKind::Network, "n")}));
    REQUIRE(service.size() == 2);
    REQUIRE(made.options.size() == 2);
    for (const search::NameIndexOptions& o : made.options) {
        CHECK(o.background_priority);
        CHECK(o.file == search::index_file("/cache/navigator", o.identity));
    }
    CHECK(made.fakes[0]->starts == 0); // never before the first frame
    service.start();
    CHECK(made.fakes[0]->starts == 1);
    CHECK(made.fakes[1]->starts == 1);

    const std::shared_ptr<search::IIndex> found = service.index_for("/data/music");
    REQUIRE(found);
    CHECK(found->root() == "/data");
    CHECK(service.index_for("/database")->root() == "/"); // whole components only
    CHECK(service.rebuild("/data/music"));
    CHECK(made.fakes[1]->rebuilds == 1);

    CHECK_FALSE(service.set_volumes({root, data}));
    CHECK(service.set_volumes({root})); // /data went away
    CHECK(service.size() == 1);
    service.shutdown(kSearchWait);
    CHECK(made.fakes[0]->stops == 1);
    CHECK(made.fakes[1]->stops == 1);
}

TEST_CASE("A disabled index service creates nothing", "[index]") {
    Made made;
    IndexService service(made.service_options(false));
    CHECK_FALSE(service.set_volumes({volume("/", VolumeKind::Root, "r")}));
    service.start();
    CHECK(made.options.empty());
    CHECK_FALSE(service.index_for("/"));
}

TEST_CASE("Index states read clearly in both languages", "[index][i18n]") {
    const LanguageGuard guard;
    search::IndexStatus s;
    s.state = search::IndexState::Scanning;
    s.entries = 12000;
    s.directories = 200;
    s.dirs_pending = 100;
    set_language(Language::English);
    IndexStatusView v = present_index_status(s);
    CHECK(v.text == "Indexing 50%");
    CHECK(v.tone == IndexTone::Busy);
    CHECK(v.fraction.has_value());
    set_language(Language::Turkish);
    CHECK(present_index_status(s).text == "İndeksleniyor %50");

    set_language(Language::English);
    s.state = search::IndexState::Ready;
    v = present_index_status(s);
    CHECK(v.text == "Index ready (12,000 items)");
    CHECK(v.tone == IndexTone::Ready);
    CHECK_FALSE(v.fraction.has_value());
    s.state = search::IndexState::Failed;
    CHECK(present_index_status(s).tone == IndexTone::Problem);
    s.state = search::IndexState::Loading;
    CHECK(present_index_status(s).tone == IndexTone::Busy);
    CHECK_FALSE(index_scan_fraction(search::IndexStatus{}).has_value());
    // The first snapshot of a scan has nothing pending yet: that is not 100%.
    search::IndexStatus first;
    first.state = search::IndexState::Scanning;
    first.directories = 1;
    CHECK_FALSE(index_scan_fraction(first).has_value());
}

TEST_CASE("Every command and catalog key has English and Turkish text", "[i18n]") {
    CommandRegistry registry;
    REQUIRE(register_builtin_commands(registry));
    for (const CommandInfo& c : registry.all()) {
        INFO(c.name);
        CHECK(has_translation(c.title_key, Language::English));
        CHECK(has_translation(c.title_key, Language::Turkish));
    }
    for (const std::string_view key : catalog_keys()) {
        INFO(key);
        CHECK(has_translation(key, Language::English));
        if (!key.ends_with(".one")) {
            CHECK(has_translation(key, Language::Turkish));
        }
    }
}

TEST_CASE("The types popup toggles the highlighted row by keyboard", "[types][popup]") {
    TempDir t;
    t.dir("folder");
    t.file("a.txt");
    Workspace::Options o;
    o.home = t.path();
    Workspace ws(std::move(o));
    ws.start(std::nullopt, t.path());
    REQUIRE(ws.active_model().wait_listing(kSearchWait));
    (void)ws.frame_start();

    WorkspaceView view;
    view.open_types_popup(ws);
    REQUIRE(view.types_popup_open());
    CHECK_FALSE(view.wants_text_input());
    CommandQueue queue;
    std::vector<Intent> out;
    const auto send = [&](CommandId id) { CHECK(view.route_command(Command{id, 0}, queue, out)); };
    send(CommandId::Activate);
    REQUIRE(out.size() == 1);
    CHECK(out[0].kind == Intent::Kind::ToggleAllTypes);
    send(CommandId::CursorDown); // Folders
    send(CommandId::CursorDown); // .txt
    send(CommandId::Activate);
    REQUIRE(out.size() == 2);
    CHECK(out[1].kind == Intent::Kind::SetTypeVisible);
    CHECK(out[1].text == "txt");
    CHECK_FALSE(out[1].additive);
    CHECK(out[1].tab == ws.active_tab().value);
    send(CommandId::ToggleTypesPopup);
    CHECK_FALSE(view.types_popup_open());
}
