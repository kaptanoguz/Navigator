// src/ui/browser/browser_search_view.cpp — Phase 5 parts of BrowserView (see
// browser_view.hpp): the title bar's filter field, flatten and types toggles, the progress bar
// under the title bar, the filtered / flattened empty states, the status-bar search summary
// and the match highlights of the Name column.
#include <algorithm>
#include <cmath>
#include <numbers>
#include <string>

#include <navigator/i18n.hpp>

#include "browser/browser_view.hpp"
#include "browser/format.hpp"
#include "i18n/catalog.hpp"
#include "input/sdl_input.hpp"
#include "search/filter_rules.hpp"
#include "search/highlight_map.hpp"
#include "search/index_status.hpp"
#include "widgets/icons.hpp"
#include "widgets/text_field.hpp"

namespace navigator::ui {
namespace {

constexpr std::size_t kHighlightCacheMax = 4096;

Color over(const Color& base, const Color& overlay) {
    const float a = overlay.a;
    return Color{(base.r * (1.0F - a)) + (overlay.r * a), (base.g * (1.0F - a)) + (overlay.g * a),
                 (base.b * (1.0F - a)) + (overlay.b * a), 1.0F};
}

std::string count_of(std::size_t shown, std::size_t total, std::string_view key) {
    return substitute(substitute(tr(key), "shown", format_count(shown)), "total",
                      format_count(total));
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Title bar
// ---------------------------------------------------------------------------------------------

bool BrowserView::toggle_button(const BrowserFrame& f, const RectF& rect, WidgetId id, int icon,
                                bool on, std::string_view badge) {
    const Painter& p = f.painter;
    Renderer& r = p.renderer();
    const Palette& pal = f.palette;
    const PointerInput& ptr = f.pointer;
    const bool inside = contains(rect, ptr.x, ptr.y);
    if (inside && ptr.pressed && active_ == WidgetId::None) {
        active_ = id;
    }
    const bool hover = inside && (active_ == WidgetId::None || active_ == id);
    const bool fired = active_ == id && ptr.released && inside;
    const float rad = p.px(radius::md);
    if (on) {
        // A toggled state reads as a filled accent chip, not just a coloured glyph.
        r.fill_rounded_rect(rect, rad, pal.chip);
        if (hover) {
            r.fill_rounded_rect(rect, rad, active_ == id ? pal.control_pressed : pal.control_hover);
        }
    } else if (hover) {
        r.fill_rounded_rect(rect, rad, active_ == id ? pal.control_pressed : pal.control_hover);
    }
    const Color ink = on ? pal.chip_text : (hover ? pal.text_primary : pal.text_secondary);
    const float sz = std::round(p.px(15.0F));
    const float t = std::max(1.0F, p.px(1.35F));
    const float ix =
        badge.empty() ? std::round(rect.center_x() - (0.5F * sz)) : std::round(rect.x + p.px(7.5F));
    const LineIcon glyph = icon == 0   ? LineIcon::Funnel
                           : icon == 2 ? LineIcon::Drive // the whole-drive variant of flatten
                                       : LineIcon::Flatten;
    draw_line_icon(r, glyph, ix, std::round(rect.center_y() - (0.5F * sz)), sz, t, ink,
                   pal.background);
    if (!badge.empty()) {
        const ShapedText b = p.shape(type::keycap, badge);
        p.draw(b, ix + sz + std::round(p.px(4.0F)),
               std::round(rect.center_y() + (0.5F * b.cap_height())), ink);
    }
    return fired;
}

void BrowserView::draw_filter_field(const BrowserFrame& f, const RectF& area, bool collapsed) {
    const Painter& p = f.painter;
    Renderer& r = p.renderer();
    const Palette& pal = f.palette;
    const PointerInput& ptr = f.pointer;
    const BrowserModel& m = f.model;
    const TextEdit& edit = m.filter_edit();
    const bool has_text = !edit.empty();
    const RectF box = snap(area);
    const float rad = std::round(p.px(radius::md - 1.0F));
    const float hair = std::max(1.0F, std::round(p.px(1.0F)));
    const bool inside = contains(box, ptr.x, ptr.y);

    if (collapsed) {
        // Narrow panes: a magnifier button (design review Phase 4 notes, 140dp rule).
        if (inside && ptr.pressed && active_ == WidgetId::None) {
            active_ = WidgetId::FilterField;
        }
        if (active_ == WidgetId::FilterField && ptr.released && inside) {
            f.queue.enqueue(CommandId::FocusFilter);
        }
        if (inside && (active_ == WidgetId::None || active_ == WidgetId::FilterField)) {
            r.fill_rounded_rect(box, p.px(radius::md),
                                active_ == WidgetId::FilterField ? pal.control_pressed
                                                                 : pal.control_hover);
        }
        draw_search_glyph(r, box.center_x() - p.px(1.0F), box.center_y() - p.px(1.0F), p.px(5.0F),
                          std::max(1.0F, p.px(1.5F)),
                          inside ? pal.text_primary : pal.text_secondary, pal.background);
        return;
    }

    // Field: the address bar's styling (accent ring when focused) so the two read as siblings.
    const bool focused = f.filter_focused;
    const Color fill =
        over(pal.background, pal.surface.with_alpha(focused || has_text ? 1.0F : 0.72F));
    r.fill_rounded_rect(box, rad, fill);
    if (focused) {
        r.stroke_rounded_rect(box, rad, std::max(1.0F, std::round(p.px(1.5F))), pal.focus_ring);
    } else if (has_text) {
        r.stroke_rounded_rect(box, rad, hair, pal.accent.with_alpha(0.55F));
    } else {
        r.stroke_rounded_rect(box, rad, hair,
                              inside ? pal.keycap_border : pal.keycap_border.with_alpha(0.55F));
    }
    const float mid = box.center_y();
    float glyph_x = box.x + std::round(p.px(13.0F));
    const SearchScope scope = search_scope(m.flattened(), m.flatten_is_volume());
    if (scope == SearchScope::Folder) {
        draw_search_glyph(r, glyph_x, mid - p.px(0.5F), p.px(4.6F), std::max(1.0F, p.px(1.4F)),
                          has_text || focused ? pal.accent : pal.text_muted, fill);
    } else {
        // Scope chip (design review B1): stays while typing. Drive is a solid accent pill with
        // the drive glyph, subfolders the quiet translucent chip; narrow fields keep the glyph.
        const bool drive = scope == SearchScope::Drive;
        const ShapedText label = p.shape(type::keycap, tr(scope_label_key(scope)));
        const float ch = std::round(p.px(20.0F));
        const float icon = std::round(p.px(12.0F));
        const bool with_label = box.w >= p.px(200.0F);
        const float cw =
            std::round(icon + p.px(12.0F) + (with_label ? label.width() + p.px(5.0F) : 0.0F));
        const RectF chip = snap(RectF{box.x + std::round(p.px(4.0F)), mid - (0.5F * ch), cw, ch});
        r.fill_rounded_rect(chip, p.px(radius::sm), drive ? pal.accent : pal.chip);
        const Color ink = drive ? pal.on_accent : pal.chip_text;
        draw_line_icon(r, drive ? LineIcon::Drive : LineIcon::Flatten,
                       chip.x + std::round(p.px(6.0F)), std::round(mid - (0.5F * icon)), icon,
                       std::max(1.0F, p.px(1.2F)), ink, drive ? pal.accent : fill);
        if (with_label) {
            p.draw(label, chip.x + std::round(p.px(6.0F)) + icon + std::round(p.px(5.0F)),
                   std::round(mid + (0.5F * label.cap_height())), ink);
        }
        glyph_x = chip.right() + std::round(p.px(-3.0F));
    }

    // Right-hand contents, right to left.
    float right = box.right() - std::round(p.px(5.0F));
    const float btn = std::round(p.px(20.0F));
    bool over_widget = false;
    if (has_text) {
        // Clear (x) button: list.clear_filter.
        const RectF clear{right - btn, mid - (0.5F * btn), btn, btn};
        const bool in_clear = contains(clear, ptr.x, ptr.y);
        over_widget = over_widget || in_clear;
        if (in_clear && ptr.pressed && active_ == WidgetId::None) {
            active_ = WidgetId::FilterClear;
        }
        if (active_ == WidgetId::FilterClear && ptr.released && in_clear) {
            f.queue.enqueue(CommandId::ClearFilter);
        }
        if (in_clear && (active_ == WidgetId::None || active_ == WidgetId::FilterClear)) {
            r.fill_rounded_rect(snap(clear), 0.5F * clear.w, pal.control_pressed);
        }
        draw_cross(r, clear.center_x(), mid, p.px(3.4F), std::max(1.0F, p.px(1.3F)),
                   in_clear ? pal.text_primary : pal.text_secondary);
        right = clear.x - std::round(p.px(3.0F));

        // Count chip: "12 of 3,480" (design review: accent chip with the count).
        const std::size_t shown = m.row_count();
        const std::size_t total = m.source_count();
        std::string chip_text;
        if (m.rows_pending()) {
            chip_text = "\xE2\x80\xA6";
        } else if (m.flattened() && shown == total && m.flatten_progress().from_index) {
            chip_text = format_count(shown); // index answers count matches: "133", not "133 of 133"
        } else {
            chip_text = count_of(shown, total, "filter.count");
        }
        ShapedText ct = p.shape(type::keycap, chip_text);
        const float chip_pad = p.px(14.0F);
        // The query stays readable: a minimum-width field shows just the match count (the
        // status bar has "12 of 3,480").
        const float query_room =
            std::min(p.shape(type::row, edit.text()).width() + p.px(4.0F), p.px(72.0F));
        if (right - (glyph_x + p.px(15.0F)) - (ct.width() + chip_pad) < query_room) {
            ct = p.shape(type::keycap, format_count(shown));
        }
        const float ch = std::round(p.px(18.0F));
        const float cw = std::round(ct.width() + chip_pad);
        if (cw < box.w * 0.48F) {
            const RectF chip = snap(RectF{right - cw, mid - (0.5F * ch), cw, ch});
            const bool none = shown == 0 && !m.rows_pending();
            r.fill_rounded_rect(chip, 0.5F * ch, none ? pal.danger.with_alpha(0.14F) : pal.chip);
            p.draw(ct, chip.x + std::round(p.px(7.0F)),
                   std::round(chip.center_y() + (0.5F * ct.cap_height())),
                   none ? pal.danger : pal.chip_text);
            right = chip.x - std::round(p.px(4.0F));
        }
    }
    if (has_text || focused) {
        // Match mode toggle (search.toggle_fuzzy): "≈" lit while fuzzy.
        const bool fuzzy = m.match_mode() == search::MatchMode::Fuzzy;
        const RectF mode{right - btn, mid - (0.5F * btn), btn, btn};
        if (mode.x > box.x + p.px(80.0F)) {
            const bool in_mode = contains(mode, ptr.x, ptr.y);
            over_widget = over_widget || in_mode;
            if (in_mode && ptr.pressed && active_ == WidgetId::None) {
                active_ = WidgetId::FilterMode;
            }
            if (active_ == WidgetId::FilterMode && ptr.released && in_mode) {
                f.queue.enqueue(CommandId::ToggleFuzzy);
            }
            const RectF mr = snap(mode);
            if (fuzzy) {
                r.fill_rounded_rect(mr, p.px(radius::sm), pal.chip);
            }
            if (in_mode) {
                r.fill_rounded_rect(mr, p.px(radius::sm), pal.control_hover);
                // The mode in words on hover (design review O6), drawn after the list.
                tooltip_text_ = std::string(tr(fuzzy ? "filter.fuzzy_on" : "filter.fuzzy_off"));
                tooltip_anchor_ = mr;
            }
            const ShapedText g = p.shape(type::numeric, "\xE2\x89\x88"); // U+2248
            p.draw(g, std::round(mr.center_x() - (0.5F * g.width())),
                   std::round(mr.center_y() + (0.5F * g.cap_height())),
                   fuzzy ? pal.chip_text : (in_mode ? pal.text_primary : pal.text_secondary));
            right = mode.x - std::round(p.px(3.0F));
        }
    } else if (box.w >= p.px(170.0F)) {
        // Idle: the chord that focuses it, faint (it is also type-to-filter).
        const std::vector<KeyChord> chords = f.keymap.chords_for("list.filter");
        if (!chords.empty()) {
            std::string legend;
            for (const std::string& part : chord_labels(chords.front())) {
                legend += legend.empty() ? part : "+" + part;
            }
            const ShapedText k = p.shape(type::caption, legend);
            p.draw(k, right - std::round(p.px(5.0F)) - k.width(),
                   std::round(mid + (0.5F * k.cap_height())), pal.text_muted.with_alpha(0.8F));
            right -= k.width() + std::round(p.px(10.0F));
        }
    }

    // Press anywhere else in the field focuses it (list.filter).
    if (inside && !over_widget && ptr.pressed && active_ == WidgetId::None) {
        active_ = WidgetId::FilterField;
    }
    if (active_ == WidgetId::FilterField && ptr.released && inside && !focused) {
        f.queue.enqueue(CommandId::FocusFilter);
    }

    // The scope chip names what is searched; the placeholder only invites typing.
    const std::string_view placeholder =
        m.flattened() ? std::string_view("filter.placeholder_search") : "filter.placeholder";
    TextFieldStyle st;
    st.text = &type::row;
    st.caret_height_dp = 15.0F;
    st.show_caret = focused;
    const float tx = glyph_x + std::round(p.px(11.0F));
    (void)draw_text_field(p, pal, edit, RectF{tx, box.y, std::max(right - tx, 1.0F), box.h},
                          tr(placeholder), filter_scroll_, st);
}

// ---------------------------------------------------------------------------------------------
// Progress bar and spinner
// ---------------------------------------------------------------------------------------------

void BrowserView::draw_spinner(const BrowserFrame& f, float cx, float cy, float radius) const {
    Renderer& r = f.painter.renderer();
    constexpr int kDots = 8;
    const double phase = std::fmod(spin_ms_ / 900.0, 1.0);
    const int lead = static_cast<int>(phase * kDots);
    const float dot = std::max(1.0F, f.painter.px(1.25F));
    for (int i = 0; i < kDots; ++i) {
        const float a = (static_cast<float>(i) / kDots) * 2.0F * std::numbers::pi_v<float>;
        const int age = (lead - i + kDots) % kDots;
        const float alpha = 1.0F - (static_cast<float>(age) / kDots) * 0.82F;
        r.fill_circle(cx + (std::sin(a) * radius), cy - (std::cos(a) * radius), dot,
                      f.palette.accent.with_alpha(alpha));
    }
}

void BrowserView::draw_progress(const BrowserFrame& f, const Layout& l) const {
    const BrowserModel& m = f.model;
    if (!m.flattened()) {
        return;
    }
    const FlattenProgress prog = m.flatten_progress();
    // Walks and index queries are indeterminate; an index-backed view only queries a ready
    // index, so the bar never shows the index's own scan (one engine at a time).
    const bool running = prog.walking || prog.organizing;
    if (!running) {
        return;
    }
    const Painter& p = f.painter;
    Renderer& r = p.renderer();
    const float rad = std::round(p.px(radius::lg - 2.0F));
    const float h = std::max(2.0F, std::round(p.px(2.0F)));
    const RectF track = snap(RectF{l.panel.x + rad, l.panel.y + std::max(1.0F, p.px(1.0F)),
                                   l.panel.w - (2.0F * rad), h});
    r.fill_rounded_rect(track, 0.5F * h, f.palette.accent.with_alpha(0.14F));
    // Indeterminate: a segment eases across the track (never modal: the list stays usable).
    // Without animations the segment rests in view.
    const double t = f.animations ? std::fmod(spin_ms_ / 1300.0, 1.0) : 0.45;
    const double eased = 0.5 - (0.5 * std::cos(t * std::numbers::pi));
    const float seg = track.w * 0.28F;
    const float x0 = track.x - seg + static_cast<float>(eased) * (track.w + seg);
    const float a = std::max(x0, track.x);
    const float b = std::min(x0 + seg, track.right());
    if (b > a) {
        r.fill_rounded_rect(RectF{a, track.y, b - a, h}, 0.5F * h, f.palette.accent);
    }
}

// ---------------------------------------------------------------------------------------------
// Empty states
// ---------------------------------------------------------------------------------------------

void BrowserView::draw_search_empty(const BrowserFrame& f, const Layout& l) {
    const BrowserModel& m = f.model;
    const Painter& p = f.painter;
    const Palette& pal = f.palette;
    std::string title;
    std::string detail;
    std::string_view hint_cmd;
    std::string_view hint_key;
    bool spinner = false;
    const std::string query = display_name(m.filter_edit().text());
    const auto no_matches = [&] {
        if (m.filter_active()) {
            title = substitute(tr("filter.no_matches"), "query", query);
            if (m.type_filter().active()) {
                detail = std::string(tr("filter.no_matches_types"));
            }
            hint_cmd = "list.clear_selection";
            hint_key = "hint.clear_filter";
        } else {
            title = std::string(tr("filter.all_types_hidden"));
            hint_cmd = "search.types";
            hint_key = "hint.show_types";
        }
    };
    if (m.flattened()) {
        const FlattenProgress prog = m.flatten_progress();
        if (prog.error && prog.found == 0) {
            title = std::string(tr("flatten.unreadable"));
            detail = std::string(tr("error." + std::string(to_string(prog.error->code))));
        } else if ((prog.walking || prog.organizing) && m.row_count() == 0 &&
                   (prog.found == 0 || m.rows_pending())) {
            title = std::string(tr(prog.from_index ? "flatten.querying" : "flatten.searching"));
            spinner = true;
        } else if (prog.found == 0 && !m.filter_active()) {
            title = std::string(tr("flatten.empty"));
            hint_cmd = "search.flatten";
            hint_key = "hint.leave_flatten";
        } else {
            no_matches();
        }
    } else if (m.rows_pending()) {
        title = std::string(tr("filter.filtering"));
    } else {
        no_matches();
    }
    const float cx = l.body.x + (0.5F * l.body.w);
    const float cy = l.body.y + (l.body.h * 0.42F);
    float baseline = std::round(cy);
    if (spinner) {
        draw_spinner(f, cx, baseline - p.px(34.0F), p.px(8.0F));
    }
    const ShapedText t =
        fitter_.fit(p, type::empty_title, title, l.body.w - p.px(48.0F), Elide::Middle);
    p.draw(t, std::round(cx - (0.5F * t.width())), baseline, pal.text_secondary);
    if (!detail.empty()) {
        const ShapedText d = fitter_.fit(p, type::row, detail, l.body.w - p.px(48.0F), Elide::End);
        baseline += std::round(p.px(22.0F));
        p.draw(d, std::round(cx - (0.5F * d.width())), baseline, pal.text_muted);
    }
    if (!hint_cmd.empty()) {
        draw_key_hint(f, hint_cmd, tr(hint_key), cx, baseline + std::round(p.px(30.0F)), false);
    }
}

// ---------------------------------------------------------------------------------------------
// Status bar
// ---------------------------------------------------------------------------------------------

float BrowserView::draw_search_status(const BrowserFrame& f, const Layout& l, float x,
                                      float limit) {
    const Painter& p = f.painter;
    Renderer& r = p.renderer();
    const Palette& pal = f.palette;
    const BrowserModel& m = f.model;
    const float mid = l.status.y + (0.5F * l.status.h);
    const float start = x;
    const ShapedText sep = p.shape(type::status, "  \xC2\xB7  ");
    bool first = true;
    // Parts come in priority order: what does not fit before the selection summary (or the
    // Stop button) is left out; only the first part elides.
    const auto part = [&](const std::string& text, const Color& ink) {
        ShapedText s = p.shape(type::status, text);
        if (x + (first ? 0.0F : sep.width()) + s.width() > limit) {
            if (!first || limit - x < p.px(24.0F)) {
                return;
            }
            s = fitter_.fit(p, type::status, text, limit - x, Elide::End);
        }
        if (!first) {
            p.draw(sep, x, std::round(mid + (0.5F * sep.cap_height())), pal.text_muted);
            x += sep.width();
        }
        first = false;
        p.draw(s, x, std::round(mid + (0.5F * s.cap_height())), ink);
        x += s.width();
    };
    const bool filtering = m.filter_active() || m.type_filter().active();
    const auto types_part = [&] {
        const search::TypeFilter& tf = m.type_filter();
        if (tf.active()) {
            const std::size_t n = (tf.hide_folders ? 1U : 0U) + (tf.hide_no_extension ? 1U : 0U) +
                                  tf.hidden_extensions.size();
            part(tr_count("status.types_hidden", n), pal.text_secondary);
        }
    };
    if (!m.flattened()) {
        part(m.rows_pending() ? std::string(tr("filter.filtering"))
                              : count_of(m.row_count(), m.source_count(), "status.filtered"),
             pal.text_primary);
        types_part();
        if (!m.show_hidden() && m.hidden_count() > 0) {
            // Next to "1 type hidden": say which kind of hidden (design review O7).
            part(tr_count("status.hidden_files", m.hidden_count()), pal.text_secondary);
        }
        return x - start;
    }

    const FlattenProgress prog = m.flatten_progress();
    const bool running = prog.walking || (prog.from_index && prog.organizing);
    const ShapedText stop_label = p.shape(type::status, tr("status.stop"));
    const float button_gap = std::round(p.px(12.0F));
    float stop_w = std::round(stop_label.width() + p.px(28.0F));
    const bool stop_icon_only = limit - x < p.px(190.0F);
    if (stop_icon_only) {
        stop_w = std::round(p.px(20.0F));
    }
    if (running) {
        draw_spinner(f, x + p.px(6.0F), mid, p.px(4.5F));
        x += std::round(p.px(18.0F));
        limit -= stop_w + button_gap; // the Stop button always fits
    }
    if (prog.from_index) {
        part(prog.limited ? count_of(m.row_count(), prog.found, "status.filtered")
                          : tr_count("status.index_matches", prog.found),
             pal.text_primary);
    } else {
        part(filtering ? count_of(m.row_count(), prog.found, "status.filtered")
                       : tr_count("status.found", prog.found),
             pal.text_primary);
    }
    // How a search ended outranks the details.
    if (prog.error && prog.found > 0) {
        part(std::string(tr("status.search_error")), pal.danger);
    } else if (prog.truncated) {
        part(std::string(tr("status.search_limit")), pal.warning);
    } else if (prog.cancelled) {
        part(std::string(tr("status.search_stopped")), pal.text_secondary);
    }
    // Only an index-backed view talks about the index; a walk talks about folders (design
    // review O2: one engine at a time).
    if (prog.from_index && f.index_status) {
        const IndexStatusView iv = present_index_status(*f.index_status);
        const Color ink = iv.tone == IndexTone::Problem ? pal.danger
                          : iv.tone == IndexTone::Ready ? pal.text_secondary
                                                        : pal.chip_text;
        part(iv.text, ink);
    }
    if (!prog.from_index) {
        part(tr_count("status.folders", prog.stats.dirs_visited), pal.text_secondary);
    }
    if (prog.skipped > 0) {
        part(tr_count("status.skipped", prog.skipped), pal.warning);
    }
    types_part();
    if (running) {
        const std::string_view key = prog.from_index ? "status.querying"
                                     : prog.volume   ? "status.listing_drive"
                                                     : "status.searching";
        part(std::string(tr(key)), pal.text_secondary);
    }
    // Stop button (search.cancel) while something runs.
    if (running) {
        const ShapedText& label = stop_label;
        const float bh = std::round(p.px(20.0F));
        const RectF b = snap(RectF{x + button_gap, mid - (0.5F * bh), stop_w, bh});
        const PointerInput& ptr = f.pointer;
        const bool inside = contains(b, ptr.x, ptr.y);
        if (inside && ptr.pressed && active_ == WidgetId::None) {
            active_ = WidgetId::StopSearch;
        }
        if (active_ == WidgetId::StopSearch && ptr.released && inside) {
            f.queue.enqueue(CommandId::CancelSearch);
        }
        r.stroke_rounded_rect(b, 0.5F * bh, std::max(1.0F, std::round(p.px(1.0F))),
                              pal.keycap_border);
        if (inside) {
            r.fill_rounded_rect(b, 0.5F * bh,
                                active_ == WidgetId::StopSearch ? pal.control_pressed
                                                                : pal.control_hover);
        }
        const float icon = std::round(p.px(10.0F));
        draw_line_icon(r, LineIcon::Stop,
                       stop_icon_only ? std::round(b.center_x() - (0.5F * icon))
                                      : b.x + std::round(p.px(8.0F)),
                       std::round(b.center_y() - (0.5F * icon)), icon, 1.0F,
                       inside ? pal.text_primary : pal.text_secondary, pal.background);
        if (!stop_icon_only) {
            p.draw(label, b.x + std::round(p.px(20.0F)),
                   std::round(b.center_y() + (0.5F * label.cap_height())),
                   inside ? pal.text_primary : pal.text_secondary);
        }
        x = b.right();
    }
    return x - start;
}

// ---------------------------------------------------------------------------------------------
// Rows
// ---------------------------------------------------------------------------------------------

std::vector<std::pair<std::uint32_t, std::uint32_t>>
BrowserView::name_highlights(const BrowserFrame& f, const std::string& raw,
                             const std::string& display, const std::string& shown) {
    const BrowserModel& m = f.model;
    if (hl_gen_ != m.filter_generation()) {
        hl_gen_ = m.filter_generation();
        hl_matcher_ = search::Matcher(m.query());
        hl_cache_.clear();
    }
    auto it = hl_cache_.find(raw);
    if (it == hl_cache_.end()) {
        if (hl_cache_.size() >= kHighlightCacheMax) {
            hl_cache_.clear();
        }
        std::optional<search::Match> mm = hl_matcher_.match(raw);
        it = hl_cache_
                 .emplace(raw,
                          mm ? std::move(mm->highlights) : std::vector<search::HighlightRange>{})
                 .first;
    }
    return to_shown_ranges(display, shown, to_display_ranges(raw, display, it->second));
}

std::string BrowserView::folder_text(const BrowserFrame& f, std::size_t row) const {
    // Subfolder searches: the folder relative to the root, empty for rows directly in it, so
    // the subfolder paths stand out (design review O4). Drive searches: absolute, with "~" for
    // the home folder (O3). Copy path and activation use the full path (row_path).
    const BrowserModel& m = f.model;
    const std::string_view parent = m.row_parent(row);
    if (!m.flatten_is_volume()) {
        return parent.empty() ? std::string() : display_name(parent);
    }
    Path folder = m.flatten_root();
    if (!parent.empty()) {
        folder /= Path(std::string(parent));
    }
    if (f.home != nullptr && !f.home->empty() && *f.home != f.home->root_path()) {
        const Path rel = folder.lexically_relative(*f.home);
        if (!rel.empty() && *rel.begin() != "..") {
            return rel == "." ? std::string("~") : "~/" + display_name(rel.string());
        }
    }
    return display_name(folder.string());
}

void BrowserView::draw_tooltip(const BrowserFrame& f) {
    if (tooltip_text_.empty()) {
        return;
    }
    const Painter& p = f.painter;
    Renderer& r = p.renderer();
    const ShapedText t = p.shape(type::caption, tooltip_text_);
    const float h = std::round(p.px(26.0F));
    const float w = std::round(t.width() + p.px(20.0F));
    float x = std::round(tooltip_anchor_.center_x() - (0.5F * w));
    x = std::clamp(x, f.bounds.x + p.px(4.0F), f.bounds.right() - w - p.px(4.0F));
    const RectF tip = snap(RectF{x, std::round(tooltip_anchor_.bottom() + p.px(8.0F)), w, h});
    const float rad = std::round(p.px(radius::md - 2.0F));
    r.drop_shadow(tip, rad, p.px(12.0F), p.px(3.0F), f.palette.shadow);
    r.fill_rounded_rect(tip, rad, f.palette.surface_raised);
    r.stroke_rounded_rect(tip, rad, std::max(1.0F, std::round(p.px(1.0F))), f.palette.border);
    p.draw(t, tip.x + std::round(p.px(10.0F)), std::round(tip.center_y() + (0.5F * t.cap_height())),
           f.palette.text_primary);
    tooltip_text_.clear();
}

} // namespace navigator::ui
