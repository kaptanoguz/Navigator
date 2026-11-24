#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <navigator/plugins.hpp>

TEST_CASE("PluginManager loads sample C ABI plugin and dispatches all capabilities",
          "[plugins][p10]") {
    navigator::plugins::PluginManager pm;
    const navigator::Path sample_so(NAVIGATOR_SAMPLE_PLUGIN_PATH);

    auto loaded = pm.load_file(sample_so);
    REQUIRE(loaded.has_value());
    CHECK(loaded->id == "org.navigator.sample.hello");
    CHECK(loaded->name == "Sample Hello Plugin");
    CHECK(loaded->version == "1.0.0");
    CHECK(loaded->command_count == 1);
    CHECK(loaded->column_count == 1);
    CHECK(loaded->has_overlay);
    CHECK(loaded->has_preview);

    // Duplicate load rejected with AlreadyExists.
    auto dup = pm.load_file(sample_so);
    REQUIRE_FALSE(dup.has_value());
    CHECK(dup.error().code == navigator::ErrorCode::AlreadyExists);

    // Command invocation succeeds with valid dir, fails with empty dir.
    REQUIRE(pm.commands().size() == 1);
    CHECK(pm.commands()[0].id == "sample.hello");
    CHECK(pm.invoke_command("sample.hello", navigator::Path("/tmp"), {}).has_value());
    CHECK_FALSE(pm.invoke_command("sample.hello", navigator::Path(), {}).has_value());
    CHECK_FALSE(pm.invoke_command("unknown.cmd", navigator::Path("/tmp"), {}).has_value());

    // Overlay badge fires on .navsample files and not on other files.
    auto ov = pm.query_overlay(navigator::Path("/tmp/demo.navsample"));
    REQUIRE(ov.has_value());
    CHECK(ov->badge_text == "S");
    CHECK(ov->color_rgba == 0x4EC9B0FFu);
    CHECK_FALSE(pm.query_overlay(navigator::Path("/tmp/plain.txt")).has_value());

    // Custom column
    auto col_val = pm.query_column("sample.name_len", navigator::Path("/tmp/hello.txt"));
    REQUIRE(col_val.has_value());
    CHECK(*col_val == "9 chars");

    // Text preview
    auto prev = pm.query_preview(navigator::Path("/tmp/demo.navsample"));
    REQUIRE(prev.has_value());
    CHECK(prev->find("demo.navsample") != std::string::npos);

    pm.unload_all();
    CHECK(pm.plugins().empty());
}

TEST_CASE("PluginManager handles missing files cleanly", "[plugins][p10]") {
    navigator::plugins::PluginManager pm;
    auto missing = pm.load_file(navigator::Path("/nonexistent/plugin.so"));
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().code == navigator::ErrorCode::NotFound);
    CHECK(pm.load_directory(navigator::Path("/nonexistent/plugins_dir")) == 0);
}
