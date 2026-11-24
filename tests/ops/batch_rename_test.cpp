// tests/ops/batch_rename_test.cpp — Phase 9B batch rename planner, execution, swaps & undo/redo.
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <navigator/batch_rename.hpp>
#include <navigator/ops.hpp>

#include "support.hpp"

using namespace navigator;
using namespace navigator::ops;
using namespace navigator::ops::test;

namespace {

BatchRenameItem make_item(const Path& dir, std::string name, bool is_dir = false,
                          Timestamp ts = Timestamp{}) {
    BatchRenameItem item;
    item.path = dir / name;
    item.original_name = std::move(name);
    item.is_directory = is_dir;
    item.mtime = ts;
    return item;
}

std::string read_file_str(const Path& p) {
    std::ifstream in(p.native(), std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

} // namespace

TEST_CASE("Batch rename tokens: counter, date, uid, stem, ext, parent", "[ops][batch_rename]") {
    const Path dir{"/tmp/Photos"};
    const Timestamp ts{std::chrono::seconds{1700000000}}; // 2023-11-14 (or 15 depending on TZ)
    std::vector<BatchRenameItem> items = {
        make_item(dir, "alpha.jpg", false, ts),
        make_item(dir, "beta.tar.gz", false, ts),
        make_item(dir, "folder.v1", true, ts),
    };

    BatchRenameRule rule;
    rule.template_str = "{parent}_{counter:3}_{stem}_{uid:6}{ext}";
    rule.counter_start = 10;
    rule.counter_step = 5;
    rule.uid_seed = 42;

    const BatchRenamePlan plan = build_batch_rename_plan(items, rule);
    REQUIRE(plan.rows.size() == 3);
    REQUIRE(plan.can_apply());
    REQUIRE(plan.changed_count == 3);
    REQUIRE(plan.collision_count == 0);
    REQUIRE(plan.invalid_count == 0);

    const std::string uid0 = format_batch_uid(42, 0, 6);
    const std::string uid1 = format_batch_uid(42, 1, 6);
    const std::string uid2 = format_batch_uid(42, 2, 6);
    CHECK(uid0.size() == 6);
    CHECK(uid0 != uid1);
    CHECK(uid1 != uid2);

    CHECK(plan.rows[0].proposed_name == "Photos_010_alpha_" + uid0 + ".jpg");
    CHECK(plan.rows[1].proposed_name == "Photos_015_beta_" + uid1 + ".tar.gz");
    CHECK(plan.rows[2].proposed_name == "Photos_020_folder.v1_" + uid2);

    // Date token formats cleanly and matches format_batch_date.
    BatchRenameRule date_rule;
    date_rule.template_str = "{date:YYYYMMDD}_{stem}{ext}";
    const BatchRenamePlan date_plan = build_batch_rename_plan(items, date_rule);
    REQUIRE(date_plan.can_apply());
    const std::string expected_date = format_batch_date(ts, "YYYYMMDD");
    CHECK(expected_date.size() == 8);
    CHECK(date_plan.rows[0].proposed_name == expected_date + "_alpha.jpg");
}

TEST_CASE("Batch rename find/replace, case transform, whitespace trim, and Unicode",
          "[ops][batch_rename]") {
    const Path dir{"/tmp/docs"};
    std::vector<BatchRenameItem> items = {
        make_item(dir, "  Rapor 2026 Draft.txt  "),
        make_item(dir, "Güneş_şarkısı_draft.txt"),
    };

    BatchRenameRule rule;
    rule.template_str = "{name}";
    rule.find_text = "draft";
    rule.replace_text = "final";
    rule.find_case_sensitive = false;
    rule.case_transform = CaseTransform::Title;
    rule.trim_whitespace = true;

    const BatchRenamePlan plan = build_batch_rename_plan(items, rule);
    REQUIRE(plan.can_apply());
    CHECK(plan.rows[0].proposed_name == "Rapor 2026 Final.txt");
    CHECK(plan.rows[1].proposed_name == "Güneş_şarkısı_Final.txt");
}

TEST_CASE("Batch rename single-line and multi-line paste semantics", "[ops][batch_rename]") {
    const Path dir{"/tmp/p"};
    std::vector<BatchRenameItem> items = {
        make_item(dir, "a.jpg"),
        make_item(dir, "b.raw"),
        make_item(dir, "c.xmp"),
    };

    // Single-line paste into multi-item batch preserves extensions via template.
    const BatchPasteResult single = interpret_batch_paste("  Holiday_01  \n", items, true);
    CHECK(single.is_single_line);
    CHECK(single.single_line_template == "Holiday_01{ext}");

    BatchRenameRule rule;
    rule.template_str = single.single_line_template;
    const BatchRenamePlan p1 = build_batch_rename_plan(items, rule);
    REQUIRE(p1.can_apply());
    CHECK(p1.rows[0].proposed_name == "Holiday_01.jpg");
    CHECK(p1.rows[1].proposed_name == "Holiday_01.raw");
    CHECK(p1.rows[2].proposed_name == "Holiday_01.xmp");

    // Multi-line paste assigns line-by-line overrides.
    const BatchPasteResult multi = interpret_batch_paste("one.jpg\r\ntwo.raw\r\nthree.xmp\r\n", items);
    CHECK_FALSE(multi.is_single_line);
    REQUIRE(multi.line_overrides.size() == 3);
    const BatchRenamePlan p2 = build_batch_rename_plan(items, BatchRenameRule{}, multi.line_overrides);
    REQUIRE(p2.can_apply());
    CHECK(p2.rows[0].proposed_name == "one.jpg");
    CHECK(p2.rows[1].proposed_name == "two.raw");
    CHECK(p2.rows[2].proposed_name == "three.xmp");
}

TEST_CASE("Batch rename detects internal, existing-sibling, and invalid-name errors",
          "[ops][batch_rename]") {
    const Path dir{"/tmp/c"};
    std::vector<BatchRenameItem> items = {
        make_item(dir, "a.txt"),
        make_item(dir, "b.txt"),
        make_item(dir, "c.txt"),
    };

    // Internal collision: all items map to "same.txt".
    BatchRenameRule dup_rule;
    dup_rule.template_str = "same{ext}";
    const BatchRenamePlan dup_plan = build_batch_rename_plan(items, dup_rule);
    CHECK_FALSE(dup_plan.can_apply());
    CHECK(dup_plan.collision_count == 3);
    CHECK(dup_plan.rows[0].status == PreviewRowStatus::CollisionInternal);
    CHECK(dup_plan.rows[1].status == PreviewRowStatus::CollisionInternal);
    CHECK(dup_plan.rows[2].status == PreviewRowStatus::CollisionInternal);

    // Existing sibling collision vs vacated swap.
    const std::vector<std::string> siblings = {"a.txt", "b.txt", "c.txt", "taken.txt"};
    const std::vector<std::string> overrides = {"b.txt", "a.txt", "taken.txt"};
    const BatchRenamePlan mixed =
        build_batch_rename_plan(items, BatchRenameRule{}, overrides, siblings);
    CHECK_FALSE(mixed.can_apply());
    // a->b and b->a are vacated swaps (Ready!), while c->taken is CollisionExisting.
    CHECK(mixed.rows[0].status == PreviewRowStatus::Ready);
    CHECK(mixed.rows[1].status == PreviewRowStatus::Ready);
    CHECK(mixed.rows[2].status == PreviewRowStatus::CollisionExisting);
    CHECK(mixed.collision_count == 1);

    // Invalid name (slash).
    const std::vector<std::string> bad = {"ok.txt", "bad/name.txt", "ok2.txt"};
    const BatchRenamePlan bad_plan = build_batch_rename_plan(items, BatchRenameRule{}, bad);
    CHECK_FALSE(bad_plan.can_apply());
    CHECK(bad_plan.invalid_count == 1);
    CHECK(bad_plan.rows[1].status == PreviewRowStatus::InvalidName);
    CHECK(bad_plan.rows[1].name_error == NameError::ContainsSlash);
}

TEST_CASE("Batch rename executes 200 files, handles swaps, and Undo/Redo restores all",
          "[ops][batch_rename]") {
    Fixture fx;
    std::vector<BatchRenameItem> items;
    std::vector<std::string> siblings;
    items.reserve(200);
    siblings.reserve(200);
    for (int i = 0; i < 200; ++i) {
        const std::string name = "file_" + std::to_string(i) + ".txt";
        const Path p = fx.file(name, "payload-" + std::to_string(i));
        items.push_back(make_item(fx.root(), name));
        siblings.push_back(name);
        REQUIRE(std::filesystem::exists(p.native()));
    }

    // Rename 200 files with counter + Turkish characters.
    BatchRenameRule rule;
    rule.template_str = "belge_{counter:3}_öğe{ext}";
    const BatchRenamePlan plan = build_batch_rename_plan(items, rule, {}, siblings);
    REQUIRE(plan.can_apply());
    REQUIRE(plan.changed_count == 200);

    OpRequest req = make_batch_rename_request(items, plan);
    REQUIRE(req.kind == OpKind::BatchRename);
    REQUIRE(req.sources.size() == 200);

    Env env(fx);
    auto res = run(env, req);
    REQUIRE(res.has_value());
    REQUIRE(res->done == 200);
    REQUIRE(res->failed == 0);
    REQUIRE(res->record.has_value());
    CHECK(res->record->kind == OpKind::BatchRename);
    CHECK(make_label(*res->record).key == "undo.label.batch_rename");

    CHECK(std::filesystem::exists((fx / "belge_001_öğe.txt").native()));
    CHECK(std::filesystem::exists((fx / "belge_200_öğe.txt").native()));
    CHECK_FALSE(std::filesystem::exists((fx / "file_0.txt").native()));

    // Undo restores all 200 original names in one operation.
    UndoStack stack;
    stack.record_done(*res->record, OpOrigin::User);
    auto undo_rec = stack.take_undo();
    REQUIRE(undo_rec != nullptr);

    OpRequest undo_req;
    undo_req.kind = OpKind::Undo;
    undo_req.origin = OpOrigin::Undo;
    undo_req.record = undo_rec;
    auto undo_res = run(env, undo_req);
    REQUIRE(undo_res.has_value());
    REQUIRE(undo_res->done == 200);
    REQUIRE(undo_res->failed == 0);
    REQUIRE(undo_res->record.has_value());

    CHECK(std::filesystem::exists((fx / "file_0.txt").native()));
    CHECK(std::filesystem::exists((fx / "file_199.txt").native()));
    CHECK_FALSE(std::filesystem::exists((fx / "belge_001_öğe.txt").native()));

    // Redo reapplies all 200 renames.
    stack.record_done(*undo_res->record, OpOrigin::Undo);
    auto redo_rec = stack.take_redo();
    REQUIRE(redo_rec != nullptr);

    OpRequest redo_req;
    redo_req.kind = OpKind::Redo;
    redo_req.origin = OpOrigin::Redo;
    redo_req.record = redo_rec;
    auto redo_res = run(env, redo_req);
    REQUIRE(redo_res.has_value());
    REQUIRE(redo_res->done == 200);
    CHECK(std::filesystem::exists((fx / "belge_001_öğe.txt").native()));
    CHECK(std::filesystem::exists((fx / "belge_200_öğe.txt").native()));
}

TEST_CASE("Batch rename cyclic swap (a->b, b->c, c->a) succeeds and undoes cleanly",
          "[ops][batch_rename]") {
    Fixture fx;
    (void)fx.file("a.txt", "AAA");
    (void)fx.file("b.txt", "BBB");
    (void)fx.file("c.txt", "CCC");

    OpRequest req;
    req.kind = OpKind::BatchRename;
    req.sources = {fx / "a.txt", fx / "b.txt", fx / "c.txt"};
    req.new_names = {"b.txt", "c.txt", "a.txt"};

    Env env(fx);
    auto res = run(env, req);
    REQUIRE(res.has_value());
    REQUIRE(res->done == 3);
    REQUIRE(res->failed == 0);
    REQUIRE(res->record.has_value());

    CHECK(read_file_str(fx / "b.txt") == "AAA");
    CHECK(read_file_str(fx / "c.txt") == "BBB");
    CHECK(read_file_str(fx / "a.txt") == "CCC");

    // Undo the cyclic swap!
    OpRequest undo_req;
    undo_req.kind = OpKind::Undo;
    undo_req.origin = OpOrigin::Undo;
    undo_req.record = std::make_shared<UndoRecord>(*res->record);
    auto undo_res = run(env, undo_req);
    REQUIRE(undo_res.has_value());
    REQUIRE(undo_res->done == 3);
    REQUIRE(undo_res->failed == 0);

    CHECK(read_file_str(fx / "a.txt") == "AAA");
    CHECK(read_file_str(fx / "b.txt") == "BBB");
    CHECK(read_file_str(fx / "c.txt") == "CCC");
}
