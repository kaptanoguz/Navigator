// src/ui/palette/palette_view.cpp — Phase 8 Command Palette overlay.
#include "palette/palette_view.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

#include <navigator/fs.hpp>
#include <navigator/i18n.hpp>

#include "input/shortcut_text.hpp"
#include "widgets/text_field.hpp"
#include "widgets/text_fit.hpp"

namespace navigator::ui {
namespace {

constexpr float kCardWidth = 640.0F;
constexpr float kFieldHeight = 46.0F;
constexpr float kRowHeight = 36.0F;
constexpr float kFooterHeight = 30.0F;
constexpr std::size_t kVisibleRows = 10;

std::string format_placeholder(std::string_view tmpl, std::string_view key,
                               std::string_view value) {
    std::string out(tmpl);
    const std::string token = "{" + std::string(key) + "}";
    if (const std::size_t pos = out.find(token); pos != std::string::npos) {
        out.replace(pos, token.size(), value);
    }
    return out;
}

bool contains_folded(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) {
        return true;
    }
    return fs::fold_name(haystack).find(fs::fold_name(needle)) != std::string::npos;
}

} // namespace

void PaletteOverlay::open(const CommandRegistry& registry, const Keymap& keymap,
                          Span<const Alias> aliases, const palette::History& history,
                          const CommandContext& context, std::string undo_label,
                          std::string redo_label, std::vector<PickerOption> running_ops,
                          std::vector<PickerOption> bookmarks) {
    open_ = true;
    mode_ = Mode::Commands;
    context_ = context;
    running_ops_ = std::move(running_ops);
    bookmarks_ = std::move(bookmarks);
    query_.set({});
    field_scroll_ = 0.0F;
    scroll_ = 0;
    entries_.clear();

    const auto collect_aliases = [&](std::string_view cmd_name, std::uint64_t arg) {
        std::vector<std::string> out;
        for (const Alias& a : aliases) {
            if (a.command == cmd_name && a.arg == arg) {
                out.push_back(a.name);
            }
        }
        return out;
    };

    for (const CommandInfo& info : registry.all()) {
        if (!info.palette_visible) {
            continue;
        }
        palette::Entry e;
        e.command = Command{info.id, 0};
        e.name = std::string(info.name);
        e.title = std::string(tr(info.title_key));
        if (info.id == CommandId::Undo && !undo_label.empty()) {
            e.match_title = e.title;
            e.title = format_placeholder(tr("cmd.edit.undo_labeled"), "label", undo_label);
        } else if (info.id == CommandId::Redo && !redo_label.empty()) {
            e.match_title = e.title;
            e.title = format_placeholder(tr("cmd.edit.redo_labeled"), "label", redo_label);
        }
        e.category = std::string(tr(category_title_key(info.category)));
        if (!info.description_key.empty()) {
            const std::string_view desc = tr(info.description_key);
            if (desc != info.description_key) {
                e.description = std::string(desc);
            }
        }
        e.shortcut = primary_shortcut_text(keymap, info.name, 0, true);
        e.aliases = collect_aliases(info.name, 0);
        e.category_id = info.category;
        e.danger = effective_danger(info, context);
        e.arg_kind = info.arg_kind;
        e.requires_all = info.requires_all;
        e.hidden_if_any = info.hidden_if_any;
        entries_.push_back(std::move(e));
    }

    // Named non-zero variants (Copy Folder Path, New Tab at Home, Paste into Folder).
    for (const CommandVariant& v : builtin_command_variants()) {
        const CommandInfo* info = registry.find(v.id);
        if (info == nullptr || !info->palette_visible) {
            continue;
        }
        palette::Entry e;
        e.command = Command{v.id, v.arg};
        e.name = std::string(info->name);
        e.title = std::string(tr(v.title_key));
        e.category = std::string(tr(category_title_key(info->category)));
        e.shortcut = primary_shortcut_text(keymap, info->name, v.arg, false);
        e.aliases = collect_aliases(info->name, v.arg);
        e.category_id = info->category;
        e.danger = effective_danger(*info, context);
        e.arg_kind = info->arg_kind;
        e.requires_all = info->requires_all | v.requires_all;
        e.hidden_if_any = info->hidden_if_any;
        entries_.push_back(std::move(e));
    }

    rerank(history);
}

void PaletteOverlay::close() noexcept {
    open_ = false;
    mode_ = Mode::Commands;
    selected_.reset();
    scroll_ = 0;
}

void PaletteOverlay::set_query(std::string_view text, const palette::History& history) {
    query_.set(text);
    rerank(history);
}

void PaletteOverlay::query_changed(const palette::History& history) { rerank(history); }

void PaletteOverlay::rerank(const palette::History& history) {
    scroll_ = 0;
    if (mode_ == Mode::Commands) {
        results_ = palette::rank(query_.text(), entries_, history, context_);
        selected_ = results_.default_index;
        return;
    }
    const auto& src = (mode_ == Mode::PickOp) ? running_ops_ : bookmarks_;
    filtered_picker_.clear();
    for (std::size_t i = 0; i < src.size(); ++i) {
        if (contains_folded(src[i].label, query_.text()) ||
            contains_folded(src[i].detail, query_.text())) {
            filtered_picker_.push_back(i);
        }
    }
    selected_ = filtered_picker_.empty() ? std::nullopt : std::optional<std::size_t>{0};
}

void PaletteOverlay::ensure_visible() noexcept {
    if (!selected_) {
        return;
    }
    if (*selected_ < scroll_) {
        scroll_ = *selected_;
    } else if (*selected_ >= scroll_ + kVisibleRows) {
        scroll_ = *selected_ - kVisibleRows + 1;
    }
}

std::optional<PaletteAction> PaletteOverlay::activate_command_hit(std::size_t hit_index) {
    if (hit_index >= results_.hits.size()) {
        return std::nullopt;
    }
    const palette::Entry& entry = entries_[results_.hits[hit_index].entry];

    // P5: secondary pickers for OpId (>1 running) and BookmarkRow (not focused).
    if (entry.arg_kind == ArgKind::OpId && context_.running_operations > 1U &&
        !running_ops_.empty()) {
        pending_picker_cmd_ = entry.command;
        mode_ = Mode::PickOp;
        query_.set({});
        field_scroll_ = 0.0F;
        filtered_picker_.clear();
        for (std::size_t i = 0; i < running_ops_.size(); ++i) {
            filtered_picker_.push_back(i);
        }
        selected_ = filtered_picker_.empty() ? std::nullopt : std::optional<std::size_t>{0};
        scroll_ = 0;
        return std::nullopt;
    }
    if (entry.arg_kind == ArgKind::BookmarkRow &&
        !has(context_.flags, ContextFlags::BookmarkRowFocused) && !bookmarks_.empty()) {
        pending_picker_cmd_ = entry.command;
        mode_ = Mode::PickBookmark;
        query_.set({});
        field_scroll_ = 0.0F;
        filtered_picker_.clear();
        for (std::size_t i = 0; i < bookmarks_.size(); ++i) {
            filtered_picker_.push_back(i);
        }
        selected_ = filtered_picker_.empty() ? std::nullopt : std::optional<std::size_t>{0};
        scroll_ = 0;
        return std::nullopt;
    }

    PaletteAction act;
    act.command = entry.command;
    act.record_history = palette::is_recordable(entry);
    if (act.record_history) {
        act.history_key = palette::history_key(entry);
    }
    close();
    return act;
}

std::optional<PaletteAction> PaletteOverlay::activate_selected() {
    if (!selected_) {
        return std::nullopt; // P4: destructive-only result has no default highlight
    }
    if (mode_ == Mode::Commands) {
        return activate_command_hit(*selected_);
    }
    if (*selected_ >= filtered_picker_.size()) {
        return std::nullopt;
    }
    const auto& src = (mode_ == Mode::PickOp) ? running_ops_ : bookmarks_;
    const PickerOption& opt = src[filtered_picker_[*selected_]];
    PaletteAction act;
    act.command = Command{pending_picker_cmd_.id, opt.arg};
    act.record_history = false; // picker runtime-id results are never recorded
    close();
    return act;
}

std::optional<PaletteAction> PaletteOverlay::handle_key(KeyChord chord,
                                                        const palette::History& history) {
    if (!open_) {
        return std::nullopt;
    }
    const std::size_t count =
        (mode_ == Mode::Commands) ? results_.hits.size() : filtered_picker_.size();

    if (chord.key == Key::Escape && chord.mods == Modifiers::None) {
        if (mode_ != Mode::Commands) {
            mode_ = Mode::Commands;
            query_.set({});
            rerank(history);
        } else {
            close();
        }
        return std::nullopt;
    }
    if ((chord.key == Key::Enter || chord.key == Key::KeypadEnter) &&
        chord.mods == Modifiers::None) {
        return activate_selected();
    }
    if (chord.key == Key::Down && chord.mods == Modifiers::None && count > 0) {
        selected_ = selected_ ? ((*selected_ + 1) % count) : std::size_t{0};
        ensure_visible();
        return std::nullopt;
    }
    if (chord.key == Key::Up && chord.mods == Modifiers::None && count > 0) {
        selected_ = selected_ ? ((*selected_ == 0 ? count - 1 : *selected_ - 1)) : (count - 1);
        ensure_visible();
        return std::nullopt;
    }
    if (chord.key == Key::PageDown && chord.mods == Modifiers::None && count > 0) {
        const std::size_t base = selected_.value_or(0);
        selected_ = std::min(count - 1, base + kVisibleRows);
        ensure_visible();
        return std::nullopt;
    }
    if (chord.key == Key::PageUp && chord.mods == Modifiers::None && count > 0) {
        const std::size_t base = selected_.value_or(0);
        selected_ = base > kVisibleRows ? base - kVisibleRows : 0;
        ensure_visible();
        return std::nullopt;
    }
    return std::nullopt;
}

std::optional<PaletteAction> PaletteOverlay::draw(const Painter& p, const Palette& pal,
                                                  const RectF& viewport,
                                                  const PointerInput& pointer,
                                                  const palette::History& history) {
    if (!open_) {
        return std::nullopt;
    }
    Renderer& r = p.renderer();

    // Dim backdrop.
    r.fill_rect(viewport, pal.shadow.with_alpha(0.45F));

    const std::size_t count =
        (mode_ == Mode::Commands) ? results_.hits.size() : filtered_picker_.size();
    const std::size_t rows_shown = std::min(count, kVisibleRows);
    const float card_w = std::min(p.px(kCardWidth), viewport.w - p.px(32.0F));
    const float field_h = p.px(kFieldHeight);
    const float row_h = p.px(kRowHeight);
    const float footer_h = p.px(kFooterHeight);
    const float list_h =
        rows_shown > 0 ? static_cast<float>(rows_shown) * row_h + p.px(8.0F) : p.px(44.0F);
    const float card_h = field_h + list_h + footer_h;
    const float card_x = std::floor(viewport.x + (viewport.w - card_w) * 0.5F);
    const float card_y = std::floor(viewport.y + std::min(p.px(72.0F), viewport.h * 0.12F));
    const RectF card{card_x, card_y, card_w, card_h};

    // Click outside backdrop closes the palette.
    if (pointer.pressed && !contains(card, pointer.x, pointer.y)) {
        close();
        return std::nullopt;
    }

    // Wheel scrolling.
    if (contains(card, pointer.x, pointer.y) && pointer.wheel_y != 0.0F && count > kVisibleRows) {
        const auto max_scroll = count - kVisibleRows;
        if (pointer.wheel_y < 0.0F) {
            scroll_ = std::min(max_scroll, scroll_ + 1);
        } else if (scroll_ > 0) {
            --scroll_;
        }
    }

    // Card shadow + surface + border.
    const float rad = p.px(radius::lg);
    r.drop_shadow(card, rad, p.px(18.0F), p.px(6.0F), pal.shadow.with_alpha(0.35F));
    r.fill_rounded_rect(card, rad, pal.surface_raised);
    r.stroke_rounded_rect(card, rad, p.px(1.0F), pal.border);

    // Search field.
    const std::string_view placeholder =
        mode_ == Mode::PickOp ? tr("palette.pick_op")
                              : (mode_ == Mode::PickBookmark ? tr("palette.pick_bookmark")
                                                             : tr("palette.placeholder"));
    const RectF field_box{card.x + p.px(14.0F), card.y + p.px(4.0F), card.w - p.px(28.0F),
                          field_h - p.px(8.0F)};
    draw_text_field(p, pal, query_, field_box, placeholder, field_scroll_);
    r.fill_rect(RectF{card.x, card.y + field_h, card.w, p.px(1.0F)}, pal.border);

    // Rows.
    std::optional<PaletteAction> clicked_action;
    const float list_top = card.y + field_h + p.px(4.0F);
    if (count == 0) {
        const ShapedText empty_t = p.shape(type::body, tr("palette.empty"));
        p.draw(empty_t, card.x + p.px(16.0F), list_top + p.px(24.0F), pal.text_muted);
    } else {
        for (std::size_t vis = 0; vis < rows_shown; ++vis) {
            const std::size_t idx = scroll_ + vis;
            if (idx >= count) {
                break;
            }
            const RectF row_rect{card.x + p.px(6.0F), list_top + static_cast<float>(vis) * row_h,
                                 card.w - p.px(12.0F), row_h};
            const bool hovered = contains(row_rect, pointer.x, pointer.y);
            const bool is_sel = selected_.has_value() && *selected_ == idx;
            if (is_sel) {
                r.fill_rounded_rect(row_rect, p.px(radius::sm), pal.selection);
            } else if (hovered) {
                r.fill_rounded_rect(row_rect, p.px(radius::sm), pal.row_hover);
            }
            if (hovered && pointer.pressed) {
                selected_ = idx;
                clicked_action = activate_selected();
            }

            const float baseline = row_rect.y + row_h * 0.65F;
            if (mode_ == Mode::Commands) {
                const palette::Hit& hit = results_.hits[idx];
                const palette::Entry& e = entries_[hit.entry];
                const Color title_color =
                    (e.danger == Danger::Destructive) ? pal.danger : pal.text_primary;

                // Right-aligned shortcut chip.
                float right_edge = row_rect.x + row_rect.w - p.px(10.0F);
                if (!e.shortcut.empty()) {
                    const ShapedText sc = p.shape(type::caption, e.shortcut);
                    const float chip_w = sc.width() + p.px(12.0F);
                    const float chip_h = p.px(20.0F);
                    const RectF chip_r{right_edge - chip_w, row_rect.y + (row_h - chip_h) * 0.5F,
                                       chip_w, chip_h};
                    r.fill_rounded_rect(chip_r, p.px(4.0F), pal.surface);
                    r.stroke_rounded_rect(chip_r, p.px(4.0F), p.px(1.0F), pal.keycap_border);
                    p.draw(sc, chip_r.x + p.px(6.0F), chip_r.y + chip_h * 0.72F, pal.keycap_text);
                    right_edge = chip_r.x - p.px(8.0F);
                }

                // Alias badge when matched via alias.
                if (hit.via == palette::MatchedVia::Alias && hit.alias < e.aliases.size()) {
                    const std::string badge = ":" + e.aliases[hit.alias];
                    const ShapedText bt = p.shape(type::caption, badge);
                    const float bw = bt.width() + p.px(10.0F);
                    const float bh = p.px(18.0F);
                    const RectF br{right_edge - bw, row_rect.y + (row_h - bh) * 0.5F, bw, bh};
                    r.fill_rounded_rect(br, p.px(4.0F), pal.chip);
                    p.draw(bt, br.x + p.px(5.0F), br.y + bh * 0.75F, pal.chip_text);
                    right_edge = br.x - p.px(8.0F);
                }

                // Category label (muted) + Title (with match highlights).
                float x = row_rect.x + p.px(10.0F);
                if (!e.category.empty()) {
                    const std::string cat = e.category + ": ";
                    const ShapedText ct = p.shape(type::body, cat);
                    p.draw(ct, x, baseline, pal.text_muted);
                    x += ct.width();
                }
                const float avail = std::max(0.0F, right_edge - x);
                const std::string fitted =
                    TextFitter::elide(p, type::body, e.title, avail, Elide::Middle);
                const ShapedText tt = p.shape(type::body, fitted);
                if (!hit.highlights.empty() && fitted == e.title) {
                    std::vector<std::pair<std::uint32_t, std::uint32_t>> ranges;
                    ranges.reserve(hit.highlights.size());
                    for (const search::HighlightRange& hr : hit.highlights) {
                        ranges.emplace_back(hr.begin, hr.end);
                    }
                    p.draw_highlighted(tt, x, baseline, title_color, pal.match, pal.match, ranges,
                                       e.title.size());
                } else {
                    p.draw(tt, x, baseline, title_color);
                }
            } else {
                const auto& src = (mode_ == Mode::PickOp) ? running_ops_ : bookmarks_;
                const PickerOption& opt = src[filtered_picker_[idx]];
                const ShapedText lt = p.shape(type::body, opt.label);
                p.draw(lt, row_rect.x + p.px(10.0F), baseline, pal.text_primary);
                if (!opt.detail.empty()) {
                    const ShapedText dt = p.shape(type::caption, opt.detail);
                    p.draw(dt, row_rect.x + row_rect.w - dt.width() - p.px(10.0F), baseline,
                           pal.text_muted);
                }
            }
        }
    }

    // Footer bar.
    const float footer_y = card.y + card.h - footer_h;
    r.fill_rect(RectF{card.x, footer_y, card.w, p.px(1.0F)}, pal.border);
    const std::string count_str = std::to_string(count);
    const ShapedText ft = p.shape(type::caption, count_str);
    p.draw(ft, card.x + card.w - ft.width() - p.px(12.0F), footer_y + footer_h * 0.68F,
           pal.text_muted);
    (void)history;

    return clicked_action;
}

} // namespace navigator::ui
