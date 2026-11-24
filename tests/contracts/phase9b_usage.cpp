// Phase 9B contract usage test (compile only, C++20 and C++23).
#include <navigator/appearance.hpp>
#include <navigator/batch_rename.hpp>
#include <navigator/commands.hpp>
#include <navigator/ops.hpp>

namespace {

using namespace navigator;
using namespace navigator::appearance;
using namespace navigator::ops;

static_assert(clamp_font_scale(10) == kMinFontScalePercent);
static_assert(clamp_font_scale(999) == kMaxFontScalePercent);
static_assert(clamp_spacing_scale(10) == kMinSpacingScalePercent);
static_assert(clamp_spacing_scale(999) == kMaxSpacingScalePercent);
static_assert(clamp_corner_radius(-5) == kMinCornerRadiusDp);
static_assert(clamp_corner_radius(99) == kMaxCornerRadiusDp);
static_assert(to_string(OpKind::BatchRename) == "batch_rename");

} // namespace

void navigator_phase9b_contract_check();
void navigator_phase9b_contract_check() {
    using namespace navigator;
    using namespace navigator::appearance;
    using namespace navigator::ops;

    // Batch rename contract.
    std::vector<BatchRenameItem> items;
    items.push_back(BatchRenameItem{Path{"/tmp/a.txt"}, "a.txt", false, Timestamp{}, 10});
    BatchRenameRule rule;
    rule.template_str = "IMG_{date}_{counter:4}_{uid:6}{ext}";
    rule.find_text = "a";
    rule.replace_text = "b";
    rule.case_transform = CaseTransform::Lower;
    rule.trim_whitespace = true;

    const auto lines = split_paste_lines("one\ntwo\n");
    (void)lines;
    const auto paste = interpret_batch_paste("photo", items, true);
    (void)paste.is_single_line;
    (void)format_batch_uid(123, 0, 8);
    (void)format_batch_date(Timestamp{}, "YYYY-MM-DD");

    const BatchRenamePlan plan = build_batch_rename_plan(items, rule, {}, {"a.txt"});
    (void)plan.has_errors();
    (void)plan.can_apply();
    const OpRequest req = make_batch_rename_request(items, plan);
    (void)req.new_names.size();

    // Appearance & custom themes contract.
    const auto col = parse_hex_color("#8EA6FF");
    if (col) {
        (void)format_hex_color(*col);
    }
    AppearanceSettings app;
    app.theme_id = "dark";
    app.density = UiDensity::Compact;
    app.font_scale_percent = 110;
    app.spacing_scale_percent = 100;
    app.corner_radius_dp = 8;
    app.animations = false;
    app.alternating_rows = true;
    app = normalize(app);

    (void)to_string(UiDensity::Comfortable);
    (void)parse_density("comfortable");
    (void)settings_file(Path{"/tmp"});
    (void)themes_dir(Path{"/tmp"});
    (void)to_json(app);
    (void)appearance_from_json("{}");
    (void)load_appearance_settings(Path{"/tmp/settings.json"});
    (void)save_appearance_settings(Path{"/tmp/settings.json"}, app);

    ThemeScheme scheme;
    scheme.id = "nord";
    scheme.name = "Nord";
    scheme.base = Theme::Dark;
    scheme.colors.accent = Rgba8{136, 192, 208, 255};
    (void)to_json(scheme);
    (void)theme_scheme_from_json("{}", "nord");
    (void)load_theme_scheme(Path{"/tmp/themes/nord.json"});
    (void)list_custom_themes(Path{"/tmp/themes"});

    const CommandId ids[] = {
        CommandId::BatchRename,          CommandId::ToggleAnimations,
        CommandId::ToggleAlternatingRows, CommandId::CycleDensity,
        CommandId::FontSizeIncrease,     CommandId::FontSizeDecrease,
        CommandId::FontSizeReset,        CommandId::OpenAppearanceSettings,
    };
    (void)ids;
}
